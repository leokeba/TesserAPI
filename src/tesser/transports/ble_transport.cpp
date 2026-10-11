#include "tesser/transports/ble_transport.h"

#include "tesser/api.h"

#if defined(ESP_PLATFORM) && !defined(ARDUINO) && defined(CONFIG_BT_NIMBLE_ENABLED)

#include <stdio.h>
#include <string.h>

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace tesser {

namespace {

// The Nordic UART Service, little-endian: 6e40000X-b5a3-f393-e0a9-e50e24dcca9e.
#define TESSER_NUS(x) BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, x, 0x00, 0x40, 0x6e)
ble_uuid128_t kService = TESSER_NUS(0x01);
ble_uuid128_t kRx = TESSER_NUS(0x02);
ble_uuid128_t kTx = TESSER_NUS(0x03);

// NimBLE keeps pointers to these: filled in begin(), field by field, since
// the structures gain fields across ESP-IDF versions.
ble_gatt_chr_def chrs[3];
ble_gatt_svc_def svcs[2];

BleTransport* instance = nullptr;

void hostTask(void*) {
    nimble_port_run();  // returns when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

}  // namespace

BleTransport::BleTransport(Api& api)
    : api_(api), line_(api, [this](const char* data, size_t len) { write(data, len); }) {
    // Anyone in range can connect: unlike a serial line, a connection proves
    // nothing (docs/DESIGN.md section 13.2).
    line_.setAuthenticated(false);
}

esp_err_t BleTransport::begin(const char* name, uint32_t stackSize, UBaseType_t priority) {
    if (instance) return ESP_ERR_INVALID_STATE;
    instance = this;
    {
        MutexGuard guard(stateMutex_);
        name_ = name;
    }
    if (!confirmed_) confirmed_ = xSemaphoreCreateBinary();
    if (!confirmed_) return ESP_ERR_NO_MEM;
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        instance = nullptr;
        return err;
    }

    ble_hs_cfg.sync_cb = &BleTransport::onSync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(name_.c_str());
    ble_att_set_preferred_mtu(517);

    memset(chrs, 0, sizeof(chrs));
    chrs[0].uuid = &kRx.u;
    chrs[0].access_cb = &BleTransport::access;
    chrs[0].flags = static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP);
    chrs[1].uuid = &kTx.u;
    chrs[1].access_cb = &BleTransport::access;
    chrs[1].flags = static_cast<ble_gatt_chr_flags>(BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_INDICATE);
    chrs[1].val_handle = &txHandle_;
    memset(svcs, 0, sizeof(svcs));
    svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    svcs[0].uuid = &kService.u;
    svcs[0].characteristics = chrs;
    if (ble_gatts_count_cfg(svcs) != 0 || ble_gatts_add_svcs(svcs) != 0) return ESP_FAIL;

    {
        // After end(), the request task may not have exited yet: it stays.
        MutexGuard guard(stateMutex_);
        exit_ = false;
        if (!task_ && xTaskCreate(&BleTransport::taskEntry, "tesser_ble", stackSize, this, priority, &task_) != pdPASS) {
            task_ = nullptr;
            return ESP_ERR_NO_MEM;
        }
    }
    up_ = true;
    nimble_port_freertos_init(hostTask);
    return ESP_OK;
}

esp_err_t BleTransport::end() {
    if (instance != this || !up_) return ESP_ERR_INVALID_STATE;
    {
        MutexGuard guard(sendMutex_);  // waits for a write in progress
        up_ = false;
    }
    synced_ = false;  // no more advertising
    // Disconnects the client (a disconnection event: the line is reset in
    // the task) and stops the host task.
    if (nimble_port_stop() != 0) return ESP_FAIL;
    esp_err_t err = nimble_port_deinit();  // and the controller
    conn_ = -1;
    notify_ = false;
    indicate_ = false;
    dropped_ = true;
    {
        MutexGuard guard(inMutex_);
        inbox_.clear();
        inbox_.shrink_to_fit();
    }
    instance = nullptr;
    {
        MutexGuard guard(stateMutex_);
        exit_ = true;
        if (task_) xTaskNotifyGive(task_);
    }
    return err;
}

void BleTransport::onSync() {
    ble_hs_util_ensure_addr(0);
    if (!instance) return;
    instance->synced_ = true;
    instance->advertise();
}

void BleTransport::start() {
    enabled_ = true;
    advertise();
}

void BleTransport::stop() {
    enabled_ = false;
    if (!synced_) return;
    ble_gap_adv_stop();
    int conn = conn_;
    if (conn >= 0) ble_gap_terminate(static_cast<uint16_t>(conn), BLE_ERR_REM_USER_CONN_TERM);
}

void BleTransport::setName(const char* name) {
    {
        MutexGuard guard(stateMutex_);
        name_ = name;
    }
    if (instance != this) return;  // begin() takes it
    ble_svc_gap_device_name_set(name);
    if (synced_ && enabled_ && conn_ < 0) {
        ble_gap_adv_stop();  // reports no ADV_COMPLETE
        advertise();
    }
}

std::string BleTransport::peerAddress() const {
    if (conn_ < 0) return std::string();
    MutexGuard guard(stateMutex_);
    return peer_;
}

uint16_t BleTransport::mtu() const {
    int conn = conn_;
    return conn < 0 || !up_ ? uint16_t(0) : ble_att_mtu(static_cast<uint16_t>(conn));
}

// The service's UUID fills the advertisement; the name goes in the scan
// response, which browsers' choosers show. Called from NimBLE's task and
// from the application's (start(), setName()).
void BleTransport::advertise() {
    if (!enabled_ || !synced_ || conn_ >= 0) return;
    std::string name;
    {
        MutexGuard guard(stateMutex_);
        name = name_;
    }
    ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &kService;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);
    ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = reinterpret_cast<const uint8_t*>(name.c_str());
    rsp.name_len = static_cast<uint8_t>(name.size() < 29 ? name.size() : 29);
    rsp.name_is_complete = name.size() < 29 ? 1 : 0;
    ble_gap_adv_rsp_set_fields(&rsp);

    uint8_t ownAddr = 0;
    if (ble_hs_id_infer_auto(0, &ownAddr) != 0) return;
    ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(ownAddr, nullptr, BLE_HS_FOREVER, &params, &BleTransport::gapEvent, this);
}

// Called in NimBLE's host task, which must never wait for the API: an
// indication sent under the API lock waits for a confirmation this task
// delivers.
int BleTransport::gapEvent(ble_gap_event* event, void* arg) {
    auto* self = static_cast<BleTransport*>(arg);
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                ble_gap_conn_desc desc;
                if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                    const uint8_t* a = desc.peer_id_addr.val;  // least significant byte first
                    MutexGuard guard(self->stateMutex_);
                    snprintf(self->peer_, sizeof(self->peer_), "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3],
                             a[2], a[1], a[0]);
                }
                self->conn_ = event->connect.conn_handle;
                // stop() may have missed a client connecting at that moment.
                if (!self->enabled_) ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                xTaskNotifyGive(self->task_);
            } else {
                self->advertise();
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT: {
            self->conn_ = -1;
            self->notify_ = false;
            self->indicate_ = false;
            self->dropped_ = true;
            {
                MutexGuard guard(self->inMutex_);
                self->inbox_.clear();
            }
            xSemaphoreGive(self->confirmed_);  // a sender waiting gives up
            xTaskNotifyGive(self->task_);
            self->advertise();
            break;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            self->advertise();
            break;
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == self->txHandle_) {
                self->notify_ = event->subscribe.cur_notify;
                self->indicate_ = event->subscribe.cur_indicate;
            }
            break;
        case BLE_GAP_EVENT_NOTIFY_TX:
            // An indication ends confirmed (BLE_HS_EDONE) or not; 0 only
            // means it went out.
            if (event->notify_tx.indication && event->notify_tx.status != 0) xSemaphoreGive(self->confirmed_);
            break;
        default:
            break;
    }
    return 0;
}

int BleTransport::access(uint16_t, uint16_t attr, ble_gatt_access_ctxt* ctxt, void*) {
    BleTransport* self = instance;
    if (!self || ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr == self->txHandle_) return BLE_ATT_ERR_UNLIKELY;
    char buf[BLE_ATT_ATTR_MAX_LEN];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, static_cast<uint16_t>(sizeof(buf)), &len) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    {
        MutexGuard guard(self->inMutex_);
        if (self->inbox_.size() + len > kMaxInbox) return BLE_ATT_ERR_PREPARE_QUEUE_FULL;
        self->inbox_.append(buf, len);
    }
    xTaskNotifyGive(self->task_);
    return 0;
}

// Requests are handled here, under the API lock. After a disconnection,
// the client's subscriptions and token are dropped. After end(), the task
// exits between requests.
void BleTransport::taskEntry(void* arg) {
    auto* self = static_cast<BleTransport*>(arg);
    std::string in;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (self->dropped_.exchange(false)) {
            self->line_.reset();  // subscriptions, a line cut short, the token
            self->report(false);
        }
        {
            MutexGuard guard(self->stateMutex_);
            if (self->exit_) {
                self->exit_ = false;
                self->task_ = nullptr;
                break;
            }
        }
        if (self->conn_ >= 0) self->report(true);
        {
            MutexGuard guard(self->inMutex_);
            in.swap(self->inbox_);
        }
        if (!in.empty()) self->line_.feed(in.data(), in.size());
        in.clear();
    }
    vTaskDelete(nullptr);
}

// In the task: once per change.
void BleTransport::report(bool connected) {
    if (connected == reported_) return;
    reported_ = connected;
    if (onConnection_) onConnection_(connected);
}

void BleTransport::write(const char* data, size_t len) {
    MutexGuard guard(sendMutex_);
    if (conn_ < 0 || !up_) return;
    uint16_t mtu = ble_att_mtu(static_cast<uint16_t>(conn_.load()));
    size_t piece = mtu > 23 ? mtu - 3u : 20u;
    for (size_t i = 0; i < len && conn_ >= 0; i += piece) {
        if (!sendPiece(data + i, len - i < piece ? len - i : piece)) return;
    }
}

// Notifications: NimBLE queues what it accepts until the link carries it,
// and refuses (no buffer) when its pool is full, which paces a long reply.
// Indications, for clients that subscribe with them: one at a time, each
// confirmed. Gives up after 2 s without progress.
bool BleTransport::sendPiece(const char* data, size_t len) {
    for (int waited = 0; waited < 200; waited++) {
        int conn = conn_;
        if (conn < 0 || (!notify_ && !indicate_)) return false;
        os_mbuf* om = ble_hs_mbuf_from_flat(data, static_cast<uint16_t>(len));
        if (om) {
            if (notify_) {
                int rc = ble_gatts_notify_custom(static_cast<uint16_t>(conn), txHandle_, om);
                if (rc == 0) return true;
                if (rc != BLE_HS_ENOMEM) return false;
            } else {
                xSemaphoreTake(confirmed_, 0);
                if (ble_gatts_indicate_custom(static_cast<uint16_t>(conn), txHandle_, om) == 0) {
                    return xSemaphoreTake(confirmed_, pdMS_TO_TICKS(2000)) == pdTRUE && conn_ >= 0;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

}  // namespace tesser

#endif

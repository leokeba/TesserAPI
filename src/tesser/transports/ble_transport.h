#pragma once

// JSON envelopes over Bluetooth LE, one per line, as on a serial port
// (docs/DESIGN.md section 10.6). The service is the Nordic UART Service: a
// client writes lines to RX and receives replies and notifications on TX,
// in pieces of the connection's MTU.
//
// Not included by TesserAPI.h, so that firmware without it doesn't pull in a
// BLE stack.
//
// Arduino (BLEDevice: Bluedroid or NimBLE), TX by confirmed indications:
//
//   #include <tesser/transports/ble_transport.h>
//   tesser::BleTransport ble(api);
//   ble.begin("heliostat-3");   // in setup(), once the tree is declared
//   ble.poll();                 // in loop(): requests are handled there
//
// ESP-IDF (NimBLE, CONFIG_BT_NIMBLE_ENABLED), TX by notifications paced by
// the host's buffers (or indications, if the client asks for them); requests
// are handled in the transport's own task:
//
//   nvs_flash_init();           // NimBLE keeps its state there
//   tesser::BleTransport ble(api);
//   ble.begin("heliostat-3");
#if defined(ARDUINO)

#include <Arduino.h>
#include <soc/soc_caps.h>

#if defined(SOC_BLE_SUPPORTED) && (defined(CONFIG_BLUEDROID_ENABLED) || defined(CONFIG_NIMBLE_ENABLED))

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#if defined(CONFIG_NIMBLE_ENABLED)
#include <services/gap/ble_svc_gap.h>
#endif
#include <stdio.h>
#include <string.h>

#include <atomic>
#include <functional>
#include <string>

#include "tesser/api.h"
#include "tesser/line_transport.h"
#include "tesser/platform.h"

namespace tesser {

class BleTransport : private BLEServerCallbacks, private BLECharacteristicCallbacks {
public:
    static constexpr const char* kService = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
    static constexpr const char* kRx = "6e400002-b5a3-f393-e0a9-e50e24dcca9e";
    static constexpr const char* kTx = "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

    explicit BleTransport(Api& api)
        : api_(api), line_(api, [this](const char* data, size_t len) { write(data, len); }) {
        // Anyone in range can connect: unlike a serial line, a connection
        // proves nothing (docs/DESIGN.md section 13.2).
        line_.setAuthenticated(false);
        line_.setTransport(TransportKind::Ble);
        // Notifications and deferred replies wait for poll(), which writes
        // them without the API lock (docs/DESIGN.md section 9).
        line_.queueOutput([] {});
    }

    // Starts the BLE stack under this name (unless the sketch did), the
    // service, and advertising. One client at a time: advertising stops
    // while one is connected.
    void begin(const char* name) {
        if (!BLEDevice::getInitialized()) {
            BLEDevice::init(name);
        } else {
            setDeviceName(name);
        }
        BLEDevice::setMTU(517);
        server_ = BLEDevice::createServer();
        server_->setCallbacks(this);
        BLEService* service = server_->createService(kService);
        tx_ = service->createCharacteristic(kTx, BLECharacteristic::PROPERTY_INDICATE);
        tx_->addDescriptor(new BLE2902());
        BLECharacteristic* rx =
            service->createCharacteristic(kRx, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
        rx->setCallbacks(this);
        service->start();
        BLEAdvertising* adv = BLEDevice::getAdvertising();
        adv->addServiceUUID(kService);
        adv->setScanResponse(true);  // the name goes there: the UUID fills the advertisement
        BLEDevice::startAdvertising();
    }

    // Handles what the client wrote, in the caller's task (loop()), under
    // the API lock, then writes the notifications that wait. After a
    // disconnection, drops the client's subscriptions and token, and
    // advertises again.
    void poll() {
        if (dropped_.exchange(false)) {
            line_.reset();  // subscriptions, a line cut short, the token
            if (enabled_ && server_) BLEDevice::startAdvertising();
            report(false);
        }
        if (connected_) report(true);
        std::string in;
        {
            MutexGuard guard(inMutex_);
            in.swap(inbox_);
        }
        if (!in.empty()) line_.feed(in.data(), in.size());
        line_.flush();
    }

    // Stops the BLE stack and gives its memory back (BLEDevice::deinit(false),
    // which allows a new init); a connected client is dropped as by a
    // disconnection, in the next poll(). begin() starts it again. Call it in
    // the task that calls poll(), as a setter or action does.
    void end() {
        if (!server_) return;
        BLEDevice::deinit(false);
        server_ = nullptr;
        tx_ = nullptr;
        connected_ = false;
        dropped_ = true;
        MutexGuard guard(inMutex_);
        inbox_.clear();
    }
    bool begun() const { return server_ != nullptr; }

    // Bluetooth on and off at runtime, after begin(): stop() drops the
    // client (as a disconnection does, in the next poll()) and stops
    // advertising; start() advertises again. The stack stays up.
    void start() {
        enabled_ = true;
        if (server_ && !connected_) BLEDevice::startAdvertising();
    }
    void stop() {
        enabled_ = false;
        if (!server_) return;
        BLEDevice::stopAdvertising();
        if (connected_) server_->disconnect(server_->getConnId());
    }
    bool enabled() const { return enabled_; }

    // The device name (GAP name, and the scan response that choosers show).
    // Advertising restarts with it; a connected client sees it after it
    // leaves. The scan response holds 29 bytes of it.
    void setName(const char* name) {
        if (!server_) return;  // begin() takes it
        setDeviceName(name);
        if (server_ && enabled_ && !connected_) {
            BLEDevice::stopAdvertising();
            BLEDevice::startAdvertising();
        }
    }

    bool connected() const { return connected_; }
    // The connected client's address ("aa:bb:cc:dd:ee:ff"), empty when none,
    // and the connection's ATT MTU, 0 when none.
    std::string peerAddress() const {
        if (!connected_) return std::string();
        MutexGuard guard(inMutex_);
        return peer_;
    }
    uint16_t mtu() const { return connected_ ? mtu_.load() : uint16_t(0); }
    // Called with true when a client connects and false when it leaves, in
    // poll(), so it may use the API.
    void onConnection(std::function<void(bool connected)> fn) { onConnection_ = std::move(fn); }

    LineTransport& lines() { return line_; }

    // Requests count as authenticated (access levels, Api::authorize())
    // only when set: for a bench, or a link the application secures itself.
    // Otherwise the client's level is the API's token check's answer for the
    // token it sent with {"op":"auth"}, or for none (LineTransport).
    void setAuthenticated(bool authenticated) { line_.setAuthenticated(authenticated); }

private:
    // Called from the BLE stack's task, which must never wait for the API:
    // indications sent under the API lock wait for this task's confirmations.
#if defined(CONFIG_BLUEDROID_ENABLED)
    void onConnect(BLEServer*, esp_ble_gatts_cb_param_t* param) override {
        connecting(param->connect.remote_bda, false);
    }
    void onMtuChanged(BLEServer*, esp_ble_gatts_cb_param_t* param) override { mtu_ = param->mtu.mtu; }
#else
    void onConnect(BLEServer*, ble_gap_conn_desc* desc) override { connecting(desc->peer_id_addr.val, true); }
    void onMtuChanged(BLEServer*, ble_gap_conn_desc*, uint16_t mtu) override { mtu_ = mtu; }
#endif
    void onDisconnect(BLEServer*) override {
        connected_ = false;
        dropped_ = true;
        MutexGuard guard(inMutex_);
        inbox_.clear();
    }
    // NimBLE stores addresses least significant byte first.
    void connecting(const uint8_t* a, bool reversed) {
        char buf[18];
        if (reversed) {
            snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
        } else {
            snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3], a[4], a[5]);
        }
        {
            MutexGuard guard(inMutex_);
            peer_ = buf;
        }
        mtu_ = 23;
        connected_ = true;
        // stop() may have missed a client connecting at that moment.
        if (!enabled_ && server_) server_->disconnect(server_->getConnId());
    }

    static void setDeviceName(const char* name) {
#if defined(CONFIG_BLUEDROID_ENABLED)
        esp_ble_gap_set_device_name(name);  // the scan response includes it
#else
        ble_svc_gap_device_name_set(name);
        BLEDevice::getAdvertising()->setName(name);
#endif
    }

    // In poll(): once per change.
    void report(bool connected) {
        if (connected == reported_) return;
        reported_ = connected;
        if (onConnection_) onConnection_(connected);
    }
    void onWrite(BLECharacteristic* c) override {
        String v = c->getValue();
        MutexGuard guard(inMutex_);
        if (inbox_.size() + v.length() <= kMaxInbox) inbox_.append(v.c_str(), v.length());
    }

    // Each piece is indicated and confirmed before the next, which paces a
    // long reply to what the link carries, as a UART's buffer does.
    void write(const char* data, size_t len) {
        if (!connected_ || !server_) return;
        uint16_t mtu = server_->getPeerMTU(server_->getConnId());
        size_t piece = mtu > 23 ? mtu - 3u : 20u;
        for (size_t i = 0; i < len && connected_; i += piece) {
            size_t n = len - i < piece ? len - i : piece;
            tx_->setValue(reinterpret_cast<const uint8_t*>(data + i), n);
            tx_->indicate();
        }
    }

    static constexpr size_t kMaxInbox = 8192;

    Api& api_;
    LineTransport line_;
    BLEServer* server_ = nullptr;
    BLECharacteristic* tx_ = nullptr;
    std::atomic<bool> connected_{false};
    std::atomic<bool> dropped_{false};
    std::atomic<bool> enabled_{true};
    std::atomic<uint16_t> mtu_{0};
    mutable Mutex inMutex_;  // the inbox and peer_
    std::string inbox_;
    std::string peer_;
    std::function<void(bool)> onConnection_;
    bool reported_ = false;
};

}  // namespace tesser

#endif  // BLE support

#elif defined(ESP_PLATFORM)

#include "sdkconfig.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED)

#include <atomic>
#include <functional>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tesser/line_transport.h"
#include "tesser/platform.h"

struct ble_gap_event;
struct ble_gatt_access_ctxt;

namespace tesser {

class BleTransport {
public:
    explicit BleTransport(Api& api);
    BleTransport(const BleTransport&) = delete;
    BleTransport& operator=(const BleTransport&) = delete;

    // Starts NimBLE (the controller and the host task), the service under
    // this device name, advertising, and the task that handles requests,
    // under the API lock. One transport per firmware; one client at a time.
    esp_err_t begin(const char* name, uint32_t stackSize = 6144, UBaseType_t priority = 5);
    // Stops NimBLE and gives its memory back (the host, the controller, the
    // request task: about 45 KB on an ESP32); a connected client is dropped
    // as by a disconnection. begin() starts it again. Doesn't wait for the
    // request task, so it may be called from a setter or action, even one a
    // Bluetooth client called.
    esp_err_t end();
    bool begun() const { return up_; }

    // Bluetooth on and off at runtime: stop() drops the client (as a
    // disconnection does) and stops advertising; start() advertises again.
    // The stack stays up. Either may come before begin().
    void start();
    void stop();
    bool enabled() const { return enabled_; }

    // The device name (GAP name, and the scan response that choosers show).
    // Advertising restarts with it; a connected client sees it after it
    // leaves. The scan response holds 29 bytes of it.
    void setName(const char* name);

    bool connected() const { return conn_ >= 0; }
    // The connected client's address ("aa:bb:cc:dd:ee:ff"), empty when none,
    // and the connection's ATT MTU, 0 when none.
    std::string peerAddress() const;
    uint16_t mtu() const;
    // Called with true when a client connects and false when it leaves, in
    // the transport's task, so it may use the API. Set before begin().
    void onConnection(std::function<void(bool connected)> fn) { onConnection_ = std::move(fn); }

    LineTransport& lines() { return line_; }
    // Requests count as authenticated (access levels, Api::authorize())
    // only when set: for a bench, or a link the application secures itself.
    // Otherwise the client's level is the API's token check's answer for the
    // token it sent with {"op":"auth"}, or for none (LineTransport).
    void setAuthenticated(bool authenticated) { line_.setAuthenticated(authenticated); }

    // NimBLE's callbacks (C function pointers), forwarded to the instance.
    static int gapEvent(ble_gap_event* event, void* arg);
    static int access(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt* ctxt, void* arg);
    static void onSync();

private:
    static void taskEntry(void* arg);
    void advertise();
    void report(bool connected);
    void write(const char* data, size_t len);
    bool sendPiece(const char* data, size_t len);

    static constexpr size_t kMaxInbox = 8192;

    Api& api_;
    LineTransport line_;
    mutable Mutex stateMutex_;  // name_, peer_, task_ and exit_
    std::string name_;
    char peer_[18] = {};
    std::atomic<bool> enabled_{true};
    std::atomic<bool> up_{false};      // between begin() and end()
    std::atomic<bool> synced_{false};  // the host is up: advertising works
    std::atomic<int> conn_{-1};
    std::atomic<bool> notify_{false};
    std::atomic<bool> indicate_{false};
    std::atomic<bool> dropped_{false};
    uint16_t txHandle_ = 0;
    Mutex inMutex_;
    std::string inbox_;
    TaskHandle_t task_ = nullptr;
    bool exit_ = false;  // end(): the task exits, unless begin() comes first
    Mutex sendMutex_;    // held while writing, so end() never pulls NimBLE from under a write
    SemaphoreHandle_t confirmed_ = nullptr;  // an indication's confirmation
    std::function<void(bool)> onConnection_;
    bool reported_ = false;  // in the task
};

}  // namespace tesser

#endif  // CONFIG_BT_NIMBLE_ENABLED
#endif  // platform

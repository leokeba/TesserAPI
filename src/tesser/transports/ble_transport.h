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

#include <atomic>
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
    }

    // Starts the BLE stack under this name (unless the sketch did), the
    // service, and advertising. One client at a time: advertising stops
    // while one is connected.
    void begin(const char* name) {
        if (!BLEDevice::getInitialized()) BLEDevice::init(name);
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
    // the API lock. After a disconnection, drops the client's subscriptions
    // and advertises again.
    void poll() {
        if (dropped_.exchange(false)) {
            api_.dropSubscriber(&line_);
            line_.feed('\n');  // ends a line cut short; its reply goes nowhere
            BLEDevice::startAdvertising();
        }
        std::string in;
        {
            MutexGuard guard(inMutex_);
            in.swap(inbox_);
        }
        if (!in.empty()) line_.feed(in.data(), in.size());
    }

    bool connected() const { return connected_; }
    LineTransport& lines() { return line_; }

    // Requests count as authenticated (access levels, Api::authorize())
    // only when set: for a bench, or a link the application secures itself.
    void setAuthenticated(bool authenticated) { line_.setAuthenticated(authenticated); }

private:
    // Called from the BLE stack's task, which must never wait for the API:
    // indications sent under the API lock wait for this task's confirmations.
    void onConnect(BLEServer*) override { connected_ = true; }
    void onDisconnect(BLEServer*) override {
        connected_ = false;
        dropped_ = true;
        MutexGuard guard(inMutex_);
        inbox_.clear();
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
    Mutex inMutex_;
    std::string inbox_;
};

}  // namespace tesser

#endif  // BLE support

#elif defined(ESP_PLATFORM)

#include "sdkconfig.h"

#if defined(CONFIG_BT_NIMBLE_ENABLED)

#include <atomic>
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

    bool connected() const { return conn_ >= 0; }
    LineTransport& lines() { return line_; }
    // Requests count as authenticated (access levels, Api::authorize())
    // only when set: for a bench, or a link the application secures itself.
    void setAuthenticated(bool authenticated) { line_.setAuthenticated(authenticated); }

    // NimBLE's callbacks (C function pointers), forwarded to the instance.
    static int gapEvent(ble_gap_event* event, void* arg);
    static int access(uint16_t conn, uint16_t attr, ble_gatt_access_ctxt* ctxt, void* arg);
    static void onSync();

private:
    static void taskEntry(void* arg);
    void advertise();
    void write(const char* data, size_t len);
    bool sendPiece(const char* data, size_t len);

    static constexpr size_t kMaxInbox = 8192;

    Api& api_;
    LineTransport line_;
    std::string name_;
    std::atomic<int> conn_{-1};
    std::atomic<bool> notify_{false};
    std::atomic<bool> indicate_{false};
    std::atomic<bool> dropped_{false};
    uint16_t txHandle_ = 0;
    Mutex inMutex_;
    std::string inbox_;
    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t confirmed_ = nullptr;  // an indication's confirmation
};

}  // namespace tesser

#endif  // CONFIG_BT_NIMBLE_ENABLED
#endif  // platform

#pragma once

// Arduino only: JSON envelopes over Bluetooth LE, one per line, as on a
// serial port (docs/DESIGN.md section 10.6). The service is the Nordic UART
// Service: a client writes lines to RX and receives replies and
// notifications as indications on TX, in pieces of the connection's MTU.
//
// Not included by TesserAPI.h, so that sketches without it don't pull in a
// BLE stack:
//
//   #include <tesser/transports/ble_transport.h>
//   tesser::BleTransport ble(api);
//   ble.begin("heliostat-3");   // in setup(), once the tree is declared
//   ble.poll();                 // in loop(): requests are handled there
#if defined(ARDUINO)

#include <Arduino.h>
#include <soc/soc_caps.h>

#if defined(SOC_BLE_SUPPORTED) && (defined(CONFIG_BLUEDROID_ENABLED) || defined(CONFIG_NIMBLE_ENABLED))

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>

#include <atomic>
#include <string>

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

#endif
#endif

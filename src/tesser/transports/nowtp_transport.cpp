#include "tesser/transports/nowtp_transport.h"

#if defined(ESP_PLATFORM) && defined(TESSER_HAVE_NOWTP)

#include <stdio.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "tesser/api.h"

namespace tesser {

namespace {

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

}  // namespace

NowTpTransport::NowTpTransport(Api* api, nowtp::EspNowTransport& now, uint8_t port,
                               DatagramEndpoint::Options options)
    : now_(now),
      port_(port),
      endpoint_(
          api, TransportKind::NowTP,
          [this](const PeerAddress& to, const std::string& message, Delivery delivery) {
              nowtp::Mac mac = nowtp::Mac::from(to.bytes);
              nowtp::SendOptions opts;
              opts.reliable = delivery == Delivery::Reliable && !mac.isBroadcast();
              opts.latestOnly = delivery == Delivery::LatestOnly;
              return now_.send(mac, port_, message.data(), message.size(), opts) == nowtp::Status::Ok;
          },
          &nowMs, options) {}

NowTpTransport::~NowTpTransport() { end(); }

bool NowTpTransport::begin(bool runTask, uint32_t stackSize, UBaseType_t priority) {
    if (running_) return false;
    running_ = true;
    if (runTask) {
        if (xTaskCreate(&NowTpTransport::taskEntry, "tesser_nowtp", stackSize, this, priority, &task_) != pdPASS) {
            running_ = false;
            return false;
        }
        TaskHandle_t task = task_;
        endpoint_.onQueued([task] { xTaskNotifyGive(task); });
    }
    now_.listen(port_, [this](const nowtp::Message& m) {
        endpoint_.receive(address(m.src), reinterpret_cast<const char*>(m.data), m.len, m.reliable);
    });
    return true;
}

void NowTpTransport::end() {
    if (!running_) return;
    now_.listen(port_, nullptr);
    running_ = false;
    if (task_) {
        xTaskNotifyGive(task_);
        while (task_) vTaskDelay(pdMS_TO_TICKS(5));
    }
    endpoint_.onQueued(nullptr);
}

void NowTpTransport::taskEntry(void* arg) {
    auto* self = static_cast<NowTpTransport*>(arg);
    while (self->running_) {
        // Wake on new messages; poll periodically to expire client calls.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        self->endpoint_.process();
    }
    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

bool NowTpTransport::get(const nowtp::Mac& to, std::string_view path, DatagramEndpoint::ResponseHandler done,
                         const Query& query, uint32_t timeoutMs) {
    return endpoint_.request(address(to), Op::Get, path, std::string_view(), std::move(done), query, timeoutMs);
}

bool NowTpTransport::set(const nowtp::Mac& to, std::string_view path, std::string_view bodyJson,
                         DatagramEndpoint::ResponseHandler done, uint32_t timeoutMs) {
    return endpoint_.request(address(to), Op::Set, path, bodyJson, std::move(done), Query(), timeoutMs);
}

bool NowTpTransport::advertise() {
    Api* api = endpoint_.api();
    if (!api) return false;
    std::string meta = buildAdvertisement(port_, api->schemaHash());
    return now_.setDiscoveryMetadata(meta.data(), meta.size()) == nowtp::Status::Ok;
}

bool NowTpTransport::parseAdvertisement(const std::vector<uint8_t>& metadata, uint8_t& port, uint32_t& schemaHash) {
    return tesser::parseAdvertisement(
        std::string_view(reinterpret_cast<const char*>(metadata.data()), metadata.size()), port, schemaHash);
}

void NowTpTransport::peerEvent(nowtp::PeerEvent event, const nowtp::PeerInfo& peer) {
    if (event == nowtp::PeerEvent::Lost) return peerLost(peer.mac);
    uint8_t port;
    uint32_t hash;
    if (parseAdvertisement(peer.metadata, port, hash) && port == port_) {
        endpoint_.peerSeen(address(peer.mac), peer.name, hash);
    }
}

void NowTpTransport::mountPeers(Object& parent, uint32_t mirrorIntervalMs) {
    endpoint_.mountPeers(parent, mirrorIntervalMs);
    for (const nowtp::PeerInfo& p : now_.peers()) peerEvent(nowtp::PeerEvent::Found, p);
}

}  // namespace tesser

#endif

#pragma once

// NowTP (ESP-NOW) transport. Compiled when NowTP is available: the ESP-IDF
// component sets TESSER_HAVE_NOWTP when a "nowtp" component is in the build;
// Arduino and PlatformIO detect <NowTP.h>. See docs/DESIGN.md section 10.3.
#if defined(ESP_PLATFORM) && !defined(TESSER_HAVE_NOWTP) && defined(__has_include)
#if __has_include(<NowTP.h>)
#define TESSER_HAVE_NOWTP 1
#endif
#endif

#if defined(ESP_PLATFORM) && defined(TESSER_HAVE_NOWTP)

#include <NowTP.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tesser/datagram.h"

namespace tesser {

class NowTpTransport {
public:
    static constexpr uint8_t kDefaultPort = 84;

    // `api` may be null for a client-only node.
    NowTpTransport(Api* api, nowtp::EspNowTransport& now, uint8_t port = kDefaultPort,
                   DatagramEndpoint::Options options = DatagramEndpoint::Options());
    ~NowTpTransport();
    NowTpTransport(const NowTpTransport&) = delete;
    NowTpTransport& operator=(const NowTpTransport&) = delete;

    // Listens on the port. With runTask, a worker task handles requests and
    // client callbacks; otherwise call poll() from loop(). Requests never run
    // inside NowTP's own callbacks (which hold NowTP's lock).
    bool begin(bool runTask = true, uint32_t stackSize = 6144, UBaseType_t priority = 4);
    void end();
    void poll() { endpoint_.process(); }

    // Client calls to another node. `done` runs in the worker task (or poll()).
    bool get(const nowtp::Mac& to, std::string_view path, DatagramEndpoint::ResponseHandler done,
             const Query& query = Query(), uint32_t timeoutMs = 0);
    bool set(const nowtp::Mac& to, std::string_view path, std::string_view bodyJson,
             DatagramEndpoint::ResponseHandler done, uint32_t timeoutMs = 0);

    // Call from your NowTP peer-event handler when a peer is lost: fails its
    // pending calls (and, later, drops its subscriptions).
    void peerLost(const nowtp::Mac& mac) { endpoint_.forgetPeer(address(mac)); }

    DatagramEndpoint& endpoint() { return endpoint_; }
    uint8_t port() const { return port_; }

    static PeerAddress address(const nowtp::Mac& mac) { return PeerAddress::fromBytes(mac.bytes, 6); }

private:
    static void taskEntry(void* arg);

    nowtp::EspNowTransport& now_;
    uint8_t port_;
    DatagramEndpoint endpoint_;
    TaskHandle_t task_ = nullptr;
    volatile bool running_ = false;
};

}  // namespace tesser

#endif

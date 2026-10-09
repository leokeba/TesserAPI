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
#include <string.h>

#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tesser/datagram.h"
#include "tesser/remote.h"

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
    // pending calls and drops its subscriptions.
    void peerLost(const nowtp::Mac& mac) { endpoint_.forgetPeer(address(mac)); }

    // Requests from these peers count as authenticated (Api::authorize()).
    // ESP-NOW frames carry no proof of origin, so this trusts MAC addresses:
    // combine it with encrypted peers (EspNowTransport::addPeer) when that
    // matters.
    void trustPeers(std::vector<nowtp::Mac> peers) {
        endpoint_.trust([peers = std::move(peers)](const PeerAddress& a) {
            for (const nowtp::Mac& m : peers) {
                if (a.length == 6 && memcmp(a.bytes, m.bytes, 6) == 0) return true;
            }
            return false;
        });
    }

    // Grafts another node's tree (at its `remotePath`) under `parent`: a
    // gateway's GET /api/<name>/... is forwarded to that node over ESP-NOW.
    RemoteNode& remote(Object& parent, const char* name, const nowtp::Mac& mac, const char* remotePath = "/") {
        return parent.remote(name, endpoint_, address(mac), remotePath);
    }

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

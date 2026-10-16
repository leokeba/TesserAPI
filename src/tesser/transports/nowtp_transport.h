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

#include <memory>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tesser/datagram.h"
#include "tesser/remote.h"

namespace tesser {

class NowTpTransport {
public:
    static constexpr uint8_t kDefaultPort = 84;

    // The endpoint's options for NowTP messages of up to `maxMessageSize`
    // bytes: pass NowTP's Config::maxMessageSize when it isn't the default
    // (docs/DESIGN.md section 15), so a reply that wouldn't fit answers
    // too_large instead of being rejected by the link.
    static DatagramEndpoint::Options endpointOptions(size_t maxMessageSize = 16 * 1024) {
        DatagramEndpoint::Options o;
        o.maxMessage = maxMessageSize;
        return o;
    }

    // `api` may be null for a client-only node.
    NowTpTransport(Api* api, nowtp::EspNowTransport& now, uint8_t port = kDefaultPort,
                   DatagramEndpoint::Options options = endpointOptions());
    ~NowTpTransport();
    NowTpTransport(const NowTpTransport&) = delete;
    NowTpTransport& operator=(const NowTpTransport&) = delete;

    // Listens on the port. With runTask, a worker task handles requests and
    // client callbacks; otherwise call poll() from loop(), or from a task
    // that endpoint().onQueued() wakes (begin() and end() leave that hook
    // alone then). Requests never run
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
    void peerLost(const nowtp::Mac& mac) { endpoint_.linkLost(address(mac)); }
    // Or hand it every peer event: TesserAPI peers on this port are marked
    // online (and mounted, with mountPeers()), lost ones offline.
    //   radio.onPeerEvent([](nowtp::PeerEvent e, const nowtp::PeerInfo& p) { nowApi.peerEvent(e, p); });
    void peerEvent(nowtp::PeerEvent event, const nowtp::PeerInfo& peer);

    // Mounts every TesserAPI peer on this port under `parent`, named after
    // its NowTP discovery name (docs/DESIGN.md section 14.1), including the
    // peers already discovered. Needs peerEvent() for the ones found later.
    void mountPeers(Object& parent, uint32_t mirrorIntervalMs = 0);

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

    // Announces this node in NowTP discovery metadata as
    // {"tesser":<port>,"schema":"<schemaHash, 8 hex digits>"}, so peers can
    // tell TesserAPI nodes apart and keep cached schemas while the hash
    // holds. Call once the tree is complete: after that, the advertisement
    // follows the tree, 1 s after its last change (peers mounted, nodes
    // added, a mounted peer going offline).
    bool advertise();
    // Reads such metadata; false if it isn't a TesserAPI advertisement.
    static bool parseAdvertisement(const std::vector<uint8_t>& metadata, uint8_t& port, uint32_t& schemaHash);

    DatagramEndpoint& endpoint() { return endpoint_; }
    uint8_t port() const { return port_; }

    static PeerAddress address(const nowtp::Mac& mac) { return PeerAddress::fromBytes(mac.bytes, 6); }

private:
    // What NowTP's completion handlers report to, cleared before the
    // transport goes: they can run after it, in NowTP's task.
    struct Reports {
        Mutex mutex;
        DatagramEndpoint* endpoint = nullptr;
    };

    static void taskEntry(void* arg);
    nowtp::CompletionHandler completion(const PeerAddress& to, uint32_t callId);
    template <class T>
    void installOwnedSend(T& now);

    std::shared_ptr<Reports> reports_ = std::make_shared<Reports>();
    nowtp::EspNowTransport& now_;
    uint8_t port_;
    DatagramEndpoint endpoint_;
    TaskHandle_t task_ = nullptr;
    volatile bool running_ = false;
    bool ownHook_ = false;  // onQueued() wakes our worker: ours to clear
};

}  // namespace tesser

#endif

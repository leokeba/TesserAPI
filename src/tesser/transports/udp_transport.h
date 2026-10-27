#pragma once

// UDP transport: TesserAPI between nodes and clients on a LAN (Ethernet or
// Wi-Fi), with discovery and broadcast writes. lwIP sockets, so ESP-IDF and
// Arduino-ESP32 3.x alike. See docs/DESIGN.md section 10.7.

#if defined(ESP_PLATFORM)

#include <atomic>
#include <string_view>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tesser/datagram.h"
#include "tesser/remote.h"
#include "tesser/udp_link.h"

namespace tesser {

class UdpTransport {
public:
    static constexpr uint16_t kDefaultPort = 8484;

    // Which tasks begin() starts (docs/DESIGN.md section 10.7).
    enum class Tasks : uint8_t {
        None,     // poll() reads the socket and does the work
        Receive,  // a receive task reads the socket; poll() does the work
        Both,     // a receive task and a worker: nothing to call
    };

    // `api` may be null for a client-only node.
    explicit UdpTransport(Api* api, UdpLink::Options options = UdpLink::Options(),
                          DatagramEndpoint::Options endpointOptions = UdpLink::endpointOptions());
    // Call end() first if the transport may be running: destruction waits
    // for its tasks.
    ~UdpTransport();
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Opens the socket on `port` (all interfaces) once the network is up.
    // `tasks` says which tasks handle the traffic and the requests; without
    // a worker, call poll() at least every 20 ms, and when the endpoint's
    // onQueued() hook fires, which begin() leaves to the application then.
    // `stackSize` is the worker's, `priority` the worker's (the receive
    // task runs one above). Advertises the device's Wi-Fi station MAC as its
    // id, and, unless setName() came first, "tesser-<last two MAC bytes>" as
    // its name.
    bool begin(uint16_t port, Tasks tasks, uint32_t stackSize = 6144, UBaseType_t priority = 4);
    // runTask: Tasks::Both, else Tasks::None.
    bool begin(uint16_t port = kDefaultPort, bool runTask = true, uint32_t stackSize = 6144,
               UBaseType_t priority = 4) {
        return begin(port, runTask ? Tasks::Both : Tasks::None, stackSize, priority);
    }
    // Says goodbye, closes the socket, and forgets every peer (without a
    // worker, at the next poll()). Safe from a request handler, including
    // one this transport runs: it doesn't wait for the worker, which
    // finishes the request at hand and exits (or carries on, if begin()
    // with a worker comes first). begin() again to restart, on any port.
    void end();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }
    // Without a worker: handles what arrived (reading the socket too
    // without a receive task), then retransmits, advertises and runs the
    // requests.
    void poll();

    // ---- interfaces (docs/DESIGN.md section 10.7) -------------------------
    static constexpr uint8_t kEthernet = 1, kWifi = 2, kAp = 4, kAllInterfaces = 7;
    static uint8_t interfaceBit(NetInterface iface);
    // The interfaces UDP may use (default all): nothing is served,
    // advertised, broadcast or routed on the others. With exactly one, the
    // socket is bound to it at begin() (SO_BINDTODEVICE), so everything
    // leaves by it even when interfaces share a subnet; with several,
    // peers are told apart by subnet. Takes full effect at the next begin().
    void setInterfaces(uint8_t mask) { interfaces_ = mask & kAllInterfaces; }
    uint8_t interfaces() const { return interfaces_.load(); }
    // The interface `peer` is reached by: the one the socket is bound to,
    // else the one interface that is up whose subnet holds it (the default
    // interface for a peer beyond them); Any when several share the subnet.
    NetInterface interfaceOf(const PeerAddress& peer) const;

    // ---- discovery (docs/DESIGN.md section 14.1) ------------------------
    void setName(std::string_view name) {
        named_ = true;
        link_.setName(name);
    }
    bool advertise() { return link_.advertise(); }
    void mountPeers(Object& parent, uint32_t mirrorIntervalMs = 0) { link_.mountPeers(parent, mirrorIntervalMs); }

    // ---- client ---------------------------------------------------------
    bool get(const PeerAddress& to, std::string_view path, DatagramEndpoint::ResponseHandler done,
             const Query& query = Query(), uint32_t timeoutMs = 0) {
        return link_.endpoint().request(to, Op::Get, path, std::string_view(), std::move(done), query, timeoutMs);
    }
    bool set(const PeerAddress& to, std::string_view path, std::string_view bodyJson,
             DatagramEndpoint::ResponseHandler done, uint32_t timeoutMs = 0) {
        return link_.endpoint().request(to, Op::Set, path, bodyJson, std::move(done), Query(), timeoutMs);
    }
    // One datagram that every node behind `to` applies (none answers, this
    // one doesn't): a directed broadcast address (192.168.1.255:8484), or
    // an empty address for every network this node is on.
    bool broadcastSet(const PeerAddress& to, std::string_view path, std::string_view bodyJson) {
        return link_.broadcastSet(to, path, bodyJson);
    }
    RemoteNode& remote(Object& parent, const char* name, const PeerAddress& peer, const char* remotePath = "/") {
        return parent.remote(name, link_.endpoint(), peer, remotePath);
    }

    // ---- access (docs/DESIGN.md section 13) -----------------------------
    // The least level of a peer (default public); tokens sent with "auth"
    // can raise it. Datagrams carry no proof of origin.
    void setPeerAccess(Access level) { link_.endpoint().setPeerAccess(level); }

    // The broadcast address of each interface that is up, on `port` (of
    // those in `mask`).
    static std::vector<PeerAddress> broadcastAddresses(uint16_t port, uint8_t mask = kAllInterfaces);

    UdpLink& link() { return link_; }
    DatagramEndpoint& endpoint() { return link_.endpoint(); }

private:
    static void receiveEntry(void* arg);
    static void workEntry(void* arg);
    bool sendTo(const PeerAddress& to, const uint8_t* head, size_t headLen, const uint8_t* body, size_t bodyLen);
    void receiveWaiting();
    // The interfaces (bits) whose subnet holds `peer`; the bound one's.
    uint8_t candidates(const PeerAddress& peer) const;
    bool allowed(const PeerAddress& peer) const;

    UdpLink link_;
    Mutex socketMutex_;  // the socket isn't closed during a send
    Mutex taskMutex_;    // begin() and the worker's exit
    std::atomic<int> socket_{-1};
    std::atomic<bool> running_{false};
    uint16_t port_ = 0;
    bool named_ = false;
    std::atomic<Tasks> tasks_{Tasks::Both};
    std::atomic<uint8_t> interfaces_{kAllInterfaces};
    std::atomic<NetInterface> bound_{NetInterface::Any};  // the socket's interface, if bound
    bool ownHook_ = false;  // onQueued() wakes our worker: ours to clear
    TaskHandle_t receiveTask_ = nullptr;
    TaskHandle_t workTask_ = nullptr;
    std::vector<uint8_t> buffer_;
};

}  // namespace tesser

#endif

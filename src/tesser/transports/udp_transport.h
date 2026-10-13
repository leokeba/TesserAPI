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

    // `api` may be null for a client-only node.
    explicit UdpTransport(Api* api, UdpLink::Options options = UdpLink::Options(),
                          DatagramEndpoint::Options endpointOptions = UdpLink::endpointOptions());
    // Call end() first if the transport may be running: destruction waits
    // for its tasks.
    ~UdpTransport();
    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    // Opens the socket on `port` (all interfaces) once the network is up.
    // With runTask, a receive task and a worker task handle the traffic and
    // the requests; otherwise call poll() from loop(). Advertises the
    // device's Wi-Fi station MAC as its id, and, unless setName() came
    // first, "tesser-<last two MAC bytes>" as its name.
    bool begin(uint16_t port = kDefaultPort, bool runTask = true, uint32_t stackSize = 6144,
               UBaseType_t priority = 4);
    // Says goodbye, closes the socket, and forgets every peer. Safe from a
    // request handler, including one this transport runs: it doesn't wait
    // for the worker, which finishes the request at hand and exits (or
    // carries on, if begin() comes first). begin() again to restart, on any
    // port.
    void end();
    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }
    // Without a task: handles what arrived, then retransmits, advertises and
    // runs the requests.
    void poll();

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

    // The broadcast address of each interface that is up, on `port`.
    static std::vector<PeerAddress> broadcastAddresses(uint16_t port);

    UdpLink& link() { return link_; }
    DatagramEndpoint& endpoint() { return link_.endpoint(); }

private:
    static void receiveEntry(void* arg);
    static void workEntry(void* arg);
    bool sendTo(const PeerAddress& to, const uint8_t* data, size_t len);
    void receiveWaiting();

    UdpLink link_;
    Mutex socketMutex_;  // the socket isn't closed during a send
    Mutex taskMutex_;    // begin() and the worker's exit
    std::atomic<int> socket_{-1};
    std::atomic<bool> running_{false};
    uint16_t port_ = 0;
    bool named_ = false;
    TaskHandle_t receiveTask_ = nullptr;
    TaskHandle_t workTask_ = nullptr;
    std::vector<uint8_t> buffer_;
};

}  // namespace tesser

#endif

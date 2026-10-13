#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "tesser/datagram.h"
#include "tesser/platform.h"

namespace tesser {

class Api;
class Object;

// An IPv4 address and UDP port as a PeerAddress: the four address bytes in
// order, then the port, most significant byte first (6 bytes).
PeerAddress udpAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port);
// "192.168.1.59" or "192.168.1.59:8484" (`defaultPort` without one); false
// if the text isn't one.
bool parseUdpAddress(std::string_view text, uint16_t defaultPort, PeerAddress& out);
// "192.168.1.59:8484"; empty for an address that isn't a UDP one.
std::string formatUdpAddress(const PeerAddress& address);

// TesserAPI over UDP (docs/DESIGN.md section 10.7): the DatagramEndpoint's
// envelopes, in messages of up to Options::maxMessage bytes that the link
// splits into datagrams, delivers reliably when asked (acknowledgements,
// retransmission, duplicate suppression), and broadcasts; plus discovery
// advertisements. Platform-independent: an adapter (UdpTransport, or a test)
// sends the datagrams with `send`, hands received ones to packet(), and calls
// tick() every few milliseconds.
//
// `send` gets the destination and the datagram. An empty destination means
// every local network: the adapter sends the datagram to each interface's
// broadcast address.
class UdpLink {
public:
    using PacketFn = std::function<bool(const PeerAddress& to, const uint8_t* data, size_t len)>;

    struct Options {
        uint16_t fragmentSize = 1400;     // message bytes per datagram (plus a 20-byte header)
        size_t maxMessage = 16384;        // longer messages are refused, both ways (at most 64 fragments)
        uint32_t ackTimeoutMs = 100;      // first retransmission of an unacknowledged message; doubles, up to 1 s
        uint32_t sendTimeoutMs = 3000;    // a reliable message fails after this (the peer is then forgotten)
        size_t maxOutgoing = 16;          // reliable messages waiting for their acknowledgement
        size_t reassemblyBytes = 32768;   // messages being reassembled, in total
        size_t maxPeers = 16;             // peers remembered (duplicate suppression, discovery)
        uint32_t advertiseMs = 5000;      // advertisement period, once advertise() is called
        uint32_t peerTimeoutMs = 15000;   // an advertised peer silent for this long is lost
        // Broadcasts (messages, queries, goodbyes) go this many times: Wi-Fi
        // doesn't retry them. Receivers take a message's first copy only.
        uint8_t broadcastCopies = 2;
    };

    struct Stats {
        uint32_t sent = 0;         // datagrams the adapter took
        uint32_t received = 0;     // datagrams, our own broadcasts excluded
        uint32_t retransmits = 0;  // datagrams sent again
        uint32_t failed = 0;       // reliable messages never acknowledged
        uint32_t duplicates = 0;   // messages received again (acknowledged, not delivered)
        uint32_t dropped = 0;      // malformed datagrams, refused or evicted messages
    };

    // The endpoint options the link uses by default: a deeper queue (a
    // broadcast's replies arrive together), and set requests applied when
    // they arrive by broadcast.
    static DatagramEndpoint::Options endpointOptions() {
        DatagramEndpoint::Options o;
        o.queueLimit = 16;
        o.broadcastSet = true;
        return o;
    }

    // `api` may be null for a client-only node.
    UdpLink(Api* api, PacketFn send, DatagramEndpoint::ClockFn clock, Options options,
            DatagramEndpoint::Options endpointOptions);
    UdpLink(Api* api, PacketFn send, DatagramEndpoint::ClockFn clock, Options options)
        : UdpLink(api, std::move(send), std::move(clock), options, endpointOptions()) {}
    UdpLink(Api* api, PacketFn send, DatagramEndpoint::ClockFn clock)
        : UdpLink(api, std::move(send), std::move(clock), Options(), endpointOptions()) {}
    ~UdpLink();
    UdpLink(const UdpLink&) = delete;
    UdpLink& operator=(const UdpLink&) = delete;

    // Starts a session. `nonce` identifies it: peers that see a new one take
    // this node as restarted. Pick it at random. Queries the networks for
    // advertising nodes.
    void start(uint32_t nonce);
    // Ends it: says goodbye when advertising, and forgets every peer (their
    // calls fail, their subscriptions and file transfers are dropped, the
    // remote nodes show offline) in the next tick().
    void stop();
    bool running() const { return running_.load(); }

    // A datagram from `from`. Any task; requests run in tick().
    void packet(const PeerAddress& from, const uint8_t* data, size_t len);
    // Retransmits, advertises, forgets silent peers, then runs the
    // endpoint's process(). Call every 10-50 ms, and when onQueued() fires.
    // Returns true if it did anything.
    bool tick();
    // Called (from packet()'s context) when the endpoint has work, so an
    // adapter can wake its worker.
    void onQueued(std::function<void()> fn) { endpoint_.onQueued(std::move(fn)); }

    // ---- discovery ------------------------------------------------------
    // The advertised name, and the device's MAC (so that a gateway that
    // also reaches it over NowTP mounts it once, docs/DESIGN.md section
    // 14.1). Set them before advertise().
    void setName(std::string_view name);
    void setDeviceId(const uint8_t mac[6]);
    // Announces this node on every local network every advertiseMs, with its
    // name and schema hash, and follows the tree like NowTpTransport's.
    bool advertise();
    // Mounts every advertising peer under `parent` (docs/DESIGN.md section
    // 14.1), and asks the networks for them.
    void mountPeers(Object& parent, uint32_t mirrorIntervalMs = 0);

    // ---- client ---------------------------------------------------------
    // Writes `bodyJson` to `path` on every node behind `to` at once: one
    // broadcast set, which no node answers and this one doesn't apply. `to`
    // is a broadcast address; empty means every local network. Replies can
    // be collected with endpoint().broadcast().
    bool broadcastSet(const PeerAddress& to, std::string_view path, std::string_view bodyJson) {
        return endpoint_.broadcast(to, Op::Set, path, bodyJson);
    }

    DatagramEndpoint& endpoint() { return endpoint_; }
    Stats stats() const;
    const Options& options() const { return options_; }

private:
    struct Peer {
        PeerAddress address;
        uint32_t nonce = 0;
        uint32_t heardMs = 0;
        uint32_t newest = 0;  // highest message id completed
        uint64_t window = 0;  // bit i: message newest - i completed
        bool any = false;     // `newest` is valid
        bool advertised = false;
    };
    struct Outgoing {
        PeerAddress to;
        uint32_t id;
        std::string data;
        uint8_t count;
        uint64_t acked;
        uint32_t probeMs;
        uint32_t backoffMs;
        uint32_t deadlineMs;
    };
    struct Partial {
        PeerAddress from;
        uint32_t nonce;
        uint32_t id;
        std::string data;
        uint64_t got;
        uint16_t chunk;
        uint8_t count;
        uint8_t flags;
        uint32_t touchedMs;
    };
    struct Inbound {
        PeerAddress from;
        std::string data;
        bool reliable;
        bool broadcast;
    };

    bool sendMessage(const PeerAddress& to, const std::string& message, Delivery delivery);
    void transmit(const PeerAddress& to, uint32_t id, const std::string& data, uint8_t count, uint64_t mask,
                  uint8_t flags);
    void sendAck(const PeerAddress& to, uint32_t id, uint8_t status, uint64_t got);
    void sendAdvert(const PeerAddress& to, uint8_t flags);
    bool sendDatagram(const PeerAddress& to, const uint8_t* data, size_t len);
    Peer* peerFor(const PeerAddress& from, uint32_t nonce, uint32_t now, bool& restarted);
    static bool seen(const Peer& p, uint32_t id);
    static void markSeen(Peer& p, uint32_t id);
    void handleData(const PeerAddress& from, Peer& peer, const uint8_t* data, size_t len, uint32_t now,
                    std::vector<Inbound>& out);
    void handleAck(const PeerAddress& from, const uint8_t* data, size_t len, uint32_t now);
    void dropPartials(const PeerAddress& from);
    bool makeRoom(size_t bytes);
    uint32_t now() const { return clock_ ? clock_() : 0; }

    PacketFn send_;
    DatagramEndpoint::ClockFn clock_;
    Options options_;
    DatagramEndpoint endpoint_;
    Api* api_;

    mutable Mutex mutex_;  // everything below; never held while calling the endpoint
    std::atomic<bool> running_{false};
    uint32_t nonce_ = 0;
    uint32_t nextId_ = 0;
    std::vector<Peer> peers_;
    std::vector<Outgoing> outgoing_;
    std::vector<Partial> partials_;
    std::vector<PeerAddress> lost_;  // peers to forget in tick()
    std::vector<uint8_t> scratch_;
    Stats stats_;
    // discovery
    std::string name_;
    PeerAddress device_;
    bool advertising_ = false;
    uint32_t hash_ = 0;
    uint32_t advertisedMs_ = 0;
};

}  // namespace tesser

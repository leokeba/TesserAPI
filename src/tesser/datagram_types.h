#pragma once

// What a DatagramEndpoint and its parts both name: peers' addresses, why they
// are lost, and the types of the endpoint's interface.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <functional>
#include <string>
#include <string_view>

#include <ArduinoJson.h>

#include "tesser/budget.h"
#include "tesser/bytes.h"
#include "tesser/request.h"

namespace tesser {

// Address of a peer on a datagram link: a MAC for NowTP, an IPv4 address
// and port for UDP. Six bytes: peer tables hold many, so there is no room
// for IPv6 (which the UDP transport doesn't speak), and longer addresses are
// cut short.
struct PeerAddress {
    static constexpr uint8_t kMaxLength = 6;
    uint8_t bytes[kMaxLength] = {};
    uint8_t length = 0;

    static PeerAddress fromBytes(const uint8_t* b, uint8_t len) {
        PeerAddress a;
        a.length = len > sizeof(a.bytes) ? sizeof(a.bytes) : len;
        memcpy(a.bytes, b, a.length);
        return a;
    }
    bool operator==(const PeerAddress& o) const { return length == o.length && memcmp(bytes, o.bytes, length) == 0; }
    bool operator!=(const PeerAddress& o) const { return !(*this == o); }
};

// Why an endpoint forgot a peer.
enum class PeerLoss : uint8_t {
    Lost,       // the link said so (NowTP's peer lost), or forgetPeer() without a reason
    Failed,     // a reliable message wasn't acknowledged in time
    Goodbye,    // it said it was leaving
    Restarted,  // it came back with a new session: whatever it held is gone
    Silent,     // an advertising peer went quiet
    Stopped,    // this side's link stopped
    AuthFailed, // the link couldn't authenticate with it: it holds another pairing key (NowTP), or none
};

// A message a link accepted but couldn't deliver (DatagramEndpoint::sendFailed()).
enum class SendFailure : uint8_t {
    Undelivered,  // not acknowledged after the link's retries: the peer is unreachable
    Rejected,     // the peer refused it: no listener on the port, or too large
    AuthFailed,   // the peer holds another pairing key, or none
};
// "lost" / "failed" / "goodbye" / "restarted" / "silent" / "stopped".
const char* toString(PeerLoss reason);

namespace detail {

// DatagramEndpoint's types (DatagramEndpoint::Options, ::ResponseHandler, ...).
struct DatagramTypes {
    // The message is passed by value: a link that keeps it (for
    // retransmissions) takes it without a copy when the caller moves it.
    using SendFn = std::function<bool(const PeerAddress& to, std::string message, Delivery delivery)>;
    using ClockFn = std::function<uint32_t()>;
    using ResponseHandler = std::function<void(Status status, JsonVariantConst body)>;
    // A reply to a broadcast request; see broadcast().
    using BroadcastHandler = std::function<void(const PeerAddress& from, Status status, JsonVariantConst body)>;

    struct Options {
        size_t queueLimit = 8;           // incoming messages waiting for process()
        bool requireReliableSet = true;  // reject set requests that didn't arrive reliably
        bool broadcastSet = false;       // but apply set requests that arrived by broadcast
        uint32_t timeoutMs = 3000;       // default client call timeout
        size_t maxCalls = 8;             // client calls in flight
        size_t maxTokens = 8;            // peers holding a token ("auth"); the oldest is forgotten
        // The largest message the link carries (NowTP's
        // Config::maxMessageSize, UdpLink's maxMessage); 0: no limit. A reply
        // that would be larger answers too_large (the client reads in
        // pieces), and a larger notification is missed ("overflow"), rather
        // than either failing on the link. Every node of a link must take
        // what the others send it: give them all the same.
        size_t maxMessage = 0;
        // Bytes of incoming messages waiting for process(), in all and from
        // any one peer (docs/DESIGN.md section 15); 0: no limit. Raised to
        // maxMessage if below it, so the largest message can always queue.
        size_t queueBytes = 20480;
        size_t queueBytesPerPeer = 16384;
        // What peers can make this node hold (docs/DESIGN.md section 14.1):
        // nodes mounted for peers that announced themselves (mountPeers());
        // past it, an offline one nothing uses is evicted, else the new peer
        // isn't mounted. Routes a peer's announcement adds to one node,
        // through this endpoint; past it, an offline one is replaced. File
        // transfers peers have open at once; past it, busy. 0: no limit.
        size_t maxMounted = 16;
        size_t maxRoutesPerNode = 4;
        size_t maxFileSessions = 4;
    };

    struct Stats {
        uint32_t requests = 0;
        uint32_t responses = 0;
        uint32_t dropped = 0;    // queue full or malformed
        uint32_t timeouts = 0;   // client calls that expired
        uint32_t unmatched = 0;  // responses for no pending call
        uint32_t evicted = 0;    // mounted nodes removed for a new one (maxMounted)
        uint32_t unmounted = 0;  // peers not mounted: maxMounted, none to evict
        uint32_t tooLarge = 0;   // requests larger than their peer takes, not sent (peerLimit())
    };

    // An "ok" reply's body as the peer's JSON text, unparsed.
    using RawHandler = std::function<void(std::string_view bodyJson)>;
    // A link that sends from a malloc()ed buffer it takes over (setOwnedSend()).
    using OwnedSendFn = std::function<bool(const PeerAddress& to, Bytes message, Delivery delivery)>;

    // Where a Builder writes an envelope: the link's own buffer when it
    // takes one over (setOwnedSend()), else a string.
    class EnvelopeOut {
    public:
        virtual ~EnvelopeOut() = default;
        // Room for `bytes` in all; false if the heap can't give it.
        virtual bool reserve(size_t bytes) = 0;
        virtual void append(const char* data, size_t len) = 0;
        // The pool `bytes` of it come from, to reserve them in.
        virtual Pool pool(size_t bytes) const { return mallocPool(bytes); }
        void append(std::string_view s) { append(s.data(), s.size()); }
    };
    using Builder = std::function<bool(uint32_t id, EnvelopeOut& out)>;
    using NotificationHandler = std::function<void(const PeerAddress& from, JsonObjectConst envelope)>;
    using AuthHandler = std::function<void(const PeerAddress& peer, Status status, Access level)>;
    using InterfaceFn = std::function<NetInterface(const PeerAddress& peer)>;
    using PeerLossHandler =
        std::function<void(const PeerAddress& peer, const PeerAddress& device, PeerLoss reason)>;
    using PeerLostHandler = std::function<void(const PeerAddress& peer, const PeerAddress& device)>;
};

}  // namespace detail
}  // namespace tesser

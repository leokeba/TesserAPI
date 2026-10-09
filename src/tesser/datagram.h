#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class Api;

// Address of a peer on a datagram link: a MAC for NowTP, IP and port for UDP.
struct PeerAddress {
    uint8_t bytes[18] = {};
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

enum class Delivery : uint8_t {
    Reliable,    // retransmitted until acknowledged (requests, responses, events)
    LatestOnly,  // a newer message supersedes older ones (state changes)
    BestEffort,  // broadcast
};

// Serves the API and acts as a client over a datagram link: one JSON envelope
// per message, in both directions. Platform-independent; a link adapter
// (NowTP, UDP, ...) feeds receive() and implements `send`.
//
// receive() only copies the message into a bounded queue, so it is safe to
// call from the link's own callback even while the link holds its locks.
// process() does the work: it handles requests under the API lock and runs
// client callbacks. Call it from a worker task or from loop().
class DatagramEndpoint {
public:
    using SendFn = std::function<bool(const PeerAddress& to, const std::string& message, Delivery delivery)>;
    using ClockFn = std::function<uint32_t()>;
    using ResponseHandler = std::function<void(Status status, JsonVariantConst body)>;

    struct Options {
        size_t queueLimit = 8;           // incoming messages waiting for process()
        bool requireReliableSet = true;  // reject set requests that didn't arrive reliably
        uint32_t timeoutMs = 3000;       // default client call timeout
        size_t maxCalls = 8;             // client calls in flight
    };

    struct Stats {
        uint32_t requests = 0;
        uint32_t responses = 0;
        uint32_t dropped = 0;    // queue full or malformed
        uint32_t timeouts = 0;   // client calls that expired
        uint32_t unmatched = 0;  // responses for no pending call
    };

    // `api` may be null for a client-only endpoint.
    DatagramEndpoint(Api* api, TransportKind kind, SendFn send, ClockFn clock, Options options);
    DatagramEndpoint(Api* api, TransportKind kind, SendFn send, ClockFn clock)
        : DatagramEndpoint(api, kind, std::move(send), std::move(clock), Options()) {}
    ~DatagramEndpoint();

    // Any task. `reliable`: the link guaranteed delivery (so it was unicast).
    void receive(const PeerAddress& from, const char* data, size_t len, bool reliable);

    // Handles queued messages and expires client calls. Returns true if it did
    // anything.
    bool process();

    // Called (from receive()'s context) whenever something is queued, so an
    // adapter can wake its worker.
    void onQueued(std::function<void()> fn) { onQueued_ = std::move(fn); }

    // ---- client side ----------------------------------------------------
    // Sends a request; `done` runs later in process() with the response, or
    // with Status::Timeout / Status::Busy. `body` is JSON text (or empty).
    // Returns false (without calling `done`) if it couldn't be sent.
    bool request(const PeerAddress& to, Op op, std::string_view path, std::string_view bodyJson,
                 ResponseHandler done, const Query& query = Query(), uint32_t timeoutMs = 0);
    bool request(const PeerAddress& to, Op op, std::string_view path, JsonVariantConst body, ResponseHandler done,
                 const Query& query = Query(), uint32_t timeoutMs = 0);

    // Drops pending calls to a peer (e.g. NowTP reported it lost); their
    // handlers get Status::Timeout.
    void forgetPeer(const PeerAddress& peer);

    Stats stats() const;
    size_t pendingCalls() const;

private:
    struct Incoming {
        PeerAddress from;
        std::string text;
        bool reliable;
    };
    struct CallSlot {
        uint32_t id;
        PeerAddress to;
        uint32_t deadline;
        ResponseHandler done;
    };

    bool sendRequest(const PeerAddress& to, uint32_t id, const std::string& envelope, ResponseHandler done,
                     uint32_t timeoutMs);
    void handle(Incoming& msg);
    void reject(const Incoming& msg, Status status, const char* message);
    void expire(uint32_t now, bool all, const PeerAddress* peer);

    Api* api_;
    TransportKind kind_;
    SendFn send_;
    ClockFn clock_;
    Options options_;
    std::function<void()> onQueued_;

    mutable Mutex mutex_;
    std::deque<Incoming> queue_;
    std::vector<CallSlot> calls_;
    uint32_t nextId_ = 1;
    Stats stats_;
};

// Builds a request envelope. `bodyJson` is inserted verbatim when not empty.
std::string buildRequestEnvelope(uint32_t id, Op op, std::string_view path, const Query& query,
                                 std::string_view bodyJson);

}  // namespace tesser

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class Api;
class Object;
class RemoteNode;
struct ClientInbox;

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
    // `owner` tags the call so cancelCalls(owner) can drop it.
    bool request(const PeerAddress& to, Op op, std::string_view path, std::string_view bodyJson,
                 ResponseHandler done, const Query& query = Query(), uint32_t timeoutMs = 0,
                 const void* owner = nullptr);
    bool request(const PeerAddress& to, Op op, std::string_view path, JsonVariantConst body, ResponseHandler done,
                 const Query& query = Query(), uint32_t timeoutMs = 0, const void* owner = nullptr);
    // Drops (without calling) the pending calls tagged with `owner`.
    void cancelCalls(const void* owner);

    // Used by RemoteNode.
    void addRemote(RemoteNode* remote);
    void removeRemote(RemoteNode* remote);
    // Used by PeerClient: notifications from `peer` are queued in `inbox`.
    void addClient(const PeerAddress& peer, std::shared_ptr<ClientInbox> inbox);
    void removeClient(const ClientInbox* inbox);

    // Requests from peers for which `fn` returns true count as authenticated
    // (see Api::authorize()). Without it, none do.
    void trust(std::function<bool(const PeerAddress&)> fn) { trust_ = std::move(fn); }

    // Notifications ("change" / "event" envelopes) from nodes this one
    // subscribed to, all of them, including those a mirrored RemoteNode
    // also consumes. Runs in process().
    using NotificationHandler = std::function<void(const PeerAddress& from, JsonObjectConst envelope)>;
    void onNotification(NotificationHandler fn) { onNotification_ = std::move(fn); }

    // The peer is gone (e.g. NowTP reported it lost): its pending calls get
    // Status::Timeout, its subscriptions are dropped and its remote nodes
    // show it offline. Safe from any task, including link callbacks: the
    // work happens in the next process().
    void forgetPeer(const PeerAddress& peer);

    // ---- discovery ------------------------------------------------------
    // Mounts every TesserAPI peer reported by peerSeen() under `parent`, an
    // object of this endpoint's API, as a remote node named after the peer
    // (docs/DESIGN.md section 14.1). `mirrorIntervalMs` > 0 also mirrors it.
    void mountPeers(Object& parent, uint32_t mirrorIntervalMs = 0);
    // A peer announced itself as a TesserAPI node (from NowTP discovery
    // metadata, ...): `name` is its advertised name, `schemaHash` the hash
    // it advertises. Marks its remote nodes online and mounts it if needed.
    // Safe from any task; takes effect in the next process().
    void peerSeen(const PeerAddress& peer, std::string_view name, uint32_t schemaHash);

    // Keeps an advertisement current: once the API's tree has changed (a
    // peer mounted, a node added, a remote node's state, see
    // schemaRevision()) and then stayed unchanged for `debounceMs`,
    // process() recomputes Api::schemaHash() and calls `publish` with it if
    // it differs from the last one. `current` is the hash already published.
    // An empty `publish` stops it.
    void autoAdvertise(std::function<void(uint32_t schemaHash)> publish, uint32_t current,
                       uint32_t debounceMs = 1000);

    Stats stats() const;
    size_t pendingCalls() const;
    uint32_t now() const { return clock_ ? clock_() : 0; }
    Api* api() const { return api_; }

private:
    struct Incoming {
        PeerAddress from;
        std::string text;
        bool reliable;
    };
    class PeerSubscriber : public Subscriber {
    public:
        PeerSubscriber(DatagramEndpoint& ep, const PeerAddress& address) : ep_(ep), peer(address) {}
        bool notify(const std::string& message, Delivery delivery) override {
            return ep_.send_ && ep_.send_(peer, message, delivery);
        }

    private:
        DatagramEndpoint& ep_;

    public:
        PeerAddress peer;
    };

    struct Seen {
        PeerAddress peer;
        std::string name;
        uint32_t schemaHash;
    };

    struct CallSlot {
        uint32_t id;
        PeerAddress to;
        uint32_t deadline;
        ResponseHandler done;
        const void* owner;
    };

    bool sendRequest(const PeerAddress& to, uint32_t id, const std::string& envelope, ResponseHandler done,
                     uint32_t timeoutMs, const void* owner);
    void handle(Incoming& msg);
    void reject(const Incoming& msg, Status status, const char* message);
    void expire(uint32_t now, bool all, const PeerAddress* peer);
    Subscriber* subscriberFor(const PeerAddress& peer, bool create);
    void dropPeer(const PeerAddress& peer);
    void applySeen(const Seen& seen);
    void markOnline(const PeerAddress& peer, bool online);
    void checkAdvertisement(uint32_t now);

    Api* api_;
    TransportKind kind_;
    SendFn send_;
    ClockFn clock_;
    Options options_;
    std::function<void()> onQueued_;
    NotificationHandler onNotification_;
    std::function<bool(const PeerAddress&)> trust_;

    mutable Mutex mutex_;
    std::vector<Incoming> queue_;  // a vector: an empty deque allocates
    std::vector<CallSlot> calls_;
    std::vector<PeerAddress> lostPeers_;  // forgetPeer() → process()
    std::vector<Seen> seenPeers_;         // peerSeen() → process()
    // Schema hashes peers advertised, for remote nodes created later.
    std::vector<std::pair<PeerAddress, uint32_t>> advertised_;  // guarded by mutex_
    Object* mountParent_ = nullptr;
    uint32_t mountMirrorMs_ = 0;
    Mutex subscribersMutex_;              // taken before the API lock, never after
    std::vector<PeerSubscriber*> subscribers_;
    std::vector<RemoteNode*> remotes_;  // guarded by mutex_
    std::vector<std::pair<PeerAddress, std::shared_ptr<ClientInbox>>> clients_;  // guarded by mutex_
    uint32_t nextId_ = 1;
    Stats stats_;
    // autoAdvertise()
    std::function<void(uint32_t)> publish_;
    uint32_t publishedHash_ = 0;
    uint32_t seenRevision_ = 0;
    uint32_t revisionAtMs_ = 0;
    uint32_t advertiseDebounceMs_ = 1000;
    bool revisionPending_ = false;
};

// Builds a request envelope. `bodyJson` is inserted verbatim when not empty.
std::string buildRequestEnvelope(uint32_t id, Op op, std::string_view path, const Query& query,
                                 std::string_view bodyJson);

// Discovery metadata announcing a TesserAPI node:
// {"tesser":<port>,"schema":"<schemaHash, 8 hex digits>"}.
std::string buildAdvertisement(uint8_t port, uint32_t schemaHash);
// Reads it; false if the metadata isn't a TesserAPI advertisement.
bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash);

}  // namespace tesser

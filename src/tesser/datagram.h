#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class Api;
class FileSession;
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
// Why an endpoint forgot a peer.
enum class PeerLoss : uint8_t {
    Lost,       // the link said so (NowTP's peer lost), or forgetPeer() without a reason
    Failed,     // a reliable message wasn't acknowledged in time
    Goodbye,    // it said it was leaving
    Restarted,  // it came back with a new session: whatever it held is gone
    Silent,     // an advertising peer went quiet
    Stopped,    // this side's link stopped
};
// "lost" / "failed" / "goodbye" / "restarted" / "silent" / "stopped".
const char* toString(PeerLoss reason);

class DatagramEndpoint {
public:
    using SendFn = std::function<bool(const PeerAddress& to, const std::string& message, Delivery delivery)>;
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
    // `broadcast`: the message was sent to every node; a request without an
    // "id" that came this way gets no reply.
    void receive(const PeerAddress& from, const char* data, size_t len, bool reliable, bool broadcast = false);

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
    // Sends any envelope, e.g. "upload" or "download" (docs/DESIGN.md
    // section 10.1.1), with its "id" replaced by the call's.
    bool request(const PeerAddress& to, JsonObjectConst envelope, ResponseHandler done, uint32_t timeoutMs = 0,
                 const void* owner = nullptr);
    // Sends an envelope whose members after its "id" are `fields`: JSON text
    // without the braces (`"op":"download","path":"/sys/backup"`).
    bool requestFields(const PeerAddress& to, std::string_view fields, ResponseHandler done, uint32_t timeoutMs = 0,
                       const void* owner = nullptr);
    // Sends a request once, unreliably, to every node behind `to` (a
    // broadcast address of the link: Delivery::BestEffort). Without
    // `onReply` the request has no "id", and no node answers. With it, each
    // reply that comes within `windowMs` (default: the call timeout) is
    // passed to `onReply` in process(), and `onReply` is called once more
    // with an empty address and Status::Timeout when the window closes.
    // Returns false (without calling `onReply`) if it couldn't be sent.
    bool broadcast(const PeerAddress& to, Op op, std::string_view path, std::string_view bodyJson,
                   BroadcastHandler onReply = nullptr, uint32_t windowMs = 0, const void* owner = nullptr);
    // Drops (without calling) the pending calls tagged with `owner`.
    void cancelCalls(const void* owner);

    // Used by RemoteNode: a node with a route through this endpoint, and
    // that route (`peer`), so it gets the peer's advertised hash.
    void addRemote(RemoteNode* remote);
    void removeRemote(RemoteNode* remote);
    void routeAdded(RemoteNode* remote, const PeerAddress& peer);
    // Used by PeerClient: notifications from `peer` are queued in `inbox`.
    void addClient(const PeerAddress& peer, std::shared_ptr<ClientInbox> inbox);
    void removeClient(const ClientInbox* inbox);

    // Requests from peers for which `fn` returns true count as authenticated
    // (see Api::authorize()), with full access. Other peers get the level the
    // API's token check gives their token (Api::tokenAccess()): the one they
    // sent with {"op":"auth","token":"..."} (docs/DESIGN.md section 10.1),
    // or the empty one, which is public unless the check opens the device to
    // anyone; at least setPeerAccess()'s.
    void trust(std::function<bool(const PeerAddress&)> fn) { trust_ = std::move(fn); }
    // The least level of a peer that isn't trusted (default public): User
    // lets anyone on the link use the device, short of admin nodes.
    void setPeerAccess(Access level) { peerAccess_ = level; }
    Access peerAccess() const { return peerAccess_.load(); }

    // Notifications ("change" / "event" envelopes) from nodes this one
    // subscribed to, all of them, including those a mirrored RemoteNode
    // also consumes. Runs in process().
    using NotificationHandler = std::function<void(const PeerAddress& from, JsonObjectConst envelope)>;
    void onNotification(NotificationHandler fn) { onNotification_ = std::move(fn); }

    // The peer is gone (e.g. NowTP reported it lost): its pending calls get
    // Status::Timeout, its subscriptions and file transfer are dropped and
    // its remote nodes show it offline. Safe from any task, including link callbacks: the
    // work happens in the next process(). `reason` goes to onPeerLost().
    void forgetPeer(const PeerAddress& peer, PeerLoss reason = PeerLoss::Lost);

    // ---- credentials (client side) ---------------------------------------
    // Presents `token` to `peer` with an "auth" envelope (docs/DESIGN.md
    // section 13.3) before the next request to it; requests wait for the
    // answer, since datagrams may arrive out of order. Presented again
    // whenever the peer may have forgotten it: after forgetPeer() (a
    // restart, a goodbye, a failed delivery, the link stopped) and after an
    // "unauthorized" reply, whose request is then sent once more. An empty
    // token stops presenting one (the peer keeps it until it forgets us).
    void setPeerToken(const PeerAddress& peer, std::string_view token);
    // The level the peer last granted our token (public if none yet).
    Access grantedAccess(const PeerAddress& peer) const;
    // Called, in process() and without the API lock, with each answer to a
    // token: Status::Ok and the level granted (public: the token wasn't
    // accepted), or the error (timeout: the peer didn't answer).
    using AuthHandler = std::function<void(const PeerAddress& peer, Status status, Access level)>;
    void onAuth(AuthHandler fn) { onAuth_ = std::move(fn); }

    // ---- interfaces -------------------------------------------------------
    // Which local interface reaches `peer` (docs/DESIGN.md section 14.3),
    // for route orders that name one: the link's adapter tells (UdpTransport
    // by the subnets of its interfaces). Any without one, or when unknown.
    using InterfaceFn = std::function<NetInterface(const PeerAddress& peer)>;
    void classifyInterfaces(InterfaceFn fn) { interfaceOf_ = std::move(fn); }
    NetInterface interfaceOf(const PeerAddress& peer) const {
        return interfaceOf_ ? interfaceOf_(peer) : NetInterface::Any;
    }

    // ---- peers ------------------------------------------------------------
    // A peer this endpoint had heard from was forgotten, for `reason`:
    // `device` is its MAC when known (a NowTP address; the "id" a UDP peer
    // advertises), else empty. Runs in process(), without the API lock. A
    // node that a client drives undoes what it did here (docs/DESIGN.md
    // section 14.4); a restarted peer is reported though it is back at once.
    using PeerLossHandler =
        std::function<void(const PeerAddress& peer, const PeerAddress& device, PeerLoss reason)>;
    using PeerLostHandler = std::function<void(const PeerAddress& peer, const PeerAddress& device)>;
    void onPeerLost(PeerLossHandler fn) { onPeerLost_ = std::move(fn); }
    void onPeerLost(PeerLostHandler fn) {
        if (!fn) return onPeerLost(PeerLossHandler());
        onPeerLost(PeerLossHandler(
            [fn](const PeerAddress& peer, const PeerAddress& device, PeerLoss) { fn(peer, device); }));
    }
    // Whether a peer of `device` has been heard and not forgotten since.
    bool deviceOnline(const PeerAddress& device) const;
    // The device behind `peer` (empty when unknown).
    PeerAddress deviceOf(const PeerAddress& peer) const;

    // ---- discovery ------------------------------------------------------
    // Mounts every TesserAPI peer reported by peerSeen() under `parent`, an
    // object of this endpoint's API, as a remote node named after the peer
    // (docs/DESIGN.md section 14.1). `mirrorIntervalMs` > 0 also mirrors it.
    // Each call adds a parent (again for the same one: a new interval).
    void mountPeers(Object& parent, uint32_t mirrorIntervalMs = 0);
    // A peer announced itself as a TesserAPI node (from NowTP discovery
    // metadata, ...): `name` is its advertised name, `schemaHash` the hash
    // it advertises, `device` the device's MAC when the link's address isn't
    // it (UDP); NowTP addresses are MACs. Marks its remote nodes online and
    // mounts it if needed. Safe from any task; takes effect in the next
    // process().
    void peerSeen(const PeerAddress& peer, std::string_view name, uint32_t schemaHash,
                  const PeerAddress& device = PeerAddress());

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
    TransportKind kind() const { return kind_; }
    uint32_t now() const { return clock_ ? clock_() : 0; }
    // The default call timeout (Options::timeoutMs).
    uint32_t timeoutMs() const { return options_.timeoutMs; }
    Api* api() const { return api_; }

private:
    struct Incoming {
        PeerAddress from;
        std::string text;
        bool reliable;
        bool broadcast;
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
        PeerAddress device;
    };

    // A peer's file transfers, dropped once idle for Config::transferIdleMs.
    struct PeerFiles {
        PeerAddress peer;
        std::unique_ptr<FileSession> session;
        uint32_t usedMs;
    };

    struct CallSlot {
        uint32_t id;
        PeerAddress to;
        uint32_t deadline;
        ResponseHandler done;
        const void* owner;
        BroadcastHandler each;  // a broadcast call: replies from any peer
        uint32_t sentMs;
        std::string text;      // to a peer with a token: kept to send it again after "unauthorized"
        bool held = false;     // waiting for the token's answer before it is sent
        bool retried = false;  // sent again once already
    };
    // A token presented to a peer (setPeerToken()).
    struct PeerAuth {
        PeerAddress peer;
        std::string token;
        enum State : uint8_t { Idle, Pending, Done } state = Idle;
        Access level = Access::Public;
        bool renew = false;  // changed while presented: present the new one when the answer comes
    };
    // Peers heard from, with their device when known (bounded).
    struct Known {
        PeerAddress peer;
        PeerAddress device;
        bool online;
        uint32_t heardMs;
    };

    bool sendRequest(const PeerAddress& to, uint32_t id, const std::string& envelope, ResponseHandler done,
                     uint32_t timeoutMs, const void* owner, bool internal = false);
    uint32_t nextCallId();
    PeerAuth* authLocked(const PeerAddress& peer);
    void sendAuth(const PeerAddress& peer, const std::string& token);
    void authDone(const PeerAddress& peer, Status status, JsonVariantConst body);
    void handle(Incoming& msg);
    void handleAuth(JsonObjectConst envelope, const Incoming& msg, Reply& reply);
    Access levelOf(const PeerAddress& peer);
    void reject(const Incoming& msg, Status status, const char* message);
    void expire(uint32_t now, bool all, const PeerAddress* peer);
    Subscriber* subscriberFor(const PeerAddress& peer, bool create);
    void dropPeer(const PeerAddress& peer, PeerLoss reason);
    void applySeen(const Seen& seen);
    void mount(Object& parent, uint32_t mirrorMs, const Seen& seen, const PeerAddress& device);
    void markOnline(const PeerAddress& peer, bool online);
    // Under mutex_.
    Known* knownLocked(const PeerAddress& peer);
    Known& learnLocked(const PeerAddress& peer, uint32_t now);
    void checkAdvertisement(uint32_t now);
    void handleFile(JsonObjectConst envelope, const Incoming& msg, const Client& client);
    void dropFiles(const PeerAddress* peer, uint32_t now);

    Api* api_;
    TransportKind kind_;
    SendFn send_;
    ClockFn clock_;
    Options options_;
    std::function<void()> onQueued_;
    NotificationHandler onNotification_;
    PeerLossHandler onPeerLost_;
    InterfaceFn interfaceOf_;
    AuthHandler onAuth_;
    std::function<bool(const PeerAddress&)> trust_;
    std::atomic<Access> peerAccess_{Access::Public};

    mutable Mutex mutex_;
    std::vector<Incoming> queue_;  // a vector: an empty deque allocates
    std::vector<CallSlot> calls_;
    std::vector<std::pair<PeerAddress, PeerLoss>> lostPeers_;  // forgetPeer() → process()
    std::vector<Seen> seenPeers_;         // peerSeen() → process()
    // Schema hashes peers advertised, for remote nodes created later.
    std::vector<std::pair<PeerAddress, uint32_t>> advertised_;  // guarded by mutex_
    std::vector<std::pair<Object*, uint32_t>> mounts_;  // mountPeers(): parent, mirror interval
    Mutex subscribersMutex_;              // taken before the API lock, never after
    std::vector<PeerSubscriber*> subscribers_;
    std::vector<RemoteNode*> remotes_;  // guarded by mutex_
    std::vector<PeerFiles> files_;      // used in process() only
    std::vector<std::pair<PeerAddress, std::shared_ptr<ClientInbox>>> clients_;  // guarded by mutex_
    std::vector<std::pair<PeerAddress, std::string>> tokens_;  // from "auth", oldest first; guarded by mutex_
    std::vector<Known> known_;  // guarded by mutex_
    std::vector<PeerAuth> auths_;  // guarded by mutex_
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

// Calls `lost` when a device is gone from every endpoint in `endpoints` that
// heard it (a peer without a known device: when that peer is), so a node
// reached over UDP and NowTP isn't taken as lost while one of them still
// works; and when it restarted (PeerLoss::Restarted), since whatever it
// held went with its previous run, though it is back at once. Takes the
// endpoints' onPeerLost(). A device lost on two endpoints at once may be
// reported twice.
void watchDevices(std::vector<DatagramEndpoint*> endpoints, DatagramEndpoint::PeerLossHandler lost);
void watchDevices(std::vector<DatagramEndpoint*> endpoints, DatagramEndpoint::PeerLostHandler lost);

// Builds a request envelope; `id` 0 leaves the "id" out. `bodyJson` is
// inserted verbatim when not empty.
std::string buildRequestEnvelope(uint32_t id, Op op, std::string_view path, const Query& query,
                                 std::string_view bodyJson);

// Discovery metadata announcing a TesserAPI node:
// {"tesser":<port>,"schema":"<schemaHash, 8 hex digits>"}.
std::string buildAdvertisement(uint8_t port, uint32_t schemaHash);
// Reads it; false if the metadata isn't a TesserAPI advertisement.
bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash);

}  // namespace tesser

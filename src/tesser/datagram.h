#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/budget.h"
#include "tesser/bytes.h"
#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class Api;
class EnvelopeReply;
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

class DatagramEndpoint {
public:
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

    // `api` may be null for a client-only endpoint.
    DatagramEndpoint(Api* api, TransportKind kind, SendFn send, ClockFn clock, Options options);
    DatagramEndpoint(Api* api, TransportKind kind, SendFn send, ClockFn clock)
        : DatagramEndpoint(api, kind, std::move(send), std::move(clock), Options()) {}
    ~DatagramEndpoint();

    // Any task. `reliable`: the link guaranteed delivery (so it was unicast).
    // `broadcast`: the message was sent to every node; a request without an
    // "id" that came this way gets no reply. False only with linkRefuses():
    // the message wasn't taken (no memory, or the queue is full) and nothing
    // answered it, so the link must refuse it, for the sender to send it again.
    bool receive(const PeerAddress& from, const char* data, size_t len, bool reliable, bool broadcast = false);
    // Same, taking the message (a link's reassembly buffer) without a copy.
    bool receive(const PeerAddress& from, std::string&& text, bool reliable, bool broadcast = false);
    // Same, taking `buffer`, allocated with malloc (freed with free(), here,
    // even when refused): a link that hands its reassembly buffer over
    // (NowTP's Message::buffer), so a large message is never on the heap twice.
    bool receiveOwned(const PeerAddress& from, void* buffer, size_t len, bool reliable, bool broadcast = false);
    // The link can refuse a reliable unicast message from its receive
    // callback, and its sender then sends it again (NowTP's Message::refused,
    // f09a571 and later). One the endpoint can't take is then refused
    // (receive() returns false) rather than answered busy, or, a reply,
    // failing its call (docs/DESIGN.md section 15).
    void linkRefuses(bool on) { linkRefuses_ = on; }

    // Handles queued messages and expires client calls. Returns true if it did
    // anything.
    bool process();

    // A link that sends from a malloc()ed buffer it takes over (NowTP's
    // sendBuffer()): replies are built in one, `slack` bytes spare at its
    // end, and handed over whole rather than copied (docs/DESIGN.md
    // section 15). `fn` takes the buffer in every case. Before begin().
    using OwnedSendFn = std::function<bool(const PeerAddress& to, Bytes message, Delivery delivery)>;
    void setOwnedSend(OwnedSendFn fn, size_t slack) {
        ownedSend_ = std::move(fn);
        ownedSlack_ = slack;
    }

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
    // Same, but an "ok" reply's body reaches `raw` as the peer's JSON text,
    // unparsed, so a gateway relays it without holding it twice (parsed and
    // rewritten); other replies, and timeouts, reach `done`.
    using RawHandler = std::function<void(std::string_view bodyJson)>;
    bool requestRaw(const PeerAddress& to, Op op, std::string_view path, JsonVariantConst body, RawHandler raw,
                    ResponseHandler done, const Query& query = Query(), uint32_t timeoutMs = 0,
                    const void* owner = nullptr);
    // Sends any envelope, e.g. "upload" or "download" (docs/DESIGN.md
    // section 10.1.1), with its "id" replaced by the call's.
    bool request(const PeerAddress& to, JsonObjectConst envelope, ResponseHandler done, uint32_t timeoutMs = 0,
                 const void* owner = nullptr);
    // Sends an envelope whose members after its "id" are `fields`: JSON text
    // without the braces (`"op":"download","path":"/sys/backup"`).
    bool requestFields(const PeerAddress& to, std::string_view fields, ResponseHandler done, uint32_t timeoutMs = 0,
                       const void* owner = nullptr);
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
    // Sends the envelope `build` writes for the call's id into `out`, which
    // it builds again rather than have it kept, should it go again (to a peer
    // with a token, after "unauthorized"); over a link that takes buffers
    // (NowTP), it is built in the one the link sends from. So a large one (a
    // relayed file chunk) is never held twice (docs/DESIGN.md section 15).
    // False from `build` (no memory for it): not sent, false. With `raw`,
    // an "ok" reply's body goes to it unparsed, as for requestRaw().
    using Builder = std::function<bool(uint32_t id, EnvelopeOut& out)>;
    bool requestBuilt(const PeerAddress& to, Builder build, ResponseHandler done, uint32_t timeoutMs = 0,
                      const void* owner = nullptr, RawHandler raw = nullptr);
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
    // Whether a call made for `owner` is in flight.
    bool hasCalls(const void* owner) const;
    // Peers holding a subscription here, and peers with a file transfer open.
    size_t peerSubscribers() const;
    size_t fileSessions() const { return files_.size(); }

    // Used by RemoteNode: a node with a route through this endpoint, and
    // that route (`peer`), so it gets the peer's advertised hash.
    // removeRemote() (its destructor) waits while another task calls the
    // node from this endpoint (process(), a token's answer): a node mustn't
    // be destroyed under the API lock, which that call may be waiting for.
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
    // forgetPeer() for every peer it knows: heard from, called, or holding
    // a subscription. For a link that stops (PeerLoss::Stopped).
    void forgetAll(PeerLoss reason);

    // The link stopped hearing the peer (NowTP's discovery lost it: no
    // announcement for a while). A peer this endpoint heard from within
    // kLossGraceMs is only marked offline: its calls, subscriptions, token
    // and file transfer stay, and its next message cancels the loss. Once it
    // has been silent that long, it is forgotten as by forgetPeer(). Safe
    // from any task, like forgetPeer() (docs/DESIGN.md section 10.3).
    void linkLost(const PeerAddress& peer);
    static constexpr uint32_t kLossGraceMs = 5000;
    // Whether linkLost() was deferred for the peer, which hasn't been heard since.
    bool lossPending(const PeerAddress& peer) const;

    // ---- link failures ----------------------------------------------------
    // The link couldn't deliver a message it had accepted (its completion
    // callback): `callId` is callIdOf() the message, 0 for one that isn't a
    // request of ours. Undelivered and AuthFailed forget the peer at once,
    // as forgetPeer() (PeerLoss::Failed, PeerLoss::AuthFailed), failing its
    // calls with reason send_failed or auth_failed instead of letting them
    // time out; Rejected fails that call alone (Status::Internal, reason
    // rejected). Safe from any task, including link callbacks: the work
    // happens in the next process(). docs/DESIGN.md section 10.3.
    void sendFailed(const PeerAddress& peer, uint32_t callId, SendFailure failure);
    // The call id of a request this endpoint sent ({"id":N,"op":...}), or 0.
    static uint32_t callIdOf(std::string_view message);

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
    // `protocol` is the version it advertises (docs/DESIGN.md section
    // 10.1.2): 1 for an advertisement that names none, 0 when unknown.
    // `maxMessage` the largest message it takes, as it advertises it (0:
    // not said).
    void peerSeen(const PeerAddress& peer, std::string_view name, uint32_t schemaHash,
                  const PeerAddress& device = PeerAddress(), uint8_t protocol = 0, size_t maxMessage = 0);

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
    // The largest reply: the API's Config::maxResponse within the link's
    // Options::maxMessage.
    size_t replyLimit() const;
    // The largest message `peer` takes (docs/DESIGN.md section 14.1): the
    // least of what it advertises, what the link knows of it
    // (setPeerLimit()) and this link's own Options::maxMessage; 0 when none
    // is known. Requests larger aren't sent; replies to it are kept within
    // it (replyLimitFor()).
    size_t peerLimit(const PeerAddress& peer) const;
    size_t replyLimitFor(const PeerAddress& peer) const;
    // What the link itself knows of a peer's limit (NowTP's
    // peerMaxMessageSize()), 0 when it doesn't. Set before use.
    void setPeerLimit(std::function<size_t(const PeerAddress&)> fn) { peerLimitFn_ = std::move(fn); }
    // This link's own limit (Options::maxMessage; 0: none), which nodes
    // advertise.
    size_t maxMessage() const { return options_.maxMessage; }

private:
    struct FreeDeleter {
        void operator()(char* p) const { free(p); }
    };
    // A message waiting for process(): its text, or a buffer the link
    // handed over (receiveOwned()).
    struct Incoming {
        PeerAddress from;
        std::string text;
        bool reliable = false;
        bool broadcast = false;
        std::unique_ptr<char, FreeDeleter> owned;
        size_t ownedLen = 0;
        Reservation held;  // its bytes, in incoming_
        std::string_view view() const { return owned ? std::string_view(owned.get(), ownedLen) : std::string_view(text); }
    };
    class PeerSubscriber : public Subscriber {
    public:
        PeerSubscriber(DatagramEndpoint& ep, const PeerAddress& address) : ep_(ep), peer(address) {}
        bool notify(const std::string& message, Delivery delivery) override {
            if (ep_.options_.maxMessage && message.size() > ep_.options_.maxMessage) return false;
            return ep_.send_ && ep_.send_(peer, message, delivery);
        }
        // Api::tokensChanged(): a peer that isn't trusted is at its token's level.
        bool recheckLevel(Access& level) override {
            if (ep_.trust_ && ep_.trust_(peer)) return false;
            level = ep_.levelOf(peer);
            return true;
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
        uint8_t protocol;
        size_t maxMessage;
    };
    struct Advertised {
        PeerAddress peer;
        uint32_t schemaHash;
        uint8_t protocol;
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
        RawHandler raw;        // requestRaw(): an "ok" reply's body, unparsed
        std::string text;      // to a peer with a token: kept to send it again after "unauthorized"
        std::shared_ptr<Builder> rebuild;  // or how to make it again (requestBuilt())
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
        bool authFailed = false;  // reported as such; cleared when heard
        size_t maxMessage = 0;    // as it advertises (0: not said)
        bool lossPending = false; // linkLost() deferred: cleared when heard
    };
    struct Failure {
        PeerAddress peer;
        uint32_t callId;
        SendFailure failure;
    };

    // A held call to send once its token is answered: built then.
    struct Held {
        uint32_t id;
        std::string text;
        std::shared_ptr<Builder> rebuild;
    };
    // ---- datagram.cpp: taking messages, serving peers ------------------------
    void handle(Incoming& msg);
    bool queue(Incoming&& msg);
    void handleAuth(JsonObjectConst envelope, const Incoming& msg, Reply& reply);
    // A request's client from `peer`: transport, address and device, before
    // its level is decided.
    Client clientOf(const PeerAddress& peer) const;
    Access levelOf(const PeerAddress& peer);
    void reject(const PeerAddress& from, std::string_view text, bool broadcast, Status status, Reason reason,
                const char* message);
    void refuse(const PeerAddress& from, std::string_view text, bool broadcast, Reason reason);
    static Options checked(Options o);
    // Replies to `to` hand their buffer to ownedSend_, when there is one.
    void ownReplies(EnvelopeReply& reply, const PeerAddress& to, bool silent);
    // Holds `len` bytes from `from` in incoming_ (`heap`: about to be
    // allocated); empty, with a busy reply to a request, when refused.
    Reservation hold(const PeerAddress& from, std::string_view text, bool reliable, bool broadcast, bool heap);
    bool refusable(bool reliable, bool broadcast) const { return linkRefuses_ && reliable && !broadcast; }
    Subscriber* subscriberFor(const PeerAddress& peer, bool create);
    void handleFile(JsonObjectConst envelope, const Incoming& msg, const Client& client);
    void dropFiles(const PeerAddress* peer, uint32_t now);

    // ---- datagram_calls.cpp: calls to peers, tokens presented to them -------
    bool sendRequest(const PeerAddress& to, uint32_t id, std::string envelope, ResponseHandler done,
                     uint32_t timeoutMs, const void* owner, bool internal = false, RawHandler raw = nullptr,
                     std::shared_ptr<Builder> rebuild = nullptr, Bytes owned = Bytes());
    // A held call's text, built again from its builder.
    static std::string rebuilt(const Builder& build, uint32_t id);
    bool takeRaw(const Incoming& msg);
    void handleResponse(JsonObjectConst envelope, const PeerAddress& from);
    uint32_t nextCallId();
    PeerAuth* authLocked(const PeerAddress& peer);
    void sendAuth(const PeerAddress& peer, const std::string& token);
    void authDone(const PeerAddress& peer, Status status, JsonVariantConst body);
    // `reason`: why the calls of a forgotten peer fail (None: a plain timeout).
    void expire(uint32_t now, bool all, const PeerAddress* peer, Reason reason = Reason::None);
    void failCall(uint32_t id, Status status, Reason reason, const char* message);

    // ---- datagram_peers.cpp: peers heard and lost, nodes routed and mounted --
    void dropPeer(const PeerAddress& peer, PeerLoss reason);
    void unreachable(const PeerAddress& peer);
    void applySeen(const Seen& seen);
    void mount(Object& parent, uint32_t mirrorMs, const Seen& seen, const PeerAddress& device);
    bool roomToMount();
    void markOnline(const PeerAddress& peer, bool online);
    // Under mutex_.
    Known* knownLocked(const PeerAddress& peer);
    Known& learnLocked(const PeerAddress& peer, uint32_t now);
    void checkAdvertisement(uint32_t now);
    bool deferLoss(const PeerAddress& peer);
    void dropDeferred(uint32_t now);

    Api* api_;
    TransportKind kind_;
    std::function<size_t(const PeerAddress&)> peerLimitFn_;
    ByteBudget incoming_;  // queued messages (Options::queueBytes)
    SendFn send_;
    OwnedSendFn ownedSend_;
    std::atomic<bool> linkRefuses_{false};
    size_t ownedSlack_ = 0;
    ClockFn clock_;
    Options options_;
    std::function<void()> onQueued_;
    NotificationHandler onNotification_;
    PeerLossHandler onPeerLost_;
    InterfaceFn interfaceOf_;
    AuthHandler onAuth_;
    std::function<bool(const PeerAddress&)> trust_;
    std::atomic<Access> peerAccess_{Access::Public};

    mutable Mutex mutex_{kRankEndpoint, "endpoint"};
    std::vector<Incoming> queue_;  // a vector: an empty deque allocates
    std::vector<CallSlot> calls_;
    std::vector<std::pair<PeerAddress, PeerLoss>> lostPeers_;  // forgetPeer() → process()
    std::vector<PeerAddress> unheardPeers_;                    // linkLost() → process()
    std::vector<Failure> failures_;                            // sendFailed() → process()
    std::vector<Seen> seenPeers_;         // peerSeen() → process()
    // Schema hashes peers advertised, for remote nodes created later.
    std::vector<Advertised> advertised_;  // guarded by mutex_
    std::vector<std::pair<Object*, uint32_t>> mounts_;  // mountPeers(): parent, mirror interval
    mutable Mutex subscribersMutex_{kRankSubscribers, "endpoint subscribers"};  // taken before the API lock, never after
    std::vector<PeerSubscriber*> subscribers_;
    std::atomic<bool> idleCheck_{false};  // a subscriber kept for a queued request
    // Deletes subscribers (`only`, or all) without subscriptions or queued
    // requests.
    void dropIdleSubscribers(const Subscriber* only);
    std::vector<RemoteNode*> remotes_;  // guarded by mutex_
    // Remote nodes called without mutex_ (they call back into the endpoint,
    // the API and the application): pinned under it while the call lasts. A
    // pinned node isn't evicted, and its destructor (removeRemote()) waits
    // for the pin to go (docs/DESIGN.md section 9).
    class Pinned {
    public:
        explicit Pinned(DatagramEndpoint& endpoint) : endpoint_(endpoint) {}
        ~Pinned();
        // Under mutex_: pins every remote node, or `node`.
        void all();
        void add(RemoteNode* node);
        std::vector<RemoteNode*> nodes;

        Pinned(const Pinned&) = delete;
        Pinned& operator=(const Pinned&) = delete;

    private:
        DatagramEndpoint& endpoint_;
    };
    bool pinnedLocked(const RemoteNode* node) const;
    bool pinned(const RemoteNode* node) const;
    void unlistLocked(const RemoteNode* remote);  // out of remotes_
    std::vector<RemoteNode*> pinned_;  // one entry per pin; guarded by mutex_
    Signal unpinned_;                  // given as pins go, for removeRemote()
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

// A peer as a ByteBudget key (per-peer limits).
uintptr_t budgetKey(const PeerAddress& peer);

// The text of member `key` of a JSON object (`json` must be valid JSON):
// false if it has none.
bool jsonMember(std::string_view json, std::string_view key, std::string_view& out);
// Calls `each` with every top-level member's key of a JSON object, until it
// returns false; false if the text isn't an object.
bool jsonKeys(std::string_view json, const std::function<bool(std::string_view key)>& each);

// Builds a request envelope; `id` 0 leaves the "id" out. `bodyJson` is
// inserted verbatim when not empty.
std::string buildRequestEnvelope(uint32_t id, Op op, std::string_view path, const Query& query,
                                 std::string_view bodyJson);

// Discovery metadata announcing a TesserAPI node:
// {"tesser":<port>,"schema":"<schemaHash, 8 hex digits>","v":<protocol>},
// and "max":<maxMessage>, the largest message the node takes, unless 0.
std::string buildAdvertisement(uint8_t port, uint32_t schemaHash, size_t maxMessage = 0);
// Reads it; false if the metadata isn't a TesserAPI advertisement.
// `protocol` is 1 when it names none (docs/DESIGN.md section 10.1.2),
// `maxMessage` 0.
bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash);
bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash, uint8_t& protocol);
bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash, uint8_t& protocol,
                        size_t& maxMessage);

}  // namespace tesser

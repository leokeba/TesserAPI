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

#include "tesser/datagram_types.h"
#include "tesser/deadline.h"
#include "tesser/detail/datagram_parts.h"
#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class Api;
struct ClientInbox;

// Serves the API and acts as a client over a datagram link: one JSON envelope
// per message, in both directions. Platform-independent; a link adapter
// (NowTP, UDP, ...) feeds receive() and implements `send`.
//
// receive() only copies the message into a bounded queue, so it is safe to
// call from the link's own callback even while the link holds its locks.
// process() does the work: it handles requests under the API lock and runs
// client callbacks. Call it from a worker task or from loop().
class DatagramEndpoint : public detail::DatagramTypes {
public:
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
    // When process() next has something to do, into `d`: now with messages
    // waiting, else its calls' timeouts and its remote nodes' timers. For a
    // worker that sleeps until then (UdpLink::nextDelayMs()). What adds
    // to it from another task calls wake(). From the task that calls process().
    void nextTick(Deadline& d);
    // How soon a tick looks again at what it can only retry.
    static constexpr uint32_t kRetryMs = 20;

    // A link that sends from a malloc()ed buffer it takes over (NowTP's
    // sendBuffer()): replies are built in one, `slack` bytes spare at its
    // end, and handed over whole rather than copied (docs/DESIGN.md
    // section 15). `fn` takes the buffer in every case. Before begin().
    void setOwnedSend(OwnedSendFn fn, size_t slack) {
        ownedSend_ = std::move(fn);
        ownedSlack_ = slack;
    }

    // Called whenever something is queued, or wake() is, so an adapter can
    // wake its worker: from receive()'s context, or any task's, possibly
    // under the API lock or this endpoint's. It should do no more than that.
    void onQueued(std::function<void()> fn) { onQueued_ = std::move(fn); }
    // A worker sleeping until nextTick() must look again.
    void wake() {
        if (onQueued_) onQueued_();
    }

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
    // Sends the envelope `build` writes for the call's id into `out`, which
    // it builds again rather than have it kept, should it go again (to a peer
    // with a token, after "unauthorized"); over a link that takes buffers
    // (NowTP), it is built in the one the link sends from. So a large one (a
    // relayed file chunk) is never held twice (docs/DESIGN.md section 15).
    // False from `build` (no memory for it): not sent, false. With `raw`,
    // an "ok" reply's body goes to it unparsed, as for requestRaw().
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
    void cancelCalls(const void* owner) { calls_.cancel(owner); }
    // Whether a call made for `owner` is in flight.
    bool hasCalls(const void* owner) const { return calls_.has(owner); }
    // Peers holding a subscription here, and peers with a file transfer open.
    size_t peerSubscribers() const { return server_.subscribers(); }
    size_t fileSessions() const { return server_.fileSessions(); }

    // Used by RemoteNode: a node with a route through this endpoint, and
    // that route (`peer`), so it gets the peer's advertised hash.
    // removeRemote() (its destructor) waits while another task calls the
    // node from this endpoint (process(), a token's answer): a node mustn't
    // be destroyed under the API lock, which that call may be waiting for.
    void addRemote(RemoteNode* remote) { mounts_.addRemote(remote); }
    void removeRemote(RemoteNode* remote) { mounts_.removeRemote(remote); }
    void routeAdded(RemoteNode* remote, const PeerAddress& peer) { mounts_.routeAdded(remote, peer); }
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
    // `idMask`: a link that carries only part of the id (NowTP's 32-bit
    // completion tag, with a peer in it) gives the bits it has; the call to
    // that peer whose id matches them is the one.
    void sendFailed(const PeerAddress& peer, uint32_t callId, SendFailure failure, uint32_t idMask = 0xffffffffu);
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
    void setPeerToken(const PeerAddress& peer, std::string_view token) { calls_.setToken(peer, token); }
    // The level the peer last granted our token (public if none yet).
    Access grantedAccess(const PeerAddress& peer) const { return calls_.granted(peer); }
    // Called, in process() and without the API lock, with each answer to a
    // token: Status::Ok and the level granted (public: the token wasn't
    // accepted), or the error (timeout: the peer didn't answer).
    void onAuth(AuthHandler fn) { onAuth_ = std::move(fn); }

    // ---- interfaces -------------------------------------------------------
    // Which local interface reaches `peer` (docs/DESIGN.md section 14.3),
    // for route orders that name one: the link's adapter tells (UdpTransport
    // by the subnets of its interfaces). Any without one, or when unknown.
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
    void mountPeers(Object& parent, uint32_t mirrorIntervalMs = 0) { mounts_.mountPeers(parent, mirrorIntervalMs); }
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
                  const PeerAddress& device = PeerAddress(), uint8_t protocol = 0, size_t maxMessage = 0) {
        mounts_.peerSeen(peer, name, schemaHash, device, protocol, maxMessage);
    }

    // Keeps an advertisement current: once the API's tree has changed (a
    // peer mounted, a node added, a remote node's state, see
    // schemaRevision()) and then stayed unchanged for `debounceMs`,
    // process() recomputes Api::schemaHash() and calls `publish` with it if
    // it differs from the last one. `current` is the hash already published.
    // An empty `publish` stops it.
    void autoAdvertise(std::function<void(uint32_t schemaHash)> publish, uint32_t current,
                       uint32_t debounceMs = 1000);

    Stats stats() const;
    size_t pendingCalls() const { return calls_.pending(); }
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
    friend class detail::CallTable;
    friend class detail::Mounter;
    friend class detail::EndpointServer;
    using Incoming = detail::Incoming;

    struct Failure {
        PeerAddress peer;
        uint32_t callId;
        SendFailure failure;
    };

    // ---- datagram.cpp: taking messages, process() ----------------------------
    void handle(Incoming& msg);
    bool queue(Incoming&& msg);
    static Options checked(Options o);
    // Holds `len` bytes from `from` in incoming_ (`heap`: about to be
    // allocated); empty, with a busy reply to a request, when refused.
    Reservation hold(const PeerAddress& from, std::string_view text, bool reliable, bool broadcast, bool heap);
    bool refusable(bool reliable, bool broadcast) const { return linkRefuses_ && reliable && !broadcast; }

    // ---- datagram_peers.cpp: peers heard and lost ----------------------------
    void dropPeer(const PeerAddress& peer, PeerLoss reason);
    void unreachable(const PeerAddress& peer);
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
    std::vector<std::pair<PeerAddress, PeerLoss>> lostPeers_;  // forgetPeer() → process()
    std::vector<PeerAddress> unheardPeers_;                    // linkLost() → process()
    std::vector<Failure> failures_;                            // sendFailed() → process()
    std::vector<std::pair<PeerAddress, std::shared_ptr<ClientInbox>>> clients_;  // guarded by mutex_
    Stats stats_;
    // autoAdvertise()
    std::function<void(uint32_t)> publish_;
    uint32_t publishedHash_ = 0;
    uint32_t seenRevision_ = 0;
    uint32_t revisionAtMs_ = 0;
    uint32_t advertiseDebounceMs_ = 1000;
    bool revisionPending_ = false;

    // Its parts (detail/datagram_parts.h), under mutex_.
    detail::PeerTable peers_;
    detail::CallTable calls_;
    detail::Mounter mounts_;
    detail::EndpointServer server_;
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

namespace detail {
// What an advertisement says, NowTP's metadata or UdpLink's
// ({"name":"...","schema":"...","id":"<MAC, 12 hex digits>","v":...,"max":...}):
// one parser for both. Members it lacks keep these values.
struct Advert {
    int port = -1;  // "tesser"
    std::string name;
    bool hasName = false;
    bool hasSchema = false;
    uint32_t schemaHash = 0;
    uint8_t protocol = 1;
    size_t maxMessage = 0;
    PeerAddress device;  // "id", when it is a MAC
};
bool parseAdvert(std::string_view text, Advert& out);
}  // namespace detail

}  // namespace tesser

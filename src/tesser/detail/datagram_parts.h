#pragma once

// The parts of a DatagramEndpoint (docs/DESIGN.md section 16), which share
// its one mutex: what they hold changes together, a FreeRTOS mutex costs
// about 90 bytes, and a peer table is held by every link. Each part keeps
// what it says here and reaches the others through the endpoint.

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/datagram_types.h"
#include "tesser/deadline.h"
#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class DatagramEndpoint;
class EnvelopeReply;
class FileSession;
class Object;
class RemoteNode;

namespace detail {

struct FreeDeleter {
    void operator()(char* p) const { free(p); }
};

// A message waiting for process(): its text, or a buffer the link handed
// over (receiveOwned()).
struct Incoming {
    PeerAddress from;
    std::string text;
    bool reliable = false;
    bool broadcast = false;
    std::unique_ptr<char, FreeDeleter> owned;
    size_t ownedLen = 0;
    Reservation held;  // its bytes, in the endpoint's queue budget
    std::string_view view() const { return owned ? std::string_view(owned.get(), ownedLen) : std::string_view(text); }
};

// A peer, as a RemoteNode's route's endpoint told it (peerSeen()).
struct SeenPeer {
    PeerAddress peer;
    std::string name;
    uint32_t schemaHash;
    PeerAddress device;
    uint8_t protocol;
    size_t maxMessage;
};

// What the endpoint knows of each peer, one record per address, under the
// endpoint's mutex. A record has up to four parts, each with its own bound
// and kept for as long as it lasts, so a peer forgotten as one is still
// there as another: heard (known), what it advertised, the token it gave
// here ("auth"), and the token this node presents to it (setPeerToken()).
// Tokens are few and strings are large, so they are kept apart.
class PeerTable {
public:
    // A token this node presents to a peer.
    struct Presented {
        std::string token;
        enum State : uint8_t { Idle, Pending, Done } state = Idle;
        Access level = Access::Public;
        bool renew = false;  // changed while presented: present the new one when the answer comes
    };
    struct Tokens {
        std::string held;  // the peer's, from "auth"
        uint32_t heldAt = 0;  // the table's count then: the oldest goes first
        bool holds = false;
        bool presents = false;
        Presented presented;
    };
    struct Peer {
        PeerAddress peer;
        PeerAddress device;  // when known
        bool known = false;  // heard from: the fields down to maxMessage count
        bool online = false;
        bool authFailed = false;   // reported as such; cleared when heard
        bool lossPending = false;  // linkLost() deferred: cleared when heard
        bool advertised = false;   // the fields below count
        uint8_t protocol = 0;
        uint32_t heardMs = 0;
        size_t maxMessage = 0;  // as it advertises (0: not said)
        uint32_t schemaHash = 0;
        uint32_t advertisedAt = 0;  // the table's count then: the oldest goes first
        Tokens* tokens = nullptr;   // owned; a plain pointer keeps the vector's code small
    };

    explicit PeerTable(TransportKind kind) : kind_(kind) {}
    ~PeerTable();
    PeerTable(const PeerTable&) = delete;
    PeerTable& operator=(const PeerTable&) = delete;

    const std::vector<Peer>& peers() const { return peers_; }
    Peer* find(const PeerAddress& peer);
    const Peer* find(const PeerAddress& peer) const;
    // Heard from, and not forgotten since.
    Peer* known(const PeerAddress& peer);
    const Peer* known(const PeerAddress& peer) const;
    // Remembers a peer heard from; the one silent longest is forgotten
    // when too many are.
    Peer& learn(const PeerAddress& peer, uint32_t now);
    void setAdvertised(const PeerAddress& peer, uint32_t schemaHash, uint8_t protocol);

    // The token `peer` gave here: false, and `out` left, without one.
    bool heldToken(const PeerAddress& peer, std::string& out) const;
    // Replaces it (empty: none); past `most` tokens, the oldest is forgotten.
    void holdToken(const PeerAddress& peer, std::string_view token, size_t most);
    void dropHeldToken(const PeerAddress& peer);

    Presented* presented(const PeerAddress& peer);
    const Presented* presented(const PeerAddress& peer) const;
    Presented& present(const PeerAddress& peer);
    void stopPresenting(const PeerAddress& peer);

private:
    Peer& record(const PeerAddress& peer);
    Tokens& tokensOf(const PeerAddress& peer);
    // Drops what is left of the record if that was its last part.
    void trim(Peer& p);
    void trimTokens(Peer& p);

    std::vector<Peer> peers_;
    uint32_t count_ = 0;  // advertisements and tokens taken, to order them
    TransportKind kind_;
};

// Calls to peers: requests and their replies, timeouts and link failures,
// and presenting tokens before them (docs/DESIGN.md sections 10.3 and 13.3).
class CallTable {
public:
    using ResponseHandler = DatagramTypes::ResponseHandler;
    using BroadcastHandler = DatagramTypes::BroadcastHandler;
    using RawHandler = DatagramTypes::RawHandler;
    using Builder = DatagramTypes::Builder;

    // A request to a peer with a token is kept, to go again after
    // "unauthorized", up to this size (unless it waits for the token).
    static constexpr size_t kKeptEnvelope = 512;
    struct Slot {
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

    explicit CallTable(DatagramEndpoint& endpoint) : ep_(endpoint) {}

    uint32_t nextId();
    bool send(const PeerAddress& to, uint32_t id, std::string envelope, ResponseHandler done, uint32_t timeoutMs,
              const void* owner, bool internal = false, RawHandler raw = nullptr,
              std::shared_ptr<Builder> rebuild = nullptr, Bytes owned = Bytes());
    bool broadcast(const PeerAddress& to, std::string_view path, Op op, std::string_view bodyJson,
                   BroadcastHandler onReply, uint32_t windowMs, const void* owner);
    // An "ok" reply to a requestRaw() call: false if it isn't one, for
    // handle() to parse it.
    bool takeRaw(const PeerAddress& from, std::string_view text);
    void handleResponse(JsonObjectConst envelope, const PeerAddress& from);

    void cancel(const void* owner);
    bool has(const void* owner) const;
    size_t pending() const;
    // The calls, for ~DatagramEndpoint to drop.
    std::vector<Slot> takeAll() { return std::move(calls_); }
    // Under the endpoint's mutex.
    const std::vector<Slot>& calls() const { return calls_; }
    void nextTick(Deadline& d) const;
    // Ends the calls past their deadline, or `all`, or `peer`'s. `reason`:
    // why the calls of a forgotten peer fail (None: a plain timeout).
    void expire(uint32_t now, bool all, const PeerAddress* peer, Reason reason = Reason::None);
    void fail(uint32_t id, Status status, Reason reason, const char* message);
    // The id of the call to `peer` whose id matches `partial` under `mask`,
    // or 0. Under the endpoint's mutex.
    uint32_t matching(const PeerAddress& peer, uint32_t partial, uint32_t mask) const;

    // ---- tokens presented ----------------------------------------------------
    void setToken(const PeerAddress& peer, std::string_view token);
    Access granted(const PeerAddress& peer) const;

private:
    // A held call to send once its token is answered: built then.
    struct Held {
        uint32_t id;
        std::string text;
        std::shared_ptr<Builder> rebuild;
    };
    // A held call's text, built again from its builder.
    static std::string rebuilt(const Builder& build, uint32_t id);
    void release(const PeerAddress& peer, const std::vector<Held>& held);
    void sendAuth(const PeerAddress& peer, const std::string& token);
    void authDone(const PeerAddress& peer, Status status, JsonVariantConst body);

    DatagramEndpoint& ep_;
    std::vector<Slot> calls_;  // guarded by the endpoint's mutex
    uint32_t nextId_ = 1;
};

// The remote nodes routed through the endpoint, and the nodes it mounts for
// peers that announce themselves (docs/DESIGN.md section 14.1).
class Mounter {
public:
    explicit Mounter(DatagramEndpoint& endpoint) : ep_(endpoint) {}

    // Remote nodes called without the endpoint's mutex (they call back into
    // the endpoint, the API and the application): pinned under it while the
    // call lasts. A pinned node isn't evicted, and its destructor
    // (removeRemote()) waits for the pin to go (docs/DESIGN.md section 9).
    class Pinned {
    public:
        explicit Pinned(Mounter& mounter) : mounter_(mounter) {}
        ~Pinned();
        // Under the endpoint's mutex: pins every remote node, or `node`.
        void all();
        void add(RemoteNode* node);
        std::vector<RemoteNode*> nodes;

        Pinned(const Pinned&) = delete;
        Pinned& operator=(const Pinned&) = delete;

    private:
        Mounter& mounter_;
        bool scratch_ = false;  // `nodes` is the mounter's pinScratch_, given back when done
    };

    void addRemote(RemoteNode* remote);
    void removeRemote(RemoteNode* remote);
    void routeAdded(RemoteNode* remote, const PeerAddress& peer);
    // Under the endpoint's mutex.
    const std::vector<RemoteNode*>& remotes() const { return remotes_; }
    // Detaches every node, for ~DatagramEndpoint. Under its mutex.
    void detachAll();
    // The peer's routes are online, or not, as of now.
    void markOnline(const PeerAddress& peer, bool online);

    void mountPeers(Object& parent, uint32_t mirrorIntervalMs);
    void peerSeen(const PeerAddress& peer, std::string_view name, uint32_t schemaHash, const PeerAddress& device,
                  uint8_t protocol, size_t maxMessage);
    // Whether a peerSeen() waits for process(). Under the endpoint's mutex.
    bool seenWaiting() const { return !seen_.empty(); }
    // Applies what peerSeen() queued; true if there was any.
    bool applySeen();

private:
    void apply(const SeenPeer& seen);
    void mount(Object& parent, uint32_t mirrorMs, const SeenPeer& seen, const PeerAddress& device);
    bool roomToMount();
    bool pinnedLocked(const RemoteNode* node) const;
    bool pinned(const RemoteNode* node) const;
    void unlistLocked(const RemoteNode* remote);  // out of remotes_

    DatagramEndpoint& ep_;
    // Guarded by the endpoint's mutex.
    std::vector<SeenPeer> seen_;                        // peerSeen() → process()
    std::vector<std::pair<Object*, uint32_t>> mounts_;  // mountPeers(): parent, mirror interval
    std::vector<RemoteNode*> remotes_;
    std::vector<RemoteNode*> pinned_;  // one entry per pin
    // A Pinned's list, kept with its capacity so process() doesn't allocate
    // each tick; one Pinned at a time has it.
    std::vector<RemoteNode*> pinScratch_;
    bool pinScratchBusy_ = false;
    Signal unpinned_;  // given as pins go, for removeRemote()
};

// Serving peers: their requests, subscriptions, file transfers and tokens.
class EndpointServer {
public:
    explicit EndpointServer(DatagramEndpoint& endpoint);
    ~EndpointServer();

    // A request (`env`, from `msg`); it has an op, and there is an Api.
    void serve(JsonObjectConst env, const Incoming& msg);
    // A message that can't be taken or parsed now: answered busy, or its
    // call failed, at once.
    void refuse(const PeerAddress& from, std::string_view text, bool broadcast, Reason reason);
    void reject(const PeerAddress& from, std::string_view text, bool broadcast, Status status, Reason reason,
                const char* message);
    // Drops the sessions of `peer`, or, without one, those with nothing open
    // and those idle for Config::transferIdleMs.
    void dropFiles(const PeerAddress* peer, uint32_t now);
    size_t fileSessions() const { return files_.size(); }
    size_t subscribers() const;
    // `peer`'s subscriber goes, with its subscriptions.
    void dropSubscriber(const PeerAddress& peer);
    // Every subscriber goes, for ~DatagramEndpoint.
    void dropAll();
    // process(): the subscribers a queued request kept.
    void checkIdle();
    // The peers holding a subscription, for forgetAll().
    template <class F>
    void eachSubscriber(F f) const {
        MutexGuard guard(subscribersMutex_);
        for (const PeerSubscriber* s : subscribers_) f(s->peer);
    }
    void nextTick(Deadline& d) const;
    // A request's client from `peer`: transport, address and device, before
    // its level is decided.
    Client clientOf(const PeerAddress& peer) const;
    Access levelOf(const PeerAddress& peer);

private:
    class PeerSubscriber : public Subscriber {
    public:
        PeerSubscriber(EndpointServer& server, const PeerAddress& address) : server_(server), peer(address) {}
        bool notify(const std::string& message, Delivery delivery) override;
        bool notifyOwned(std::string&& message, Delivery delivery) override;
        // Api::tokensChanged(): a peer that isn't trusted is at its token's level.
        bool recheckLevel(Access& level) override;

    private:
        EndpointServer& server_;

    public:
        PeerAddress peer;
    };
    struct PeerToken;
    // A peer's file transfers, dropped once idle for Config::transferIdleMs.
    struct PeerFiles {
        PeerAddress peer;
        std::unique_ptr<FileSession> session;
        uint32_t usedMs;
    };

    // Replies to `to` hand their buffer to the owned send, when there is one.
    void ownReplies(EnvelopeReply& reply, const PeerAddress& to, bool silent);
    void handleFile(JsonObjectConst envelope, const Incoming& msg, const Client& client, EnvelopeReply& reply);
    Subscriber* subscriberFor(const PeerAddress& peer, bool create);
    // Deletes subscribers (`only`, or all) without subscriptions or queued
    // requests.
    void dropIdleSubscribers(const Subscriber* only);

    DatagramEndpoint& ep_;
    mutable Mutex subscribersMutex_{kRankSubscribers, "endpoint subscribers"};  // taken before the API lock, never after
    std::vector<PeerSubscriber*> subscribers_;
    std::atomic<bool> idleCheck_{false};  // a subscriber kept for a queued request
    std::vector<PeerFiles> files_;  // used in process() only
};

}  // namespace detail
}  // namespace tesser

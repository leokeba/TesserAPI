#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "tesser/call.h"
#include "tesser/budget.h"
#include "tesser/node.h"
#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

struct Config {
    size_t maxRequestBody = 4096;  // bytes of request text a transport accepts
    size_t maxResponse = 8192;     // bytes, for transports that buffer responses
    uint8_t maxDepth = 16;         // deepest level rendered or patched
    uint32_t deferTimeoutMs = 10000;
    uint8_t maxPending = 4;  // deferred calls in flight
    uint8_t maxSubscriptions = 16;          // in total
    uint8_t maxSubscriptionsPerClient = 4;  // per Subscriber
    uint32_t watchIntervalMs = 200;         // sampling period for watch()ed values
    // Queued mode (docs/DESIGN.md section 9): requests wait in a queue and run
    // in poll(), in the application's task, instead of in the transport's.
    bool queued = false;
    uint8_t maxQueued = 8;
    // A file transfer that has moved no bytes for this long is taken over by
    // the next one to the same path (its client went quiet without going
    // away). 0: never.
    uint32_t transferIdleMs = 30000;
};

// One client's interest in a subtree (see docs/DESIGN.md section 11).
struct Subscription {
    Subscriber* subscriber = nullptr;
    Node* node = nullptr;  // the subscribed node: never removed while subscribed
    std::string path;      // normalized, "" for the root
    std::string keys;
    std::string exclude;
    int depth = Query::kUnlimited;
    uint32_t interval = 0;
    bool changes = true;
    bool events = true;
    bool remotes = true;
    uint32_t since = 0;  // generation of the last flush
    uint32_t lastFlushMs = 0;
    // A notification put off (no memory for it, or the subscriber refused
    // it) waits backoffMs, from 50 ms doubling to 2 s, before it is built
    // again; 0 when the last one went.
    uint32_t backoffMs = 0;
    uint32_t retryAtMs = 0;
    // Forwarded below a remote node (docs/DESIGN.md section 14): the remote
    // node relays its changes; poll() leaves it alone.
    bool forwarded = false;
    bool snapshot = true;
    Access access = Access::Admin;  // the client's level: what notifications may include
    // The level the client asked to act at ("sub@user", docs/DESIGN.md
    // section 13.1): its notifications carry it as "as", and only an unsub
    // at the same level ends it. Admin: none.
    Access ceiling = Access::Admin;
    Reply* waiting = nullptr;  // detached reply awaiting the remote's snapshot
};

class Storage;
class FileTransfer;
class RemoteNode;
class TransferHolder;
class Api;
namespace detail {
void handleNested(Api& api, const Request& request, Reply& reply);
}

// Decides whether a client may perform an operation on a node. Consulted for
// the target of every request, and for every node a patch touches.
using Authorizer = std::function<bool(const Client& client, Op op, const Node& node)>;

// Gives the access level of a token presented by a client (HTTP and
// WebSocket: "Authorization: Bearer" or ?token=); empty when it presented
// none. See Api::authenticate().
using TokenCheck = std::function<Access(std::string_view token)>;

namespace authorizers {
// Anyone may read and subscribe; writes and actions need an authenticated
// client (HTTP/WebSocket bearer token, trusted NowTP peer, serial line).
inline Authorizer readOnlyUnlessAuthenticated() {
    return [](const Client& c, Op op, const Node&) { return op != Op::Set || c.authenticated; };
}
// Every request needs an authenticated client.
inline Authorizer authenticatedOnly() {
    return [](const Client& c, Op, const Node&) { return c.authenticated; };
}
}  // namespace authorizers

// The root of the tree and the request handler. All requests run under one
// recursive mutex; see docs/DESIGN.md section 9.
class Api : public Object {
public:
    Api();
    ~Api() override;

    Config& config() { return config_; }
    const Config& config() const { return config_; }

    // Handles a request and completes `reply` (or detaches it for a deferred
    // action). Thread-safe.
    void handle(const Request& request, Reply& reply);

    Mutex& mutex() { return mutex_; }

    // The device this API runs on (docs/DESIGN.md section 14.1), shown as
    // "id" at the root of the schema (not hashed) and in the HTTP server's
    // mDNS record: the station MAC on target, or what setDeviceId() set
    // (a node reached by its soft-AP MAC, tests). False when unknown.
    void setDeviceId(const uint8_t mac[6]);
    bool deviceId(uint8_t mac[6]) const;
    // "c8f09ef4a67c"; empty when unknown.
    std::string deviceIdHex() const;

    // The client whose request is being handled, for setters, actions and
    // validators that want to know who writes (its transport, address and
    // device); null outside a request. Valid under the API lock, until the
    // handler returns.
    const Client* currentClient() const { return current_; }

    // Deferred calls in flight.
    int pending() const { return pending_.load(); }

    // Marks the node at `path` changed (see Node::changed()). Returns false
    // if there is no such node.
    bool changed(std::string_view path);

    // Delivers change notifications and samples watch()ed values. Call it
    // regularly (from loop(), or let startTask() do it); it does nothing
    // while nobody is subscribed. `nowMs` is a millisecond clock.
    void poll();
    void poll(uint32_t nowMs);

#if defined(ESP_PLATFORM)
    // Runs poll() every periodMs in a FreeRTOS task.
    bool startTask(uint32_t periodMs = 20, uint32_t stackSize = 4096, unsigned priority = 3);
#endif

    // Access control (docs/DESIGN.md section 13). Without an authorizer,
    // everything is allowed.
    void authorize(Authorizer fn);
    const Authorizer& authorizer() const { return authorizer_; }

    // Token authentication (docs/DESIGN.md section 13): transports that carry
    // tokens ask `fn` for each client's level. Without it, only a transport's
    // own token (HttpServer::setToken) authenticates.
    void authenticate(TokenCheck fn);
    // The level `fn` gives `token`; Public without a check.
    Access tokenAccess(std::string_view token) const;

    // Persistence (docs/DESIGN.md section 12): values and objects marked
    // persist() are saved to `storage` by poll(), debounceMs after the last
    // change to one of them, one record per top-level node; a change
    // rewrites only the records it touched. A record holds only the values
    // that differ from their defaults: what they were when persistence()
    // (or, for nodes declared since, load()) was first called. So declare
    // the tree first, then call persistence() and load().
    void persistence(Storage& storage, uint32_t debounceMs = 2000);
    // Called with each stored record older than the current version (see
    // persistVersion()), before it is applied: `record` is its top-level
    // node's name, `version` the version it was stored under (0: before
    // versions), `value` the record itself, which the migration may change
    // (rename keys, drop values equal to an old default). False drops the
    // record.
    using Migration = std::function<bool(std::string_view record, uint32_t version, JsonVariant value)>;
    // The version of the stored state's shape, stored with it, and how to
    // bring older state up to it. load() and restore() run `migrate` on
    // older records, then save them all under `version`. Before load().
    void persistVersion(uint32_t version, Migration migrate = nullptr);
    // Applies the stored records. Unknown keys and invalid values are skipped
    // with a warning. Returns false if nothing was stored.
    bool load();
    // Saves every record now. Returns false on a storage error.
    bool save();
    // The whole persisted state as one JSON document (the records under
    // their names, and "@version" when persistVersion() set one), secrets
    // included: a backup. Unlike the records, it holds every value, defaults
    // included, so restoring it gives exactly that state.
    std::string persistedState();
    // Applies a document like persistedState()'s (a backup), leniently like
    // load(): what doesn't fit the tree is skipped and its path added to
    // `skipped`. An older one goes through the migration first. What is
    // applied is marked changed, then everything is saved. BadRequest if it
    // doesn't parse, Internal if saving failed.
    Status restore(std::string_view json, std::vector<std::string>* skipped = nullptr);

    // Removes every subscription of a client. Transports call it when a
    // connection closes and before destroying a Subscriber.
    void dropSubscriber(Subscriber* subscriber);
    size_t subscriptionCount() const;
    // Whether a subscription is on `node`, or forwarded below it (a remote
    // node). Under the API lock.
    bool subscribedLocked(const Node& node) const;
    // How many subscriptions `subscriber` holds. Any task.
    size_t subscriptionsOf(const Subscriber* subscriber) const;

    // Used by the request handler and EventNode.
    // Status::Ok, or Busy when a limit is reached.
    Status subscriptionAllowed(Subscriber* subscriber) const;
    void addSubscription(Subscription&& sub);
    // Samples the watch()ed values at and below `node`, marking those that
    // changed since the last sample. A subscription samples its subtree
    // before its snapshot, so the snapshot is the baseline.
    void sampleWatched(Node& node);
    // The subscriptions of `subscriber` to `path` made at `ceiling`.
    size_t removeSubscriptions(Subscriber* subscriber, std::string_view path, Access ceiling = Access::Admin);
    // Sends an event to the subscriptions it concerns; with `accept`, only
    // to those it accepts (a remote node's events, docs/DESIGN.md section 14).
    void emitEvent(const std::string& path, const std::function<void(JsonWriter&)>& write,
                   const std::function<bool(const Subscription&)>& accept = nullptr);
    // Calls fn for each forwarded subscription to `node`, under the API lock;
    // fn returns false to remove the subscription. Used by RemoteNode.
    void forwardedSubscriptions(const Node* node, const std::function<bool(Subscription&)>& fn);
    // Finds the API whose tree holds `node`, and the node's path.
    static Api* owner(const Node& node, std::string& path);
    // Whether `path` reaches a remote node (it or below it): a request there
    // may wait on a peer (HttpServer serves those off its task).
    bool belowRemote(std::string_view path) const;

    // Used by Call/Pending.
    bool pendingBegin();
    void pendingDone() { pending_.fetch_sub(1); }

    // Used by FileTransfer, under the API lock: the file node at request.path
    // that the client may transfer (op Get: download, Set: upload), or null
    // with the error written to `reply`. `notFile` is set, with nothing
    // written, when the path resolves to another kind of node. The temporary
    // list and array elements on the path move to `elements`: an element's
    // file node lives as long as they do. With `remote`, a path below a
    // remote node that the client may transfer is null with nothing written,
    // and `remote` set: the transfer is relayed (docs/DESIGN.md section 14).
    struct RemoteTarget {
        RemoteNode* node = nullptr;
        std::string rest;  // the path below it
        std::string base;  // the full local path
        Access level = Access::Admin;  // the client's: the most the remote grants it
    };
    FileNode* fileNode(const Request& request, Reply& reply, bool& notFile,
                       std::vector<std::unique_ptr<Node>>& elements, RemoteTarget* remote = nullptr);
    // Used by FileTransfer and FileRelay, under the API lock: one transfer
    // per path (a relay claims a peer's key). A claim held by a transfer idle
    // for Config::transferIdleMs is taken over.
    bool claimTransfer(std::string_view path, TransferHolder* holder);
    void releaseTransfer(std::string_view path);

    // Requests waiting for poll() in queued mode.
    size_t queuedRequests() const;

    // FNV-1a hash of the full schema (view=hash at the root, full access):
    // changes whenever the tree's shape, types or constraints do, but not
    // with remote nodes' online state or advertised hashes. Clients can cache
    // a schema under it.
    uint32_t schemaHash();

    // A request copied out of the transport's buffers, with its detached reply.
    struct QueuedRequest {
        Request request;
        std::string path, keys, exclude, body;
        Reply* reply = nullptr;
        Reservation held;  // the copies' bytes (docs/DESIGN.md section 15)
    };

private:
    bool enqueue(const Request& request, Reply& reply);
    void runQueued();
    void handleNow(const Request& request, Reply& reply);
    void flush(Subscription& sub, uint32_t nowMs);
    // After subscriptions were removed: answers the waiting ones, and lets
    // remote nodes drop upstream subscriptions nobody uses any more.
    void subscriptionsRemoved(std::vector<Subscription>& gone, bool clientGone);
    void checkPersistence(uint32_t nowMs);
    // Writes the records of top-level nodes (all, or those changed since
    // the last save).
    bool saveRecords(bool all);
    // Records the defaults of persisted values not yet recorded.
    void captureDefaults();
    // Whether `n` holds its default (as captureDefaults() saw it).
    bool atDefault(const Node& n) const;

    Config config_;
    mutable Mutex mutex_{kRankApi, "api"};
    std::atomic<int> pending_{0};
    std::vector<Subscription> subs_;
    struct Transfer {
        std::string path;
        TransferHolder* holder;
    };
    std::vector<Transfer> transfers_;  // the file transfers in progress
    uint32_t lastWatchMs_ = 0;
    Authorizer authorizer_;
    TokenCheck tokenCheck_;
    Storage* storage_ = nullptr;
    uint32_t debounceMs_ = 2000;
    // Persisted values' defaults: node, hash of its value; sorted by node.
    std::vector<std::pair<const Node*, uint32_t>> defaults_;
    uint32_t persistVersion_ = 0;
    Migration migrate_;
    uint32_t savedGen_ = 0;  // generation when the state was last saved or loaded
    uint32_t seenGen_ = 0;   // newest persisted change seen by poll()
    uint32_t changedAtMs_ = 0;
    uint32_t lastPersistCheckMs_ = 0;
    // Set by persistence(): poll() reaches the persistence code through it,
    // so applications that don't persist don't link it.
    void (*persistHook_)(Api& api, uint32_t nowMs) = nullptr;
    mutable Mutex queueMutex_{kRankLeaf, "api queue"};  // never held with the API lock
    std::vector<QueuedRequest*> queue_;  // a vector: an empty deque allocates
    Api* nextApi_ = nullptr;  // registry used by EventNode
    const Client* current_ = nullptr;  // under mutex_: see currentClient()
    uint8_t deviceId_[6] = {};
    bool hasDeviceId_ = false;
    friend void detail::handleNested(Api&, const Request&, Reply&);
};

// Holds the API lock: use it when another task changes bound state while
// transports are running.
class Lock {
public:
    explicit Lock(Api& api) : mutex_(api.mutex()) { mutex_.lock(); }
    ~Lock() { mutex_.unlock(); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

private:
    Mutex& mutex_;
};

}  // namespace tesser

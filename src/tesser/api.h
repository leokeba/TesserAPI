#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "tesser/call.h"
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
};

// One client's interest in a subtree (see docs/DESIGN.md section 11).
struct Subscription {
    Subscriber* subscriber = nullptr;
    Node* node = nullptr;  // the subscribed node; nodes are never removed
    std::string path;      // normalized, "" for the root
    std::string keys;
    std::string exclude;
    int depth = Query::kUnlimited;
    uint32_t interval = 0;
    bool events = true;
    bool remotes = true;
    uint16_t since = 0;  // generation of the last flush
    uint32_t lastFlushMs = 0;
    // Forwarded below a remote node (docs/DESIGN.md section 14): the remote
    // node relays its changes; poll() leaves it alone.
    bool forwarded = false;
    bool snapshot = true;
    Access access = Access::Admin;  // the client's level: what notifications may include
    Reply* waiting = nullptr;  // detached reply awaiting the remote's snapshot
};

class Storage;

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
    // rewrites only the records it touched. Call load() once the tree is
    // declared.
    void persistence(Storage& storage, uint32_t debounceMs = 2000);
    // Applies the stored records. Unknown keys and invalid values are skipped
    // with a warning. Returns false if nothing was stored.
    bool load();
    // Saves every record now. Returns false on a storage error.
    bool save();
    // The whole persisted state as one JSON document (the records under
    // their names), secrets included: a backup.
    std::string persistedState();
    // Applies a document like persistedState()'s (a backup), leniently like
    // load(): what doesn't fit the tree is skipped and its path added to
    // `skipped`. What is applied is marked changed, then everything is
    // saved. BadRequest if it doesn't parse, Internal if saving failed.
    Status restore(std::string_view json, std::vector<std::string>* skipped = nullptr);

    // Removes every subscription of a client. Transports call it when a
    // connection closes and before destroying a Subscriber.
    void dropSubscriber(Subscriber* subscriber);
    size_t subscriptionCount() const;

    // Used by the request handler and EventNode.
    // Status::Ok, or Busy when a limit is reached.
    Status subscriptionAllowed(Subscriber* subscriber) const;
    void addSubscription(Subscription&& sub);
    // Samples the watch()ed values at and below `node`, marking those that
    // changed since the last sample. A subscription samples its subtree
    // before its snapshot, so the snapshot is the baseline.
    void sampleWatched(Node& node);
    size_t removeSubscriptions(Subscriber* subscriber, std::string_view path);
    void emitEvent(const std::string& path, const std::function<void(JsonWriter&)>& write);
    // Calls fn for each forwarded subscription to `node`, under the API lock;
    // fn returns false to remove the subscription. Used by RemoteNode.
    void forwardedSubscriptions(const Node* node, const std::function<bool(Subscription&)>& fn);
    // Finds the API whose tree holds `node`, and the node's path.
    static Api* owner(const Node& node, std::string& path);

    // Used by Call/Pending.
    bool pendingBegin();
    void pendingDone() { pending_.fetch_sub(1); }

    // Requests waiting for poll() in queued mode.
    size_t queuedRequests() const;

    // FNV-1a hash of the full schema: changes whenever the tree's shape,
    // types or constraints do. Clients can cache a schema under it.
    uint32_t schemaHash();

    // A request copied out of the transport's buffers, with its detached reply.
    struct QueuedRequest {
        Request request;
        std::string path, keys, exclude, body;
        Reply* reply = nullptr;
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

    Config config_;
    mutable Mutex mutex_;
    std::atomic<int> pending_{0};
    std::vector<Subscription> subs_;
    uint32_t lastWatchMs_ = 0;
    Authorizer authorizer_;
    TokenCheck tokenCheck_;
    Storage* storage_ = nullptr;
    uint32_t debounceMs_ = 2000;
    uint16_t savedGen_ = 0;  // generation when the state was last saved or loaded
    uint16_t seenGen_ = 0;   // newest persisted change seen by poll()
    uint32_t changedAtMs_ = 0;
    uint32_t lastPersistCheckMs_ = 0;
    // Set by persistence(): poll() reaches the persistence code through it,
    // so applications that don't persist don't link it.
    void (*persistHook_)(Api& api, uint32_t nowMs) = nullptr;
    mutable Mutex queueMutex_;  // never held with the API lock
    std::vector<QueuedRequest*> queue_;  // a vector: an empty deque allocates
    Api* nextApi_ = nullptr;  // registry used by EventNode
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

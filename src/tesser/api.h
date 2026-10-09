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
    uint16_t since = 0;  // generation of the last flush
    uint32_t lastFlushMs = 0;
};

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

    // Removes every subscription of a client. Transports call it when a
    // connection closes and before destroying a Subscriber.
    void dropSubscriber(Subscriber* subscriber);
    size_t subscriptionCount() const;

    // Used by the request handler and EventNode.
    // Status::Ok, or Busy when a limit is reached.
    Status subscriptionAllowed(Subscriber* subscriber) const;
    void addSubscription(Subscription&& sub);
    size_t removeSubscriptions(Subscriber* subscriber, std::string_view path);
    void emitEvent(const std::string& path, const std::function<void(JsonWriter&)>& write);
    // Finds the API whose tree holds `node`, and the node's path.
    static Api* owner(const Node& node, std::string& path);

    // Used by Call/Pending.
    bool pendingBegin();
    void pendingDone() { pending_.fetch_sub(1); }

private:
    void flush(Subscription& sub, uint32_t nowMs);
    void sampleWatched(Node& node);

    Config config_;
    mutable Mutex mutex_;
    std::atomic<int> pending_{0};
    std::vector<Subscription> subs_;
    uint32_t lastWatchMs_ = 0;
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

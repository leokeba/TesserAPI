#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

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
};

// The root of the tree and the request handler. All requests run under one
// recursive mutex; see docs/DESIGN.md section 9.
class Api : public Object {
public:
    Api() : Object("") {}

    Config& config() { return config_; }
    const Config& config() const { return config_; }

    // Handles a request and completes `reply` (or detaches it for a deferred
    // action). Thread-safe.
    void handle(const Request& request, Reply& reply);

    Mutex& mutex() { return mutex_; }

    // Deferred calls in flight.
    int pending() const { return pending_.load(); }

    // Used by Call/Pending.
    bool pendingBegin();
    void pendingDone() { pending_.fetch_sub(1); }

private:
    Config config_;
    Mutex mutex_;
    std::atomic<int> pending_{0};
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

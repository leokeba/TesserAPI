#pragma once

// A remote node's streamed sets (docs/DESIGN.md section 14.3): the queue,
// its merging and its retries. The node sends what next() gives it and
// reports the answer to done().

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/platform.h"
#include "tesser/status.h"

namespace tesser {
namespace detail {

class SetStream {
public:
    // `mutex`: the node's leaf lock; handlers are called without it.
    explicit SetStream(Mutex& mutex) : mutex_(mutex) {}
    struct Stats {
        uint32_t queued = 0;  // sets accepted
        uint32_t merged = 0;  // sets merged into one still queued
        uint32_t sent = 0;    // requests sent, resends included
        uint32_t failed = 0;  // sets dropped: error reply, peer offline, out of attempts
    };
    // A streamed set that failed: `path` below the node, the status, and the
    // remote's error body (null when it didn't answer).
    using ErrorHandler = std::function<void(const std::string& path, Status status, JsonVariantConst body)>;

    // Queues a set to `path` below the node, merged into the last one queued
    // for that path unless a set to a path above or below it came after.
    // Ok, or Busy when the queue is full.
    Status push(std::string_view path, JsonDocument&& body);
    // The set to send next, if none is in flight: true, with its path below
    // the node and its body as text; it is then in flight. Not `reachable`:
    // every set waiting is dropped and reported to the error handler.
    bool next(bool reachable, std::string& path, std::string& body);
    // The set next() gave couldn't be sent: it waits at the front again.
    void unsent();
    // The answer to the set in flight. True when the next one should go now
    // (a busy peer gets a moment: the node's tick sends it).
    bool done(Status s, JsonVariantConst body);

    void setLimit(size_t entries) { limit_ = entries; }
    void onError(ErrorHandler fn);
    Stats stats() const;
    // Sets queued or in flight.
    size_t size() const;
    // A set could go, and only the node's tick will send it (unsent(), or a
    // busy peer's answer).
    bool waiting() const;

private:
    struct Pending {
        std::string path;  // below the node
        JsonDocument body;
        uint8_t attempts = 0;
    };
    static void requeue(std::vector<Pending>& queue, Pending p);

    Mutex& mutex_;
    std::vector<Pending> queue_;
    Pending flying_;
    bool inFlight_ = false;
    size_t limit_ = 8;
    Stats stats_;
    ErrorHandler onError_;
};

}  // namespace detail
}  // namespace tesser

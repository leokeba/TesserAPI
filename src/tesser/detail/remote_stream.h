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
        uint32_t late = 0;    // timed sets the device got after their time (also failed)
    };
    // Timed sets (Query::at) in flight at once.
    static constexpr size_t kTimedWindow = 4;
    // A streamed set that failed: `path` below the node, the status, and the
    // remote's error body (null when it didn't answer).
    using ErrorHandler = std::function<void(const std::string& path, Status status, JsonVariantConst body)>;

    // Queues a set to `path` below the node, merged into the last one queued
    // for that path unless a set to a path above or below it came after.
    // `persist` false: the device doesn't save it (Query::persist); a merged
    // set is saved if any set in it was, as the device would have saved them
    // one by one (a save due takes the unsaved value along). Ok, or Busy
    // when the queue is full.
    //
    // `at` (network µs, Query::at) makes it a timed set: never merged, kept
    // in order, its time moved to 1 µs after the previous timed set's if it
    // isn't later (so two at one instant stay in order on the device, which
    // applies them by time), and sent without waiting for the untimed set
    // in flight: up to kTimedWindow at once. Not retried: a timed set that
    // fails is reported, and one past its time is counted late.
    Status push(std::string_view path, JsonDocument&& body, bool persist = true, uint64_t at = 0);
    // The set to send next, if none is in flight: true, with its path below
    // the node, its body as text and whether it is saved; it is then in
    // flight. Not `reachable`: every set waiting is dropped and reported to
    // the error handler.
    bool next(bool reachable, std::string& path, std::string& body, bool& persist);
    // The set next() gave couldn't be sent: it waits at the front again.
    void unsent();
    // Same, for timed sets: one more to send while the window has room.
    // `token` names it for unsentTimed() and doneTimed().
    bool nextTimed(bool reachable, std::string& path, std::string& body, bool& persist, uint64_t& at,
                   uint32_t& token);
    void unsentTimed(uint32_t token);
    void doneTimed(uint32_t token, Status s, JsonVariantConst body);
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
        bool persist = true;
        uint64_t at = 0;     // timed sets
        uint32_t token = 0;  // timed sets in flight
    };
    void dropAll(std::vector<Pending>& dropped);
    void reportDropped(const std::vector<Pending>& dropped, const ErrorHandler& onError);
    static void requeue(std::vector<Pending>& queue, Pending p);

    Mutex& mutex_;
    std::vector<Pending> queue_;
    Pending flying_;
    bool inFlight_ = false;
    std::vector<Pending> timed_;        // waiting, in order
    std::vector<Pending> timedFlying_;  // in flight
    uint64_t lastAt_ = 0;
    uint32_t tokens_ = 0;
    size_t limit_ = 8;
    Stats stats_;
    ErrorHandler onError_;
};

}  // namespace detail
}  // namespace tesser

#include "tesser/detail/remote_stream.h"

#include "tesser/detail/remote_json.h"

namespace tesser {
namespace detail {

namespace {

constexpr uint8_t kStreamAttempts = 3;  // a streamed set that timed out is sent twice more

// A path below a remote node: "" for the node, else "/a/b".
std::string normalPath(std::string_view p) {
    std::string s(p);
    if (!s.empty() && s.front() != '/') s.insert(0, "/");
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// One path is the other or below it.
bool overlaps(const std::string& a, const std::string& b) {
    const std::string& shorter = a.size() < b.size() ? a : b;
    const std::string& longer = a.size() < b.size() ? b : a;
    return longer.compare(0, shorter.size(), shorter) == 0 &&
           (longer.size() == shorter.size() || longer[shorter.size()] == '/');
}

}  // namespace

Status SetStream::push(std::string_view path, JsonDocument&& body) {
    std::string p = normalPath(path);
    MutexGuard guard(mutex_);
    // On top of the last set queued for the path, unless a set to a path
    // above or below it came after (they must stay in order).
    for (size_t i = queue_.size(); i-- > 0;) {
        if (queue_[i].path == p) {
            mergeJson(queue_[i].body.as<JsonVariant>(), body.as<JsonVariantConst>());
            stats_.queued++;
            stats_.merged++;
            return Status::Ok;
        }
        if (overlaps(queue_[i].path, p)) break;
    }
    if (queue_.size() >= limit_) return Status::Busy;
    queue_.emplace_back();
    queue_.back().path = std::move(p);
    queue_.back().body = std::move(body);
    stats_.queued++;
    return Status::Ok;
}

void SetStream::onError(ErrorHandler fn) {
    MutexGuard guard(mutex_);
    onError_ = std::move(fn);
}

SetStream::Stats SetStream::stats() const {
    MutexGuard guard(mutex_);
    return stats_;
}

size_t SetStream::size() const {
    MutexGuard guard(mutex_);
    return queue_.size() + (inFlight_ ? 1 : 0);
}

// Puts a set that must go again at the front, with the later sets to its
// path merged on top (up to one to a path above or below it).
void SetStream::requeue(std::vector<Pending>& queue, Pending p) {
    for (size_t i = 0; i < queue.size();) {
        if (queue[i].path == p.path) {
            mergeJson(p.body.as<JsonVariant>(), queue[i].body.as<JsonVariantConst>());
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (overlaps(queue[i].path, p.path)) break;
        i++;
    }
    queue.insert(queue.begin(), std::move(p));
}

bool SetStream::next(bool reachable, std::string& path, std::string& body) {
    std::vector<Pending> dropped;
    ErrorHandler onError;
    {
        MutexGuard guard(mutex_);
        if (inFlight_ || queue_.empty()) return false;
        if (reachable) {
            flying_ = std::move(queue_.front());
            queue_.erase(queue_.begin());
            inFlight_ = true;
            stats_.sent++;
            path = flying_.path;
            serializeJson(flying_.body, body);
            return true;
        }
        dropped.swap(queue_);
        stats_.failed += static_cast<uint32_t>(dropped.size());
        onError = onError_;
    }
    if (!onError) return false;
    JsonDocument error;
    error["error"] = "timeout";
    error["reason"] = "peer_offline";
    error["message"] = "peer offline";
    for (const Pending& p : dropped) {
        error["path"] = p.path.empty() ? std::string("/") : p.path;
        onError(p.path, Status::Timeout, error.as<JsonVariantConst>());
    }
    return false;
}

void SetStream::unsent() {
    // No call slot or no room on the link: the node's tick tries again.
    MutexGuard guard(mutex_);
    inFlight_ = false;
    stats_.sent--;
    requeue(queue_, std::move(flying_));
}

bool SetStream::done(Status s, JsonVariantConst body) {
    bool retry = s == Status::Timeout || s == Status::Busy;
    std::string failed;
    ErrorHandler onError;
    {
        MutexGuard guard(mutex_);
        if (!inFlight_) return false;
        inFlight_ = false;
        Pending p = std::move(flying_);
        if (retry && ++p.attempts < kStreamAttempts) {
            // Applied or not, it goes again, under anything newer: on the
            // next route if this one was lost.
            requeue(queue_, std::move(p));
        } else if (s != Status::Ok) {
            failed = p.path;
            stats_.failed++;
            onError = onError_;
        }
    }
    if (onError) onError(failed, s, body);
    return s != Status::Busy;
}

}  // namespace detail
}  // namespace tesser

#include "tesser/detail/fan_out.h"

#include <stdio.h>

#include "tesser/api.h"
#include "tesser/call.h"

namespace tesser {
namespace detail {

namespace {

class FanOut;

// One match's reply, buffered. It lives on the heap, so a match that waits
// (a remote node, a deferred action) detaches it as is: release() hands the
// result back.
class MatchReply : public Reply {
public:
    MatchReply(FanOut& fan, size_t index, size_t limit) : fan_(fan), index_(index), sink_(body, limit) {}

    Sink& begin(Status s) override {
        status = s;
        body.clear();
        return sink_;
    }
    void end() override { ended = true; }
    bool rollback() override {
        body.clear();
        return true;
    }
    Reply* detach() override;
    void release() override;

    Status status = Status::Internal;
    std::string body;
    bool ended = false;

private:
    FanOut& fan_;
    size_t index_;
    StringSink sink_;
};

// Whether a not_found error at `body`'s path means the match doesn't have
// the path at all: the error names a prefix of the target below the match.
// A patch key that doesn't exist is named below the target instead.
bool unresolved(const std::string& body, std::string_view match, std::string_view target) {
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    JsonVariantConst p = doc["path"];
    if (!p.is<const char*>()) return false;
    std::string_view path(p.as<const char*>());
    return path.size() > match.size() && path.size() <= target.size() && target.compare(0, path.size(), path) == 0 &&
           (path.size() == target.size() || target[path.size()] == '/');
}

// The sets of one request, and their results, until the last one answers.
class FanOut {
public:
    FanOut(Api& api, Reply& reply, std::string_view prefix, std::string_view rest, std::string_view path,
           std::vector<Match>& matches)
        : api_(api), parent_(&reply), prefix_(prefix), rest_(rest), path_(path.empty() ? std::string_view("/") : path) {
        results_.resize(matches.size());
        for (size_t i = 0; i < matches.size(); i++) {
            results_[i].name = std::move(matches[i].name);
            results_[i].offline = matches[i].offline;
        }
    }

    void dispatch(const Request& request) {
        for (size_t i = 0; i < results_.size(); i++) {
            std::string match = matchPath(i);
            if (results_[i].offline) {
                StringReply r;
                writeError(r, Status::Timeout, match, "peer offline");
                record(i, r.status, std::move(r.body));
                continue;
            }
            std::string path = match + rest_;
            Request sub = request;
            sub.path = path;
            auto* reply = new MatchReply(*this, i, api_.config().maxResponse);
            handleNested(api_, sub, *reply);
            bool waiting;
            {
                MutexGuard guard(mutex_);
                waiting = results_[i].waiting;
            }
            if (waiting) continue;  // released later, or already: the reply is gone
            if (!reply->ended) writeError(*reply, Status::Internal, path, "no reply");
            Status s = reply->status;
            std::string body = std::move(reply->body);
            delete reply;
            record(i, s, std::move(body));
        }
        bool done;
        {
            MutexGuard guard(mutex_);
            dispatching_ = false;
            done = outstanding_ == 0;
        }
        if (done) complete();
    }

    // A match waits: the request's reply is detached once, by the first.
    bool detachMatch(size_t i) {
        MutexGuard guard(mutex_);
        if (results_[i].waiting || !dispatching_) return false;
        if (!detached_) {
            if (cantWait_) return false;
            detached_ = parent_->detach();
            if (!detached_) {
                cantWait_ = true;
                return false;
            }
        }
        results_[i].waiting = true;
        outstanding_++;
        return true;
    }

    // A waiting match answered, from any task.
    void matchDone(size_t i, Status s, std::string body) {
        record(i, s, std::move(body));
        bool done;
        {
            MutexGuard guard(mutex_);
            outstanding_--;
            done = outstanding_ == 0 && !dispatching_;
        }
        if (done) complete();
    }

private:
    struct Result {
        std::string name;
        bool offline = false;
        bool waiting = false;
        bool skipped = false;
        Status status = Status::Internal;
        std::string body;
    };

    std::string matchPath(size_t i) const { return prefix_ + "/" + results_[i].name; }

    void record(size_t i, Status s, std::string body) {
        bool skip = false;
        if (s == Status::NotFound) {
            std::string match = matchPath(i);
            skip = unresolved(body, match, match + rest_);
        }
        MutexGuard guard(mutex_);
        results_[i].status = s;
        results_[i].body = std::move(body);
        results_[i].skipped = skip;
    }

    void complete() {
        write(detached_ ? *detached_ : *parent_);
        if (detached_) detached_->release();
        delete this;
    }

    void entries(JsonWriter& w, bool ok) const {
        for (const Result& r : results_) {
            if (r.skipped || (r.status == Status::Ok) != ok) continue;
            w.key(r.name);
            if (r.body.empty()) {
                w.null();
            } else {
                w.raw(r.body);
            }
        }
    }

    // "1 of 3 failed: spinner-2: remote node didn't answer"
    static std::string summary(const Result& r, size_t failed, size_t total) {
        char head[40];
        snprintf(head, sizeof(head), "%u of %u failed: ", unsigned(failed), unsigned(total));
        std::string s = head + r.name + ": ";
        JsonDocument doc;
        if (!deserializeJson(doc, r.body) && doc["message"].is<const char*>()) {
            s += doc["message"].as<const char*>();
        } else {
            s += toString(r.status);
        }
        return s;
    }

    // Every match's result by name; on failure, an error body naming the
    // first failed match, with the results split into "failed" and "ok".
    void write(Reply& out) {
        size_t total = 0, failed = 0;
        const Result* first = nullptr;
        for (const Result& r : results_) {
            if (r.skipped) continue;
            total++;
            if (r.status == Status::Ok) continue;
            failed++;
            if (!first) first = &r;
        }
        if (total == 0) return writeError(out, Status::NotFound, path_, "no match");
        JsonWriter w(out.begin(first ? first->status : Status::Ok));
        w.beginObject();
        if (first) {
            w.key("error");
            w.string(toString(first->status));
            w.key("path");
            w.string(path_);
#if !defined(TESSER_NO_MESSAGES)
            w.key("message");
            w.string(summary(*first, failed, total));
#endif
            if (failed < total) {
                w.key("partial");
                w.boolean(true);
            }
            w.key("failed");
            w.beginObject();
            entries(w, false);
            w.endObject();
            if (failed < total) {
                w.key("ok");
                w.beginObject();
                entries(w, true);
                w.endObject();
            }
        } else {
            entries(w, true);
        }
        w.endObject();
        finishBody(out, w, path_);
    }

    Api& api_;
    Reply* parent_;  // the request's reply, used only while dispatching
    Reply* detached_ = nullptr;
    bool cantWait_ = false;
    std::string prefix_;
    std::string rest_;
    std::string path_;
    std::vector<Result> results_;
    Mutex mutex_;
    size_t outstanding_ = 0;
    bool dispatching_ = true;
};

Reply* MatchReply::detach() { return fan_.detachMatch(index_) ? this : nullptr; }

void MatchReply::release() {
    FanOut& fan = fan_;
    size_t i = index_;
    Status s = ended ? status : Status::Internal;
    std::string b = std::move(body);
    delete this;
    fan.matchDone(i, s, std::move(b));
}

}  // namespace

void fanOut(Api& api, const Request& request, Reply& reply, std::string_view prefix, std::string_view rest,
            std::string_view path, std::vector<Match>& matches) {
    if (matches.empty()) return writeError(reply, Status::NotFound, path, "no match");
    (new FanOut(api, reply, prefix, rest, path, matches))->dispatch(request);
}

}  // namespace detail
}  // namespace tesser

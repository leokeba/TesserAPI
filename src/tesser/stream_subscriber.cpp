#include "tesser/stream_subscriber.h"

#include <memory>

#include "tesser/api.h"
#include "tesser/call.h"
#include "tesser/platform.h"

namespace tesser {

namespace {

// What a subscription that waits for a remote node's snapshot ends with,
// shared between open() and the reply that answers later, in another task.
struct OpenState {
    Mutex mutex;
    Signal done;
    Status status = Status::Internal;
    std::string body;  // an error's
    bool ended = false;
    bool abandoned = false;  // open() gave up: the answer is dropped
};

using Opened = std::function<void(const std::string&)>;

// Answers later (below a remote node): the stream opens in end(), under the
// API lock, as for an immediate answer, so no notification comes first.
class LaterReply : public Reply {
public:
    LaterReply(std::shared_ptr<OpenState> state, size_t limit, Opened opened)
        : state_(std::move(state)), sink_(body_, limit, true), opened_(std::move(opened)) {}
    Sink& begin(Status s) override {
        status_ = s;
        body_.clear();
        sink_.lowMemory = false;
        return sink_;
    }
    void end() override {
        MutexGuard guard(state_->mutex);
        if (state_->abandoned) return;
        if (status_ == Status::Ok) opened_(body_);
        state_->status = status_;
        if (status_ != Status::Ok) state_->body = std::move(body_);
        state_->ended = true;
        state_->done.give();
    }
    bool rollback() override {
        body_.clear();
        return true;
    }
    bool outOfMemory() const override { return sink_.lowMemory; }
    void release() override { delete this; }

private:
    std::shared_ptr<OpenState> state_;
    Status status_ = Status::Internal;
    std::string body_;
    StringSink sink_;
    Opened opened_;
};

// Passes errors through to the transport's reply. On success, the stream
// opens in end(), under the API lock, so the snapshot (kept up to `limit`
// bytes) goes out before any notification.
class OpenReply : public Reply {
public:
    OpenReply(Reply& error, size_t limit, Opened opened)
        : error_(error), limit_(limit), sink_(body, limit, true), opened_(std::move(opened)) {}

    Sink& begin(Status s) override {
        status = s;
        body.clear();
        ended = false;
        sink_.lowMemory = false;
        return s == Status::Ok ? static_cast<Sink&>(sink_) : error_.begin(s);
    }
    void end() override {
        ended = true;
        if (status != Status::Ok) return error_.end();
        opened_(body);
    }
    bool rollback() override {
        if (status != Status::Ok) return error_.rollback();
        body.clear();
        return true;
    }
    bool outOfMemory() const override { return status == Status::Ok ? sink_.lowMemory : error_.outOfMemory(); }
    // Below a remote node: open() waits for the answer.
    Reply* detach() override {
        if (later) return nullptr;
        later = std::make_shared<OpenState>();
        return new LaterReply(later, limit_, opened_);
    }

    Status status = Status::Internal;
    std::string body;
    bool ended = false;
    std::shared_ptr<OpenState> later;

private:
    Reply& error_;
    size_t limit_;
    StringSink sink_;
    Opened opened_;
};

}  // namespace

StreamSubscriber::StreamSubscriber(Api& api, Send send) : api_(api), send_(std::move(send)) {}

StreamSubscriber::~StreamSubscriber() { api_.dropSubscriber(this); }

Status StreamSubscriber::open(Request request, Reply& error) {
    std::string_view path = request.path;
    if (opened_) {
        writeError(error, Status::NotAllowed, path, "one subscription per stream");
        return Status::NotAllowed;
    }
    opened_ = true;
    request.op = Op::Subscribe;
    request.subscriber = this;
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    if (path.empty()) path = "/";
    bool snapshot = request.query.snapshot;
    std::string p(path);
    OpenReply reply(error, api_.config().maxResponse, [this, p, snapshot](const std::string& body) {
        open_ = true;
        if (!snapshot || body == "null") return;
        std::string line;
        StringSink sink(line);
        JsonWriter w(sink);
        w.beginObject();
        w.key("op");
        w.string("change");
        w.key("path");
        w.string(p);
        w.key("body");
        w.raw(body);
        w.endObject();
        line += '\n';
        send(line);
    });
    api_.handle(request, reply);
    if (std::shared_ptr<OpenState> later = reply.later) {
        // A remote node's snapshot: wait for it, as a forwarded get does.
        bool answered = later->done.take(api_.config().deferTimeoutMs);
        MutexGuard guard(later->mutex);
        if (!answered && !later->ended) {
            later->abandoned = true;
            writeError(error, Status::Timeout, request.path, "the remote node didn't answer");
            open_ = false;
            api_.dropSubscriber(this);  // the waiting subscription, answered with nothing
            return Status::Timeout;
        }
        if (later->status == Status::Ok) return Status::Ok;
        error.begin(later->status).write(later->body.data(), later->body.size());
        error.end();
        open_ = false;
        api_.dropSubscriber(this);
        return later->status;
    }
    if (!reply.ended) writeError(reply, Status::Internal, request.path, "no reply");
    if (reply.status == Status::Ok) return Status::Ok;
    open_ = false;
    // A subscription may have been added before its snapshot failed.
    api_.dropSubscriber(this);
    return reply.status;
}

bool StreamSubscriber::send(const std::string& piece) {
    sent_ = true;
    return send_ && send_(piece);
}

bool StreamSubscriber::notify(const std::string& message, Delivery) {
    if (!open_) return true;  // closed: nobody to tell
    return send(message + "\n");
}

bool StreamSubscriber::keepAlive(uint32_t nowMs, uint32_t idleMs) {
    if (!open_) return true;
    if (sent_.exchange(false) || !ticked_) {
        ticked_ = true;
        lastMs_ = nowMs;
        return true;
    }
    if (nowMs - lastMs_ < idleMs) return true;
    lastMs_ = nowMs;
    return send_ && send_("\n");
}

void StreamSubscriber::close() {
    if (open_.exchange(false)) api_.dropSubscriber(this);
}

}  // namespace tesser

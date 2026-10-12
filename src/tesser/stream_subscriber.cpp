#include "tesser/stream_subscriber.h"

#include "tesser/api.h"
#include "tesser/call.h"

namespace tesser {

namespace {

// Passes errors through to the transport's reply. On success, the stream
// opens in end(), under the API lock, so the snapshot (kept up to `limit`
// bytes) goes out before any notification.
class OpenReply : public Reply {
public:
    OpenReply(Reply& error, size_t limit, std::function<void(const std::string&)> opened)
        : error_(error), sink_(body, limit), opened_(std::move(opened)) {}

    Sink& begin(Status s) override {
        status = s;
        body.clear();
        ended = false;
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

    Status status = Status::Internal;
    std::string body;
    bool ended = false;

private:
    Reply& error_;
    StringSink sink_;
    std::function<void(const std::string&)> opened_;
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
    OpenReply reply(error, api_.config().maxResponse, [this, path, snapshot](const std::string& body) {
        open_ = true;
        if (!snapshot || body == "null") return;
        std::string line;
        StringSink sink(line);
        JsonWriter w(sink);
        w.beginObject();
        w.key("op");
        w.string("change");
        w.key("path");
        w.string(path);
        w.key("body");
        w.raw(body);
        w.endObject();
        line += '\n';
        send(line);
    });
    api_.handle(request, reply);
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

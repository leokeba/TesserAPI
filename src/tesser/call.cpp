#include "tesser/call.h"

#include "tesser/api.h"

namespace tesser {

void writeError(Reply& reply, Status status, std::string_view path, const char* message, bool partial) {
    JsonWriter w(reply.begin(status));
    w.beginObject();
    w.key("error");
    w.string(toString(status));
    w.key("path");
    w.string(path.empty() ? std::string_view("/") : path);
#if !defined(TESSER_NO_MESSAGES)
    if (message) {
        w.key("message");
        w.string(message);
    }
#else
    (void)message;
#endif
    if (partial) {
        w.key("partial");
        w.boolean(true);
    }
    w.endObject();
    reply.end();
}

namespace detail {

void finishBody(Reply& reply, JsonWriter& w, std::string_view path) {
    if (!w.ok() && reply.rollback()) {
        writeError(reply, Status::TooLarge, path, "response too large");
        return;
    }
    reply.end();
}

}  // namespace detail

Pending::Pending(const Pending& o) : s_(o.s_) {
    if (s_) s_->refs.fetch_add(1);
}

Pending::~Pending() {
    if (!s_) return;
    if (s_->refs.fetch_sub(1) != 1) return;
    // Last handle: make sure the client hears back.
    if (Reply* r = take()) {
        writeError(*r, Status::Internal, s_->path, "deferred reply dropped");
        finish(r);
    }
    delete s_;
}

Reply* Pending::take() {
    if (!s_ || s_->done.exchange(true)) return nullptr;
    return s_->reply;
}

void Pending::finish(Reply* r, JsonWriter& w) {
    detail::finishBody(*r, w, s_->path);
    finish(r);
}

void Pending::finish(Reply* r) {
    r->release();
    s_->api->pendingDone();
}

void Pending::reply() {
    Reply* r = take();
    if (!r) return;
    JsonWriter w(r->begin(Status::Ok));
    w.null();
    finish(r, w);
}

void Pending::fail(Status status, const char* message) {
    Reply* r = take();
    if (!r) return;
    writeError(*r, status, s_->path, message);
    finish(r);
}

Pending Call::defer() {
    if (!reply_ || replied_ || deferred_) return Pending();
    if (!api_.pendingBegin()) {
        fail(Status::Busy, "too many pending calls");
        return Pending();
    }
    Reply* detached = reply_->detach();
    if (!detached) {
        api_.pendingDone();
        fail(Status::NotAllowed, "transport can't defer replies");
        return Pending();
    }
    deferred_ = true;
    auto* s = new Pending::State();
    s->reply = detached;
    s->api = &api_;
    s->path.assign(path_.data(), path_.size());
    return Pending(s);
}

}  // namespace tesser

#include "tesser/envelope.h"

#include <string.h>

#include "tesser/api.h"
#include "tesser/call.h"
#include "tesser/file_session.h"

namespace tesser {

namespace {

class FixedWriter {
public:
    FixedWriter(char* buf, size_t cap) : buf_(buf), cap_(cap) {}
    size_t write(uint8_t c) { return write(&c, 1); }
    size_t write(const uint8_t* data, size_t len) {
        if (len_ + len > cap_) {
            overflow_ = true;
            return 0;
        }
        memcpy(buf_ + len_, data, len);
        len_ += len;
        return len;
    }
    size_t length() const { return len_; }
    bool overflow() const { return overflow_; }

private:
    char* buf_;
    size_t cap_;
    size_t len_ = 0;
    bool overflow_ = false;
};

std::string_view view(JsonVariantConst v) {
    JsonString s = v.as<JsonString>();
    return std::string_view(s.c_str(), s.size());
}

}  // namespace

Status parseEnvelope(std::string_view text, JsonDocument& doc, uint8_t maxDepth, Request& request, EnvelopeId& id,
                     const char*& message, Reason* reason) {
    id.length = 0;
    DeserializationError err = deserializeJson(doc, text.data(), text.size(),
                                               DeserializationOption::NestingLimit(envelopeNesting(maxDepth)));
    if (err) {
        message = err == DeserializationError::TooDeep ? "envelope nested too deeply" : "malformed JSON";
        return Status::BadRequest;
    }
    if (!doc.is<JsonObjectConst>()) {
        message = "envelope must be an object";
        return Status::BadRequest;
    }
    return requestFromEnvelope(doc.as<JsonObjectConst>(), request, id, message, reason);
}

Status checkStrict(JsonObjectConst env, std::initializer_list<const char*> known, const char*& message,
                   Reason* reason) {
    JsonVariantConst v = env["v"];
    if (v.isNull()) return Status::Ok;  // lenient: unknown members are ignored
    if (!v.is<uint8_t>() || v.as<uint8_t>() == 0) {
        message = "v must be a protocol version, 1-255";
        return Status::BadRequest;
    }
    if (v.as<uint8_t>() < 2) return Status::Ok;
    for (JsonPairConst kv : env) {
        bool ok = false;
        for (const char* k : known) ok = ok || kv.key() == k;
        if (!ok) {
            message = "an option this node doesn't know (the request is strict: \"v\")";
            if (reason) *reason = Reason::UnknownOption;
            return Status::BadRequest;
        }
    }
    return Status::Ok;
}

Status envelopeId(JsonObjectConst env, EnvelopeId& id, const char*& message) {
    id.length = 0;
    JsonVariantConst idv = env["id"];
    if (!idv.isUnbound() && !idv.isNull()) {
        if (idv.is<JsonObjectConst>() || idv.is<JsonArrayConst>()) {
            message = "id must be a scalar";
            return Status::BadRequest;
        }
        FixedWriter w(id.json, EnvelopeId::kMax);
        serializeJson(idv, w);
        if (w.overflow()) {
            message = "id too long";
            return Status::BadRequest;
        }
        id.length = w.length();
        id.json[id.length] = '\0';
    }
    return Status::Ok;
}

Status requestFromEnvelope(JsonObjectConst env, Request& request, EnvelopeId& id, const char*& message,
                           Reason* reason) {
    Status s = envelopeId(env, id, message);
    if (s != Status::Ok) return s;
    JsonVariantConst op = env["op"];
    std::string_view name;
    Access ceiling = Access::Admin;
    if (!op.is<const char*>() || !splitOp(view(op), name, ceiling) || !parseOp(name, request.op)) {
        message = "op must be \"get\", \"set\", \"sub\" or \"unsub\"";
        return Status::BadRequest;
    }
    // A gateway's client (docs/DESIGN.md section 13.1): no more than its level.
    request.query.ceiling = ceiling;
    request.client.cap(ceiling);
    s = checkStrict(env,
                    {"v", "id", "op", "path", "depth", "keys", "exclude", "view", "interval", "remotes", "changes",
                     "events", "snapshot", "body"},
                    message, reason);
    if (s != Status::Ok) return s;
    JsonVariantConst path = env["path"];
    if (path.isNull()) {
        request.path = "/";
    } else if (path.is<const char*>()) {
        request.path = view(path);
    } else {
        message = "path must be a string";
        return Status::BadRequest;
    }

    JsonVariantConst depth = env["depth"];
    if (!depth.isNull()) {
        if (!depth.is<uint8_t>()) {
            message = "depth must be an integer 0-255";
            return Status::BadRequest;
        }
        request.query.depth = depth.as<uint8_t>();
    }
    JsonVariantConst keys = env["keys"];
    if (!keys.isNull()) {
        if (!keys.is<const char*>()) {
            message = "keys must be a comma-separated string";
            return Status::BadRequest;
        }
        request.query.keys = view(keys);
    }
    JsonVariantConst exclude = env["exclude"];
    if (!exclude.isNull()) {
        if (!exclude.is<const char*>()) {
            message = "exclude must be a comma-separated string";
            return Status::BadRequest;
        }
        request.query.exclude = view(exclude);
    }
    JsonVariantConst v = env["view"];
    if (!v.isNull() && (!v.is<const char*>() || !parseView(view(v), request.query.view))) {
        message = "view must be \"value\", \"schema\", \"hash\" or \"info\"";
        return Status::BadRequest;
    }
    JsonVariantConst interval = env["interval"];
    if (!interval.isNull()) {
        if (!interval.is<uint32_t>()) {
            message = "interval must be a non-negative integer (ms)";
            return Status::BadRequest;
        }
        request.query.interval = interval.as<uint32_t>();
    }
    JsonVariantConst remotes = env["remotes"];
    if (!remotes.isNull()) {
        if (!remotes.is<bool>()) {
            message = "remotes must be a boolean";
            return Status::BadRequest;
        }
        request.query.remotes = remotes.as<bool>();
    }
    JsonVariantConst changes = env["changes"];
    if (!changes.isNull()) {
        if (!changes.is<bool>()) {
            message = "changes must be a boolean";
            return Status::BadRequest;
        }
        request.query.changes = changes.as<bool>();
    }
    JsonVariantConst events = env["events"];
    if (!events.isNull()) {
        if (!events.is<bool>()) {
            message = "events must be a boolean";
            return Status::BadRequest;
        }
        request.query.events = events.as<bool>();
    }
    JsonVariantConst snapshot = env["snapshot"];
    if (!snapshot.isNull()) {
        if (!snapshot.is<bool>()) {
            message = "snapshot must be a boolean";
            return Status::BadRequest;
        }
        request.query.snapshot = snapshot.as<bool>();
    }
    request.body = env["body"];
    return Status::Ok;
}

bool EnvelopeReply::LimitedSink::write(const char* data, size_t len) {
    if (r_.fragment_) {
        if (r_.failed_) return false;
        r_.buf_.append(data, len);
        return r_.buf_.size() < r_.chunk_ || r_.flush();
    }
    if (r_.limit_ && r_.buf_.size() + len + 1 > r_.limit_) return false;  // keep room for '}'
    // Buffered whole: on a fragmented heap, refuse rather than abort.
    if (!growFor(r_.buf_, r_.buf_.size() + len + 1)) {
        r_.lowMemory_ = true;
        return false;
    }
    r_.buf_.append(data, len);
    return true;
}

EnvelopeReply::EnvelopeReply(const EnvelopeId& id, size_t limit, Send send)
    : id_(id), limit_(limit), send_(std::move(send)) {}

EnvelopeReply::~EnvelopeReply() {
    // A streamed envelope cut short still ends, so the transport can release
    // what it holds for it (a line's output lock, a WebSocket message).
    if (flushed_) fragment_(nullptr, 0, false, true);
}

void EnvelopeReply::stream(Fragment fragment, size_t chunk) {
    fragment_ = std::move(fragment);
    chunk_ = chunk ? chunk : 1;
}

bool EnvelopeReply::flush() {
    bool first = !flushed_;
    flushed_ = true;
    if (!fragment_(buf_.data(), buf_.size(), first, false)) failed_ = true;
    buf_.clear();
    return !failed_;
}

Sink& EnvelopeReply::begin(Status status) {
    buf_.clear();
    lowMemory_ = false;
    buf_ += '{';
    if (id_.length) {
        buf_ += "\"id\":";
        buf_.append(id_.json, id_.length);
        buf_ += ',';
    }
    buf_ += "\"status\":\"";
    buf_ += toString(status);
    buf_ += "\",\"body\":";
    return sink_;
}

void EnvelopeReply::end() {
    buf_ += '}';
    if (fragment_) {
        bool first = !flushed_;
        flushed_ = false;  // the final piece goes out here
        if (failed_) {
            if (!first) fragment_(nullptr, 0, false, true);
        } else {
            fragment_(buf_.data(), buf_.size(), first, true);
        }
    } else if (send_) {
        send_(buf_);
    }
    buf_.clear();
    buf_.shrink_to_fit();
}

bool EnvelopeReply::rollback() {
    if (flushed_) return false;
    buf_.clear();
    failed_ = false;
    return true;
}

Reply* EnvelopeReply::detach() {
    auto* r = new EnvelopeReply(id_, limit_, send_);
    r->detached_ = true;
    return r;
}

void EnvelopeReply::release() {
    if (detached_) delete this;
}

void handleEnvelope(Api& api, std::string_view text, const Client& client, const EnvelopeReply::Send& send,
                    Subscriber* subscriber, const EnvelopeReply::Fragment& stream, FileSession* files) {
    JsonDocument doc;
    Request req;
    req.client = client;
    req.subscriber = subscriber;
    EnvelopeId id;
    const char* message = nullptr;
    Reason reason = Reason::None;
    Status s = parseEnvelope(text, doc, api.config().maxDepth, req, id, message, &reason);
    bool file = s != Status::Ok && files && doc.is<JsonObjectConst>() && FileSession::handles(doc.as<JsonObjectConst>());
    if (file) {
        reason = Reason::None;
        s = envelopeId(doc.as<JsonObjectConst>(), id, message);
    }
    EnvelopeReply reply(id, api.config().maxResponse, send);
    if (stream) reply.stream(stream);
    if (file && s == Status::Ok) {
        files->handle(doc.as<JsonObjectConst>(), client, reply);
        return;
    }
    if (s != Status::Ok) {
        writeError(reply, s, reason, std::string_view(), message);
        return;
    }
    api.handle(req, reply);
}

}  // namespace tesser

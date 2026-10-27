#include "tesser/envelope.h"

#include <string.h>

#include "tesser/api.h"
#include "tesser/call.h"
#include "tesser/datagram.h"
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

bool isUploadEnvelope(std::string_view text) {
    std::string_view op;
    return jsonMember(text, "op", op) && op.compare(0, 7, "\"upload") == 0;
}

JsonDocument uploadFilter() {
    JsonDocument filter;
    for (const char* k : {"v", "id", "op", "path", "offset", "size", "filename", "length", "crc32"}) filter[k] = true;
    return filter;
}

Status parseEnvelope(std::string_view text, JsonDocument& doc, uint8_t maxDepth, Request& request, EnvelopeId& id,
                     const char*& message, Reason* reason) {
    id.length = 0;
    DeserializationError err =
        isUploadEnvelope(text)
            ? deserializeJson(doc, text.data(), text.size(), DeserializationOption::Filter(uploadFilter()),
                              DeserializationOption::NestingLimit(envelopeNesting(maxDepth)))
            : deserializeJson(doc, text.data(), text.size(),
                              DeserializationOption::NestingLimit(envelopeNesting(maxDepth)));
    if (err == DeserializationError::NoMemory) {
        // Not the client's fault: busy, under its id if the text has one,
        // so it sends the request again.
        std::string_view raw;
        if (jsonMember(text, "id", raw) && raw.size() <= EnvelopeId::kMax) {
            memcpy(id.json, raw.data(), raw.size());
            id.length = raw.size();
        }
        message = "not enough memory for the request";
        if (reason) *reason = Reason::LowMemory;
        return Status::Busy;
    }
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
        // On a short heap, or past the window (no drain() came: a deep list,
        // say), whole pieces go now, under the lock.
        // With room for the closing '}' too: one more byte past an exact
        // capacity doubles it (libstdc++), unchecked.
        if (!growFor(r_.buf_, r_.buf_.size() + len + 1)) {
            if (!r_.sendPieces(false)) return false;
            if (!growFor(r_.buf_, r_.buf_.size() + len + 1)) {
                r_.lowMemory_ = true;
                return false;
            }
        }
        r_.buf_.append(data, len);
        return r_.buf_.size() <= r_.window_ || r_.sendPieces(false);
    }
    if (r_.owned_) {
        size_t size = r_.bytes_.size();
        if (r_.limit_ && size + len + 1 > r_.limit_) return false;  // keep room for '}'
        if (!growFor(r_.bytes_, size + len + 1)) {
            r_.lowMemory_ = true;
            return false;
        }
        return r_.bytes_.append(data, len);
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
    if (ended_) {
        sendPieces(true);  // nobody drained it
    } else if (flushed_) {
        // A streamed envelope cut short still ends, so the transport can
        // release what it holds for it (a line's output, a WebSocket message).
        fragment_(nullptr, 0, false, true);
    }
}

void EnvelopeReply::sendOwned(SendOwned send, size_t slack) {
    owned_ = std::move(send);
    slack_ = slack;
}

void EnvelopeReply::stream(Fragment fragment, size_t chunk, size_t window) {
    fragment_ = std::move(fragment);
    chunk_ = chunk ? chunk : 1;
    window_ = window > chunk_ ? window : chunk_;
}

bool EnvelopeReply::sendPieces(bool final) {
    size_t at = 0;
    // Whole pieces; the tail waits for more, unless this is the end.
    while (!failed_ && at < buf_.size() && (final || buf_.size() - at >= chunk_)) {
        size_t n = buf_.size() - at < chunk_ ? buf_.size() - at : chunk_;
        bool last = final && at + n == buf_.size();
        if (!fragment_(buf_.data() + at, n, !flushed_, last)) failed_ = true;
        flushed_ = !last;  // a piece is out: the end is owed, until the last
        at += n;
    }
    if (!final) {
        if (failed_) {
            buf_.clear();
        } else {
            buf_.erase(0, at);
        }
        return !failed_;
    }
    buf_.clear();
    // The end on its own: after a failed piece, or when every byte went
    // out with earlier drains.
    if (flushed_) fragment_(nullptr, 0, false, true);
    flushed_ = false;
    ended_ = false;
    buf_.shrink_to_fit();
    return !failed_;
}

bool EnvelopeReply::wantsDrain() const { return fragment_ && !failed_ && (ended_ || buf_.size() >= chunk_); }

void EnvelopeReply::drain() {
    if (!fragment_) return;
    if (ended_) {
        sendPieces(true);
    } else if (!failed_ && buf_.size() >= chunk_) {
        sendPieces(false);
    }
}

Sink& EnvelopeReply::begin(Status status) {
    buf_.clear();
    lowMemory_ = false;
    ended_ = false;
    buf_ += '{';
    if (id_.length) {
        buf_ += "\"id\":";
        buf_.append(id_.json, id_.length);
        buf_ += ',';
    }
    buf_ += "\"status\":\"";
    buf_ += toString(status);
    buf_ += "\",\"body\":";
    if (owned_ && !fragment_) {
        // The head moves to the owned buffer: the body is written there.
        bytes_.clear();
        if (!growFor(bytes_, buf_.size() + 1) || !bytes_.append(buf_)) lowMemory_ = true;
        buf_.clear();
    }
    return sink_;
}

void EnvelopeReply::end() {
    buf_ += '}';
    if (fragment_) {
        // The final pieces go with drain() or the destructor: not under the
        // API lock (docs/DESIGN.md section 9).
        ended_ = true;
        return;
    }
    if (owned_) {
        // The envelope, with the slack the link wants after it (without it,
        // if the heap can't: the link then grows it itself).
        buf_.clear();
        growFor(bytes_, bytes_.size() + slack_);
        if (bytes_.append("}", 1) || (growFor(bytes_, bytes_.size() + 1) && bytes_.append("}", 1))) {
            owned_(bytes_);
        }
        bytes_.reset();
        return;
    }
    if (send_) send_(buf_);
    buf_.clear();
    buf_.shrink_to_fit();
}

bool EnvelopeReply::rollback() {
    if (flushed_) return false;
    buf_.clear();
    bytes_.clear();
    failed_ = false;
    ended_ = false;
    return true;
}

Reply* EnvelopeReply::detach() {
    auto* r = new EnvelopeReply(id_, limit_, send_);
    r->owned_ = owned_;
    r->slack_ = slack_;
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
        files->handle(doc.as<JsonObjectConst>(), client, reply, text);
        return;
    }
    if (s != Status::Ok) {
        writeError(reply, s, reason, std::string_view(), message);
        return;
    }
    api.handle(req, reply);
}

}  // namespace tesser

#include "tesser/envelope.h"

#include <string.h>

#include "tesser/api.h"
#include "tesser/call.h"

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
                     const char*& message) {
    id.length = 0;
    DeserializationError err = deserializeJson(doc, text.data(), text.size(),
                                               DeserializationOption::NestingLimit(static_cast<uint8_t>(maxDepth + 1)));
    if (err) {
        message = err == DeserializationError::TooDeep ? "envelope nested too deeply" : "malformed JSON";
        return Status::BadRequest;
    }
    if (!doc.is<JsonObjectConst>()) {
        message = "envelope must be an object";
        return Status::BadRequest;
    }
    return requestFromEnvelope(doc.as<JsonObjectConst>(), request, id, message);
}

Status requestFromEnvelope(JsonObjectConst env, Request& request, EnvelopeId& id, const char*& message) {
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

    JsonVariantConst op = env["op"];
    if (!op.is<const char*>() || !parseOp(view(op), request.op)) {
        message = "op must be \"get\" or \"set\"";
        return Status::BadRequest;
    }
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
        message = "view must be \"value\" or \"schema\"";
        return Status::BadRequest;
    }
    request.body = env["body"];
    return Status::Ok;
}

bool EnvelopeReply::LimitedSink::write(const char* data, size_t len) {
    if (r_.limit_ && r_.buf_.size() + len + 1 > r_.limit_) return false;  // keep room for '}'
    r_.buf_.append(data, len);
    return true;
}

EnvelopeReply::EnvelopeReply(const EnvelopeId& id, size_t limit, Send send)
    : id_(id), limit_(limit), send_(std::move(send)) {}

Sink& EnvelopeReply::begin(Status status) {
    buf_.clear();
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
    if (send_) send_(buf_);
    buf_.clear();
    buf_.shrink_to_fit();
}

bool EnvelopeReply::rollback() {
    buf_.clear();
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

void handleEnvelope(Api& api, std::string_view text, const Client& client, const EnvelopeReply::Send& send) {
    JsonDocument doc;
    Request req;
    req.client = client;
    EnvelopeId id;
    const char* message = nullptr;
    Status s = parseEnvelope(text, doc, api.config().maxDepth, req, id, message);
    EnvelopeReply reply(id, api.config().maxResponse, send);
    if (s != Status::Ok) {
        writeError(reply, s, std::string_view(), message);
        return;
    }
    api.handle(req, reply);
}

}  // namespace tesser

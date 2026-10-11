#include "tesser/line_transport.h"

#include "tesser/api.h"
#include "tesser/call.h"

namespace tesser {

LineTransport::LineTransport(Api& api, Output output) : api_(api), output_(std::move(output)) {}

LineTransport::~LineTransport() { api_.dropSubscriber(this); }

bool LineTransport::notify(const std::string& message, Delivery) {
    send(message);
    return true;
}

void LineTransport::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) feed(data[i]);
}

void LineTransport::feed(char c) {
    if (c == '\n') {
        if (discarding_) {
            discarding_ = false;
            overflows_++;
            EnvelopeId noId;
            EnvelopeReply reply(noId, 0, [this](const std::string& m) { send(m); });
            writeError(reply, Status::TooLarge, std::string_view(), "line too long");
        } else {
            handleLine();
        }
        line_.clear();
        return;
    }
    if (discarding_) return;
    if (line_.size() >= api_.config().maxRequestBody + 256) {
        discarding_ = true;
        line_.clear();
        line_.shrink_to_fit();
        return;
    }
    line_ += c;
}

void LineTransport::handleLine() {
    // Skip whitespace and the noise a UART picks up while the peer resets
    // (control bytes, 0xff, ...), which would otherwise hide the request.
    size_t start = 0;
    while (start < line_.size()) {
        unsigned char c = static_cast<unsigned char>(line_[start]);
        if (c > ' ' && c < 0x7f) break;
        start++;
    }
    if (start >= line_.size() || line_[start] != '{') return;
    std::string_view text(line_.data() + start, line_.size() - start);
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);

    JsonDocument doc;
    Request req;
    EnvelopeId id;
    const char* message = nullptr;
    Status s = parseEnvelope(text, doc, api_.config().maxDepth, req, id, message);
    if (s != Status::Ok && doc.is<JsonObjectConst>()) {
        // "auth" isn't a request op, so it fails to parse as one; a usable
        // id (none, or one that was kept) means the op is what failed.
        JsonObjectConst env = doc.as<JsonObjectConst>();
        if (env["op"] == "auth" && (id.length || env["id"].isNull())) {
            handleAuth(env, id);
            return;
        }
    }
    EnvelopeReply reply(id, api_.config().maxResponse, [this](const std::string& m) { send(m); });
    reply.stream([this](const char* data, size_t len, bool first, bool final) { return sendPiece(data, len, first, final); });
    if (s != Status::Ok) {
        writeError(reply, s, std::string_view(), message);
        return;
    }
    req.client.transport = TransportKind::Serial;
    req.client.access = level();
    req.client.authenticated = req.client.access != Access::Public;
    req.subscriber = this;
    api_.handle(req, reply);
}

Access LineTransport::level() const { return authenticated_ ? Access::Admin : api_.tokenAccess(token_); }

// {"op":"auth","token":"..."} sets the line's token until the next one or
// reset(); an empty or missing token signs out. The reply gives the level.
void LineTransport::handleAuth(JsonObjectConst envelope, const EnvelopeId& id) {
    EnvelopeReply reply(id, 0, [this](const std::string& m) { send(m); });
    JsonVariantConst token = envelope["token"];
    if (!token.isNull() && !token.is<const char*>()) {
        writeError(reply, Status::BadRequest, std::string_view(), "token must be a string");
        return;
    }
    Access before = level();
    JsonString t = token.as<JsonString>();
    token_.assign(t.c_str() ? t.c_str() : "", t.size());
    Access after = level();
    // Subscriptions keep the level they were made at: drop them rather than
    // show the client what it can no longer read.
    if (after < before) api_.dropSubscriber(this);
    std::string body = std::string("{\"access\":\"") + toString(after) + "\"}";
    reply.begin(Status::Ok).write(body.data(), body.size());
    reply.end();
}

void LineTransport::reset() {
    api_.dropSubscriber(this);
    line_.clear();
    discarding_ = false;
    token_.clear();
}

// A streamed response line. The output lock is held from the first piece to
// the last, so nothing else lands in the middle of the line.
bool LineTransport::sendPiece(const char* data, size_t len, bool first, bool final) {
    if (first) outputMutex_.lock();
    if (output_ && len) output_(data, len);
    if (final) {
        if (output_) output_("\n", 1);
        outputMutex_.unlock();
    }
    return true;
}

void LineTransport::send(const std::string& message) {
    if (!output_) return;
    MutexGuard guard(outputMutex_);
    output_(message.data(), message.size());
    output_("\n", 1);
}

}  // namespace tesser

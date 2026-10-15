#include "tesser/line_transport.h"

#include <string.h>

#include "tesser/api.h"
#include "tesser/budget.h"
#include "tesser/call.h"
#include "tesser/datagram.h"
#include "tesser/sink.h"

namespace tesser {

LineTransport::LineTransport(Api& api, Output output) : api_(api), output_(std::move(output)), files_(api) {}

LineTransport::~LineTransport() { api_.dropSubscriber(this); }

bool LineTransport::notify(const std::string& message, Delivery) { return send(message, true); }

void LineTransport::feed(const char* data, size_t len) {
    for (size_t i = 0; i < len; i++) feed(data[i]);
}

namespace {

// What a line discarded keeps of its start: room for its id.
constexpr size_t kKept = 64;

// Room for one more byte: grown by half again, within the byte accountant
// and the largest block. Unlike growFor(), no room for a copy of the line:
// nothing copies it on (docs/DESIGN.md section 15).
bool growLine(std::string& s) {
    if (s.size() < s.capacity()) return true;
    size_t capacity = s.capacity() + s.capacity() / 2;
    if (capacity < 64) capacity = 64;
    Reservation room;
    if (capacity > 256) {
        room = memoryBudget().reserve(capacity, 0, true);
        if (!room) return false;
    }
    reserveExactly(s, capacity);
    return true;
}

}  // namespace

// The id at the start of the line, if it is still there.
EnvelopeId LineTransport::keptId() const {
    EnvelopeId id;
    size_t brace = line_.find('{');
    std::string_view raw;
    if (brace != std::string::npos && jsonMember(std::string_view(line_).substr(brace), "id", raw) &&
        raw.size() <= EnvelopeId::kMax) {
        memcpy(id.json, raw.data(), raw.size());
        id.length = raw.size();
    }
    return id;
}

void LineTransport::lineDamaged() { damaged_ = true; }

void LineTransport::feed(char c) {
    if (c == '\n') {
        if (damaged_) {
            // Bytes of this line were lost (lineDamaged()): what is left may
            // still parse, wrongly. Answered under its id, to be sent again.
            damaged_ = discarding_ = starved_ = false;
            overruns_++;
            EnvelopeReply reply(keptId(), 0, [this](const std::string& m) { send(m, false); });
            writeError(reply, Status::Busy, Reason::LineOverrun, std::string_view(),
                       "bytes of the line were lost: send it again");
        } else if (discarding_) {
            discarding_ = false;
            overflows_++;
            // The line's start was kept: its id, so the client hears which
            // request failed rather than waiting it out.
            EnvelopeReply reply(keptId(), 0, [this](const std::string& m) { send(m, false); });
            if (starved_)
                writeError(reply, Status::Busy, Reason::LowMemory, std::string_view(), "not enough memory for the line");
            else
                writeError(reply, Status::TooLarge, std::string_view(), "line too long");
            starved_ = false;
        } else {
            handleLine();
        }
        line_.clear();
        // An upload line takes a few KB: give them back between lines, so
        // the reply to the next one (a download) finds them.
        if (line_.capacity() > 512) line_.shrink_to_fit();
        return;
    }
    if (discarding_) return;
    if (line_.size() >= api_.config().maxRequestBody + 256 || !growLine(line_)) {
        // Too long, or the heap can't hold it: the rest of the line goes,
        // and its end gets an error (busy when it was the heap). Its start
        // is kept for the id.
        starved_ = line_.size() < api_.config().maxRequestBody + 256;
        discarding_ = true;
        if (line_.size() > kKept) line_.resize(kKept);
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
    Reason reason = Reason::None;
    Status s = parseEnvelope(text, doc, api_.config().maxDepth, req, id, message, &reason);
    // "auth", "upload" and "download" aren't request ops, so they fail to
    // parse as requests: read their id alone.
    bool auth = false, file = false;
    if (s != Status::Ok && doc.is<JsonObjectConst>()) {
        JsonObjectConst env = doc.as<JsonObjectConst>();
        auth = env["op"] == "auth";
        file = FileSession::handles(env);
        if (auth || file) {
            reason = Reason::None;
            s = envelopeId(env, id, message);
        }
        if (auth && s == Status::Ok) {
            handleAuth(env, id);
            return;
        }
    }
    EnvelopeReply reply(id, api_.config().maxResponse, [this](const std::string& m) { send(m, false); });
    // A short window: a serial line drains slowly, and what waits for it
    // is heap the next request may need.
    reply.stream([this](const char* data, size_t len, bool first, bool final) { return sendPiece(data, len, first, final); },
                 512, 1024);
    if (s != Status::Ok) {
        writeError(reply, s, reason, std::string_view(), message);
        return;
    }
    req.client.transport = TransportKind::Serial;
    req.client.access = level();
    req.client.authenticated = req.client.access != Access::Public;
    if (file) {
        files_.handle(doc.as<JsonObjectConst>(), req.client, reply, text);
        return;
    }
    req.subscriber = this;
    api_.handle(req, reply);
}

Access LineTransport::level() const { return authenticated_ ? Access::Admin : api_.tokenAccess(token_); }

// {"op":"auth","token":"..."} sets the line's token until the next one or
// reset(); an empty or missing token signs out. The reply gives the level.
void LineTransport::handleAuth(JsonObjectConst envelope, const EnvelopeId& id) {
    EnvelopeReply reply(id, 0, [this](const std::string& m) { send(m, false); });
    const char* message = nullptr;
    Reason reason = Reason::None;
    if (checkStrict(envelope, {"v", "id", "op", "token"}, message, &reason) != Status::Ok) {
        writeError(reply, Status::BadRequest, reason, std::string_view(), message);
        return;
    }
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
    // A transfer opened at the higher level ends too.
    if (after < before) {
        api_.dropSubscriber(this);
        files_.reset();
    }
    std::string body = std::string("{\"access\":\"") + toString(after) + "\"}";
    reply.begin(Status::Ok).write(body.data(), body.size());
    reply.end();
}

void LineTransport::reset() {
    api_.dropSubscriber(this);
    line_.clear();
    line_.shrink_to_fit();
    discarding_ = starved_ = damaged_ = false;
    token_.clear();
    files_.reset();
}

// A streamed response line: from its first piece to its last, the line is
// its own (streaming_), so nothing lands in the middle of it, and its pieces
// are written without any lock: a slow line (Bluetooth waits for room)
// holds up no other task.
bool LineTransport::sendPiece(const char* data, size_t len, bool first, bool final) {
    if (first) {
        MutexGuard guard(outputMutex_);
        streaming_ = true;
    }
    if (output_ && len) output_(data, len);
    if (final) {
        MutexGuard guard(outputMutex_);
        if (output_) output_("\n", 1);
        streaming_ = false;
        // What came meanwhile, in order.
        if (output_ && !waiting_.empty()) output_(waiting_.data(), waiting_.size());
        waiting_.clear();
        waiting_.shrink_to_fit();
    }
    return true;
}

bool LineTransport::send(const std::string& message, bool mayDrop) {
    if (!output_) return true;
    MutexGuard guard(outputMutex_);
    if (streaming_) {
        // A notification beyond 2 KB waiting is dropped (its subscriber is
        // told "overflow" next); a deferred reply always waits.
        if (mayDrop && (waiting_.size() + message.size() > 2048 ||
                        !growFor(waiting_, waiting_.size() + message.size() + 1)))
            return false;
        waiting_ += message;
        waiting_ += '\n';
        return true;
    }
    output_(message.data(), message.size());
    output_("\n", 1);
    return true;
}

}  // namespace tesser

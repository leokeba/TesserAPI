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

// Api::tokensChanged(): an unauthenticated line's level comes from its token.
bool LineTransport::recheckLevel(Access& out) { return token_.recheck(api_, client(), out); }

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
        room = memoryBudget().reserve(capacity, 0, true, true, mallocPool(capacity));
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

// The line's token, for "auth".
struct LineTransport::Token : TokenClient {
    explicit Token(LineTransport& l) : line(l) {}
    Access tokenLevel() override { return line.level(); }
    void setToken(std::string_view token) override {
        MutexGuard guard(line.api_.mutex());  // recheckLevel() reads it from another task
        line.token_.token.assign(token.data(), token.size());
    }
    void levelFell() override {
        line.api_.dropSubscriber(&line);
        line.files_.reset();
    }
    LineTransport& line;
};

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
    EnvelopeCall call;
    parseEnvelope(text, doc, api_.config().maxDepth, kTakesAuth | kTakesFiles, call);
    EnvelopeReply reply(call.id, api_.config().maxResponse, [this](const std::string& m) { send(m, false); });
    // A short window: a serial line drains slowly, and what waits for it
    // is heap the next request may need.
    reply.stream([this](const char* data, size_t len, bool first, bool final) { return sendPiece(data, len, first, final); },
                 512, 1024);
    if (call.failed(reply)) return;
    if (call.kind == EnvelopeCall::Kind::Auth) {
        Token token(*this);
        answerAuth(doc.as<JsonObjectConst>(), token, reply);
        return;
    }
    Request& req = call.request;
    req.client.transport = kind_;
    req.client.access = level();
    req.client.authenticated = req.client.access != Access::Public;
    if (call.kind == EnvelopeCall::Kind::File) {
        files_.handle(doc.as<JsonObjectConst>(), req.client, reply, text);
        return;
    }
    req.subscriber = this;
    api_.handle(req, reply);
}

Access LineTransport::level() const {
    Access level = Access::Admin;
    token_.recheck(api_, client(), level);
    return level;
}

Client LineTransport::client() const {
    Client client;
    client.transport = kind_;
    return client;
}

void LineTransport::reset() {
    api_.dropSubscriber(this);
    line_.clear();
    line_.shrink_to_fit();
    discarding_ = starved_ = damaged_ = false;
    {
        MutexGuard guard(api_.mutex());
        token_.token.clear();
    }
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
    bool wake = false;
    {
        MutexGuard guard(outputMutex_);
        if (!streaming_ && !wake_) {
            output_(message.data(), message.size());
            output_("\n", 1);
            return true;
        }
        // A notification that would take more than 2 KB waiting is dropped
        // (its subscriber is told "overflow" next), unless it would wait
        // alone; a deferred reply always waits.
        if (mayDrop && ((!waiting_.empty() && waiting_.size() + message.size() > 2048) ||
                        !growFor(waiting_, waiting_.size() + message.size() + 1)))
            return false;
        waiting_ += message;
        waiting_ += '\n';
        wake = !streaming_;  // else the reply's end writes it
    }
    if (wake && wake_) wake_();
    return true;
}

void LineTransport::flush() {
    std::string out;
    {
        MutexGuard guard(outputMutex_);
        if (streaming_ || waiting_.empty()) return;
        out.swap(waiting_);
    }
    // Without the lock: only this task writes meanwhile (a reply it streams
    // comes after), and other tasks' messages wait for the next flush().
    if (output_) output_(out.data(), out.size());
}

}  // namespace tesser

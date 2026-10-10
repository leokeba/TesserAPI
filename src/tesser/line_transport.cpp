#include "tesser/line_transport.h"

#include "tesser/api.h"

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
    Client client;
    client.transport = TransportKind::Serial;
    client.authenticated = authenticated_;
    handleEnvelope(api_, text, client, [this](const std::string& m) { send(m); }, this,
                   [this](const char* data, size_t len, bool first, bool final) { return sendPiece(data, len, first, final); });
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

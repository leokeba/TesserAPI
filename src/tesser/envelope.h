#pragma once

#include <stddef.h>

#include <functional>
#include <string>
#include <string_view>

#include <ArduinoJson.h>

#include "tesser/request.h"

namespace tesser {

class Api;

// The request id, kept as serialized JSON so it can be echoed verbatim after
// the request document is gone (deferred replies).
struct EnvelopeId {
    static constexpr size_t kMax = 40;
    char json[kMax + 1] = {};
    size_t length = 0;  // 0: the request had no id
};

// Parses one envelope (docs/DESIGN.md section 10.1) into `request`. Strings in
// `request` point into `doc`. On failure returns the status to reply with and
// sets `message`; `id` is filled whenever the envelope had a usable one.
Status parseEnvelope(std::string_view text, JsonDocument& doc, uint8_t maxDepth, Request& request, EnvelopeId& id,
                     const char*& message);

// Same, from an already parsed envelope object.
Status requestFromEnvelope(JsonObjectConst envelope, Request& request, EnvelopeId& id, const char*& message);

// Buffers one response envelope and hands it to `send` when complete.
// Detaching copies it, so deferred replies work for any message transport
// whose `send` may be called from another task.
//
// With stream(), the envelope instead goes out in pieces as it is written,
// so its size isn't limited (docs/DESIGN.md section 10.1).
class EnvelopeReply : public Reply {
public:
    using Send = std::function<void(const std::string& message)>;
    // One piece of a streamed envelope. `first` and `final` mark the ends;
    // a piece with both is the whole envelope. After a failed piece, only a
    // final, empty one follows. Returns false if it couldn't be sent.
    using Fragment = std::function<bool(const char* data, size_t len, bool first, bool final)>;

    EnvelopeReply(const EnvelopeId& id, size_t limit, Send send);
    ~EnvelopeReply() override;

    // Streams the envelope through `fragment` in pieces of `chunk` bytes
    // instead of buffering it whole, with no size limit. Errors can replace
    // the body until the first piece is out. A detached copy (deferred
    // reply) is buffered as usual.
    void stream(Fragment fragment, size_t chunk = 1024);

    Sink& begin(Status status) override;
    void end() override;
    bool rollback() override;
    Reply* detach() override;
    void release() override;

private:
    class LimitedSink : public Sink {
    public:
        explicit LimitedSink(EnvelopeReply& r) : r_(r) {}
        bool write(const char* data, size_t len) override;

    private:
        EnvelopeReply& r_;
    };

    bool flush();

    EnvelopeId id_;
    size_t limit_;
    Send send_;
    std::string buf_;
    LimitedSink sink_{*this};
    bool detached_ = false;
    Fragment fragment_;
    size_t chunk_ = 0;
    bool flushed_ = false;   // a piece is out: no rollback, a final piece is owed
    bool failed_ = false;    // a piece couldn't be sent
};

// Handles one envelope end to end: parse, dispatch, reply through `send`.
// `subscriber` enables "sub"/"unsub" for transports with a persistent client.
// With `stream`, the reply is streamed through it (EnvelopeReply::stream()),
// and `send` only carries deferred replies.
void handleEnvelope(Api& api, std::string_view text, const Client& client, const EnvelopeReply::Send& send,
                    Subscriber* subscriber = nullptr, const EnvelopeReply::Fragment& stream = nullptr);

}  // namespace tesser

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
class EnvelopeReply : public Reply {
public:
    using Send = std::function<void(const std::string& message)>;

    EnvelopeReply(const EnvelopeId& id, size_t limit, Send send);

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

    EnvelopeId id_;
    size_t limit_;
    Send send_;
    std::string buf_;
    LimitedSink sink_{*this};
    bool detached_ = false;
};

// Handles one envelope end to end: parse, dispatch, reply through `send`.
// `subscriber` enables "sub"/"unsub" for transports with a persistent client.
void handleEnvelope(Api& api, std::string_view text, const Client& client, const EnvelopeReply::Send& send,
                    Subscriber* subscriber = nullptr);

}  // namespace tesser

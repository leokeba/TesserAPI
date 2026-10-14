#pragma once

#include <stddef.h>

#include <functional>
#include <initializer_list>
#include <string>
#include <string_view>

#include <ArduinoJson.h>

#include "tesser/json_writer.h"
#include "tesser/request.h"

namespace tesser {

class Api;
class FileSession;

// The request id, kept as serialized JSON so it can be echoed verbatim after
// the request document is gone (deferred replies).
struct EnvelopeId {
    static constexpr size_t kMax = 40;
    char json[kMax + 1] = {};
    size_t length = 0;  // 0: the request had no id
};

// JSON nesting accepted by the parsers. A request body nests at most one
// level per key a patch descends (maxDepth) plus the value, and an envelope
// adds one around it. Whatever a node wrote (responses, notifications,
// stored records) is bounded by its JsonWriter instead, not by maxDepth:
// schemas nest two or three levels per tree level, and the peer that wrote
// a reply may have a deeper maxDepth than ours. On Xtensa each level costs
// 80 to 96 bytes of stack while parsing (-Os, -Og).
inline uint8_t requestNesting(uint8_t maxDepth) { return maxDepth < 254 ? uint8_t(maxDepth + 1) : uint8_t(255); }
inline uint8_t envelopeNesting(uint8_t maxDepth) { return maxDepth < 253 ? uint8_t(maxDepth + 2) : uint8_t(255); }
constexpr uint8_t kWrittenNesting = JsonWriter::kMaxNesting + 1;

// Parses one envelope (docs/DESIGN.md section 10.1) into `request`. Strings in
// `request` point into `doc`. On failure returns the status to reply with and
// sets `message` (and `reason`, when given); `id` is filled whenever the
// envelope had a usable one.
Status parseEnvelope(std::string_view text, JsonDocument& doc, uint8_t maxDepth, Request& request, EnvelopeId& id,
                     const char*& message, Reason* reason = nullptr);

// Same, from an already parsed envelope object.
Status requestFromEnvelope(JsonObjectConst envelope, Request& request, EnvelopeId& id, const char*& message,
                           Reason* reason = nullptr);

// A strict envelope ("v" of 2 or more, docs/DESIGN.md section 10.1.2) whose
// members aren't all among `known`: Status::BadRequest with `message` and
// `reason` set. Ok otherwise.
Status checkStrict(JsonObjectConst envelope, std::initializer_list<const char*> known, const char*& message,
                   Reason* reason);

// Reads only the envelope's id, for envelopes that aren't requests ("auth",
// "upload", "download"). An unusable id is BadRequest with `message`.
Status envelopeId(JsonObjectConst envelope, EnvelopeId& id, const char*& message);

// Buffers one response envelope and hands it to `send` when complete.
// Detaching copies it, so deferred replies work for any message transport
// whose `send` may be called from another task.
//
// With stream(), the envelope instead goes out in pieces as it is written,
// so its size isn't limited (docs/DESIGN.md section 10.1).
class EnvelopeReply : public Reply {
public:
    // The message may be taken (moved from): it isn't used afterwards.
    using Send = std::function<void(std::string& message)>;
    // One piece of a streamed envelope. `first` and `final` mark the ends;
    // a piece with both is the whole envelope. After a failed piece, only a
    // final, empty one follows. Returns false if it couldn't be sent.
    using Fragment = std::function<bool(const char* data, size_t len, bool first, bool final)>;

    EnvelopeReply(const EnvelopeId& id, size_t limit, Send send);
    ~EnvelopeReply() override;

    // Streams the envelope through `fragment` in pieces of `chunk` bytes
    // instead of buffering it whole, with no size limit. Errors can replace
    // the body until the first piece is out. A detached copy (deferred
    // reply) is buffered as usual. Pieces wait, up to `window` bytes, for
    // drain() (the core calls it without the API lock) or the destructor;
    // beyond the window they go at once.
    void stream(Fragment fragment, size_t chunk = 1024, size_t window = 4096);

    Sink& begin(Status status) override;
    void end() override;
    bool rollback() override;
    Reply* detach() override;
    void release() override;
    bool outOfMemory() const override { return lowMemory_; }
    bool wantsDrain() const override;
    void drain() override;

private:
    class LimitedSink : public Sink {
    public:
        explicit LimitedSink(EnvelopeReply& r) : r_(r) {}
        bool write(const char* data, size_t len) override;

    private:
        EnvelopeReply& r_;
    };

    // Sends what is buffered in pieces; with `final`, the last one ends the
    // envelope.
    bool sendPieces(bool final);

    EnvelopeId id_;
    size_t limit_;
    Send send_;
    std::string buf_;
    LimitedSink sink_{*this};
    bool detached_ = false;
    bool lowMemory_ = false;
    Fragment fragment_;
    size_t chunk_ = 0;
    size_t window_ = 0;
    bool ended_ = false;     // end() was called: the final piece is owed
    bool flushed_ = false;   // a piece is out: no rollback, a final piece is owed
    bool failed_ = false;    // a piece couldn't be sent
};

// Handles one envelope end to end: parse, dispatch, reply through `send`.
// `subscriber` enables "sub"/"unsub" for transports with a persistent client.
// With `stream`, the reply is streamed through it (EnvelopeReply::stream()),
// and `send` only carries deferred replies. `files` enables "upload" and
// "download" (docs/DESIGN.md section 10.1.1) for the client it belongs to.
void handleEnvelope(Api& api, std::string_view text, const Client& client, const EnvelopeReply::Send& send,
                    Subscriber* subscriber = nullptr, const EnvelopeReply::Fragment& stream = nullptr,
                    FileSession* files = nullptr);

}  // namespace tesser

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <string>
#include <string_view>

#include <ArduinoJson.h>

#include "tesser/sink.h"
#include "tesser/status.h"

namespace tesser {

enum class Op : uint8_t { Get, Set, Subscribe, Unsubscribe };

enum class View : uint8_t { Value, Schema };

// "get" / "set" / "sub" / "unsub"; false if unknown.
bool parseOp(std::string_view s, Op& out);
bool parseView(std::string_view s, View& out);
const char* toString(Op op);

struct Query {
    static constexpr int kUnlimited = -1;
    int depth = kUnlimited;  // levels of objects expanded below the target
    std::string_view keys;     // comma-separated direct children to include
    std::string_view exclude;  // comma-separated direct children to omit
    View view = View::Value;
    // Subscribe only.
    uint32_t interval = 0;  // minimum ms between change notifications
    bool events = true;     // also deliver events under the path
};

enum class TransportKind : uint8_t { Local, Serial, Http, NowTP, WebSocket };

// Who sent a request, as far as the transport knows.
struct Client {
    TransportKind transport = TransportKind::Local;
    uint8_t address[16] = {};  // IPv4/IPv6 or MAC, transport-defined
    uint8_t addressLength = 0;
    bool authenticated = false;
};

// How a message to a client should be delivered, for transports that offer
// a choice (NowTP).
enum class Delivery : uint8_t {
    Reliable,    // retransmitted until acknowledged (requests, responses, events)
    LatestOnly,  // a newer message supersedes older ones
    BestEffort,  // broadcast
};

// A client that can receive notifications: one per persistent connection
// (serial line, WebSocket connection, NowTP peer). Implemented by transports,
// which must call Api::dropSubscriber() before destroying one.
class Subscriber {
public:
    virtual ~Subscriber() = default;
    // Sends one notification envelope. Called under the API lock, from
    // Api::poll() or from an event's emit(); must not block for long.
    // Returns false if it couldn't be queued.
    virtual bool notify(const std::string& message, Delivery delivery) = 0;
    // Set when a notification was lost (by the core when notify() fails, or by
    // the transport when a queued one fails later). The next notification
    // carries "overflow": true so the client knows to re-read.
    std::atomic<bool> missed{false};
};

struct Request {
    Op op = Op::Get;
    std::string_view path;  // "/lamp/brightness"; "" or "/" is the root
    Query query;
    JsonVariantConst body;  // Set: value or patch. Get: optional shape.
    Client client;
    Subscriber* subscriber = nullptr;  // persistent transports; required for Subscribe
};

// Receives a response. Implemented by transports. The core calls begin()
// exactly once, writes one JSON value (the body) to the returned sink, then
// calls end(). Status and body encoding are the transport's business.
class Reply {
public:
    virtual ~Reply() = default;
    virtual Sink& begin(Status status) = 0;
    virtual void end() = 0;
    // Discards everything written since begin() so the core can start over
    // with an error. Only buffered replies can do this.
    virtual bool rollback() { return false; }
    // Keeps the reply alive after the handler returns, for deferred actions.
    // Returns a heap object the core completes later (begin/write/end, then
    // release()), or nullptr if the transport can't defer.
    virtual Reply* detach() { return nullptr; }
    // Called on a detached reply after end(); typically `delete this`.
    virtual void release() {}
};

// Reply that captures status and body into memory: handy for tests and for
// transports that build their own framing around the body.
class StringReply : public Reply {
public:
    explicit StringReply(size_t limit = 0) : sink_(body, limit) {}

    Sink& begin(Status s) override {
        status = s;
        body.clear();
        begun = true;
        return sink_;
    }
    void end() override { ended = true; }
    bool rollback() override {
        body.clear();
        return true;
    }

    Status status = Status::Internal;
    std::string body;
    bool begun = false;
    bool ended = false;

private:
    StringSink sink_;
};

}  // namespace tesser

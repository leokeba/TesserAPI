#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include <ArduinoJson.h>

#include "tesser/sink.h"
#include "tesser/status.h"

namespace tesser {

enum class Op : uint8_t { Get, Set, Subscribe, Unsubscribe };

// value: the data; schema: its description; hash: the FNV-1a hash of the
// schema, as 8 hex digits, for clients that cache schemas; info: the
// protocol version and features, with the hash (docs/DESIGN.md section
// 10.1.2).
enum class View : uint8_t { Value, Schema, Hash, Info };

// "get" / "set" / "sub" / "unsub"; false if unknown.
bool parseOp(std::string_view s, Op& out);
bool parseView(std::string_view s, View& out);
const char* toString(Op op);

// Access levels, ordered (docs/DESIGN.md section 13): a client has one, and a
// node may require one to be read or written.
enum class Access : uint8_t { Public, User, Admin };

// "public" / "user" / "admin".
const char* toString(Access a);
bool parseAccess(std::string_view s, Access& out);

// An envelope's op with the level it acts at (docs/DESIGN.md section 13.1):
// "get" (`ceiling` admin, no limit) or "get@user". False if the suffix
// isn't a level; `name` is then the whole text.
bool splitOp(std::string_view op, std::string_view& name, Access& ceiling);

struct Query {
    static constexpr int kUnlimited = -1;
    int depth = kUnlimited;  // levels of objects expanded below the target
    std::string_view keys;     // comma-separated direct children to include
    std::string_view exclude;  // comma-separated direct children to omit
    View view = View::Value;
    // Include mirrored remote nodes' copies (false: leave remote nodes out).
    // Mirrors subscribe with false, so copies are never re-exported.
    bool remotes = true;
    // Subscribe only.
    uint32_t interval = 0;  // minimum ms between change notifications
    bool changes = true;    // deliver change notifications (false: events only)
    bool events = true;     // also deliver events under the path
    bool snapshot = true;   // reply with the current state (false: with null)
    // Act at no more than this level (docs/DESIGN.md section 13.1): what a
    // gateway asks for its client. Below admin, it goes in the envelope's
    // op ("get@user"), which nodes that predate it refuse; a subscription
    // made with one says so in its notifications and ends only with an
    // unsub at the same level.
    Access ceiling = Access::Admin;
};

enum class TransportKind : uint8_t { Local, Serial, Http, NowTP, WebSocket, Udp };

// "local" / "serial" / "http" / "nowtp" / "websocket" / "udp".
const char* toString(TransportKind kind);
bool parseTransportKind(std::string_view s, TransportKind& out);

// The local network interface a route leaves by (docs/DESIGN.md section
// 14.3): Any when it doesn't apply (NowTP) or isn't known.
enum class NetInterface : uint8_t { Any, Ethernet, Wifi, Ap };
// "" / "ethernet" / "wifi" (the station) / "ap" (the soft-AP).
const char* toString(NetInterface iface);
bool parseNetInterface(std::string_view s, NetInterface& out);

// Who sent a request, as far as the transport knows.
struct Client {
    TransportKind transport = TransportKind::Local;
    uint8_t address[16] = {};  // IPv4/IPv6 or MAC, transport-defined
    uint8_t addressLength = 0;
    // The client's device (its Wi-Fi station MAC) when the transport knows
    // it: a NowTP address, or the "id" a UDP peer advertises.
    uint8_t device[6] = {};
    uint8_t deviceLength = 0;
    bool authenticated = false;
    // Set by transports that know the client's level (token checks); an
    // authenticated client without one has full access.
    Access access = Access::Public;

    Access level() const { return authenticated && access == Access::Public ? Access::Admin : access; }
    // Lowers the client to `ceiling` if it is above: an HTTP client whose
    // token has that level (authenticated unless public).
    void cap(Access ceiling) {
        if (level() <= ceiling) return;
        access = ceiling;
        authenticated = ceiling != Access::Public;
    }
};

// How a message to a client should be delivered, for transports that offer
// a choice (NowTP, UDP).
enum class Delivery : uint8_t {
    Reliable,    // retransmitted until acknowledged (requests, responses, events)
    LatestOnly,  // a newer message supersedes older ones
    BestEffort,  // sent once: broadcast
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
    // Same, for a message other subscribers get too (an event): immutable,
    // and counted once (docs/DESIGN.md section 15). A transport that queues
    // it for another task keeps this one rather than a copy.
    virtual bool notifyShared(const std::shared_ptr<const std::string>& message, Delivery delivery) {
        return notify(*message, delivery);
    }
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
    // The last write failed because the heap couldn't hold the reply: the
    // core answers "busy" rather than "too_large".
    virtual bool outOfMemory() const { return false; }
    // A streamed reply keeps what is written, up to a window, rather than
    // send it under the API lock (docs/DESIGN.md section 9): the core calls
    // drain() where it may let go of the lock, and once the request is done.
    // wantsDrain(): something is waiting to be sent.
    virtual bool wantsDrain() const { return false; }
    virtual void drain() {}
};

// Reply that captures status and body into memory: handy for tests and for
// transports that build their own framing around the body.
class StringReply : public Reply {
public:
    explicit StringReply(size_t limit = 0) : sink_(body, limit, true) {}

    Sink& begin(Status s) override {
        status = s;
        body.clear();
        begun = true;
        sink_.lowMemory = false;
        return sink_;
    }
    bool outOfMemory() const override { return sink_.lowMemory; }
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

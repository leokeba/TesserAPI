#include "tesser/datagram.h"

#include <stdio.h>

#include "tesser/envelope.h"
#include "tesser/json_writer.h"
#include "tesser/remote.h"

// A datagram endpoint's calls to peers: requests and their replies,
// timeouts and failures, and the tokens it presents (docs/DESIGN.md
// sections 10.3 and 13.3).

namespace tesser {

std::string buildRequestEnvelope(uint32_t id, Op op, std::string_view path, const Query& query,
                                 std::string_view bodyJson) {
    std::string out;
    StringSink sink(out);
    JsonWriter w(sink);
    w.beginObject();
    if (id) {
        w.key("id");
        w.uinteger(id);
    }
    w.key("op");
    if (query.ceiling < Access::Admin) {
        std::string named = toString(op);
        named += '@';
        named += toString(query.ceiling);
        w.string(named);
    } else {
        w.string(toString(op));
    }
    w.key("path");
    w.string(path.empty() ? std::string_view("/") : path);
    if (query.depth >= 0) {
        w.key("depth");
        w.integer(query.depth);
    }
    if (!query.keys.empty()) {
        w.key("keys");
        w.string(query.keys);
    }
    if (!query.exclude.empty()) {
        w.key("exclude");
        w.string(query.exclude);
    }
    if (query.view != View::Value) {
        w.key("view");
        w.string(query.view == View::Schema ? "schema" : query.view == View::Hash ? "hash" : "info");
    }
    if (!query.remotes) {
        w.key("remotes");
        w.boolean(false);
    }
    if (op == Op::Subscribe) {
        if (query.interval) {
            w.key("interval");
            w.uinteger(query.interval);
        }
        if (!query.changes) {
            w.key("changes");
            w.boolean(false);
        }
        if (!query.events) {
            w.key("events");
            w.boolean(false);
        }
        if (!query.snapshot) {
            w.key("snapshot");
            w.boolean(false);
        }
    }
    // With options, strict (docs/DESIGN.md section 10.1.2): a node that
    // doesn't know one refuses the request rather than ignore it.
    bool options = query.depth >= 0 || !query.keys.empty() || !query.exclude.empty() || query.view != View::Value ||
                   !query.remotes || query.ceiling < Access::Admin ||
                   (op == Op::Subscribe && (query.interval || !query.changes || !query.events || !query.snapshot));
    if (options) {
        w.key("v");
        w.integer(kProtocolVersion);
    }
    if (!bodyJson.empty()) {
        w.key("body");
        w.raw(bodyJson);
    }
    w.endObject();
    return out;
}

// An "ok" reply to a requestRaw() call: its body goes as the peer wrote it.
bool DatagramEndpoint::takeRaw(const Incoming& msg) {
    {
        // At most maxCalls: cheaper than parsing every reply twice.
        MutexGuard guard(mutex_);
        bool any = false;
        for (const CallSlot& c : calls_) any = any || c.raw;
        if (!any) return false;
    }
    JsonDocument filter;
    filter["id"] = true;
    filter["status"] = true;
    JsonDocument head;  // without the body: small whatever its size
    std::string_view text = msg.view();
    if (parseJson(head, text, kWrittenNesting, filter) ||
        head["status"] != "ok" || !head["id"].is<uint32_t>()) {
        return false;
    }
    uint32_t id = head["id"].as<uint32_t>();
    RawHandler raw;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < calls_.size(); i++) {
            CallSlot& c = calls_[i];
            if (c.id != id || c.to != msg.from || !c.raw || c.held || c.each) continue;
            raw = std::move(c.raw);
            // The round trip, for the routes through this peer: under the
            // lock, which a node's destruction takes.
            uint32_t rtt = (clock_ ? clock_() : 0) - c.sentMs;
            for (RemoteNode* r : remotes_) r->routeRoundTrip(this, msg.from, rtt);
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
            stats_.responses++;
            break;
        }
    }
    if (!raw) return false;
    std::string_view body;
    if (!jsonMember(text, "body", body)) body = "null";
    raw(body);
    return true;
}

// A reply to one of our calls (handle()).
void DatagramEndpoint::handleResponse(JsonObjectConst env, const PeerAddress& from) {
    uint32_t id = env["id"].as<uint32_t>();
    Status s = Status::Internal;
    parseStatus(env["status"].as<const char*>(), s);
    bool reauth = false, again = false;
    std::string token;
    ResponseHandler done;
    BroadcastHandler each;  // a broadcast call stays open for more replies
    {
        MutexGuard guard(mutex_);
        stats_.responses++;
        for (size_t i = 0; i < calls_.size(); i++) {
            if (calls_[i].id != id) continue;
            if (calls_[i].each) {
                each = calls_[i].each;
                break;
            }
            if (calls_[i].to == from) {
                if (calls_[i].held) break;  // not sent yet: a stray reply
                PeerAuth* a = authLocked(from);
                if (s == Status::Unauthorized && a && !calls_[i].retried &&
                    (!calls_[i].text.empty() || calls_[i].rebuild)) {
                    // The peer may have forgotten our token (it restarted
                    // unseen, or dropped it): present it again, then this.
                    calls_[i].retried = true;
                    calls_[i].held = true;
                    again = true;
                    if (a->state != PeerAuth::Pending) {
                        a->state = PeerAuth::Pending;
                        reauth = true;
                        token = a->token;
                    }
                    break;
                }
                if (s == Status::Unauthorized && a && a->state == PeerAuth::Done) a->state = PeerAuth::Idle;
                done = std::move(calls_[i].done);
                if (done) {
                    // The round trip, for the routes through this peer:
                    // under the lock, which a node's destruction takes.
                    uint32_t rtt = (clock_ ? clock_() : 0) - calls_[i].sentMs;
                    for (RemoteNode* r : remotes_) r->routeRoundTrip(this, from, rtt);
                }
                calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        if (!done && !each && !again) stats_.unmatched++;
    }
    if (reauth) sendAuth(from, token);
    if (again) return;
    if (done) done(s, env["body"]);
    if (each) each(from, s, env["body"]);
}

namespace {
class StringOut : public DatagramEndpoint::EnvelopeOut {
public:
    explicit StringOut(std::string& s) : s_(s) {}
    bool reserve(size_t bytes) override {
        s_.reserve(bytes);
        return true;
    }
    void append(const char* data, size_t len) override { s_.append(data, len); }

private:
    std::string& s_;
};

// With `slack` spare after the envelope, for the link (NowTP's trailer).
class BytesOut : public DatagramEndpoint::EnvelopeOut {
public:
    BytesOut(Bytes& b, size_t slack) : b_(b), slack_(slack) {}
    bool reserve(size_t bytes) override { return b_.reserve(bytes + slack_); }
    Pool pool(size_t) const override { return Pool::Large; }
    void append(const char* data, size_t len) override {
        if (!b_.append(data, len) && b_.reserve(b_.size() + len + slack_)) b_.append(data, len);
    }

private:
    Bytes& b_;
    size_t slack_;
};
}  // namespace

std::string DatagramEndpoint::rebuilt(const Builder& build, uint32_t id) {
    std::string text;
    StringOut out(text);
    if (!build(id, out)) text.clear();
    return text;
}

bool DatagramEndpoint::requestBuilt(const PeerAddress& to, Builder build, ResponseHandler done, uint32_t timeoutMs,
                                    const void* owner, RawHandler raw) {
    if (!build) return false;
    uint32_t id = nextCallId();
    auto keep = std::make_shared<Builder>(std::move(build));
    if (ownedSend_) {
        Bytes envelope;
        BytesOut out(envelope, ownedSlack_);
        if (!(*keep)(id, out)) return false;
        return sendRequest(to, id, std::string(), std::move(done), timeoutMs, owner, false, std::move(raw), keep,
                           std::move(envelope));
    }
    std::string envelope;
    StringOut out(envelope);
    if (!(*keep)(id, out)) return false;
    return sendRequest(to, id, std::move(envelope), std::move(done), timeoutMs, owner, false, std::move(raw), keep);
}

bool DatagramEndpoint::sendRequest(const PeerAddress& to, uint32_t id, std::string envelope,
                                   ResponseHandler done, uint32_t timeoutMs, const void* owner, bool internal,
                                   RawHandler raw, std::shared_ptr<Builder> rebuild, Bytes owned) {
    // Larger than the peer takes: not sent, rather than lost on the way or
    // refused there.
    size_t size = owned.data() ? owned.size() : envelope.size();
    size_t limit = peerLimit(to);
    if (limit && size > limit) {
        MutexGuard guard(mutex_);
        stats_.tooLarge++;
        return false;
    }
    uint32_t now = clock_ ? clock_() : 0;
    bool hold = false, startAuth = false;
    std::string token;
    {
        MutexGuard guard(mutex_);
        // A token's own "auth" doesn't count: it would hold up what waits for it.
        if (!internal && calls_.size() >= options_.maxCalls) return false;
        CallSlot slot{id, to, now + (timeoutMs ? timeoutMs : options_.timeoutMs), std::move(done), owner, nullptr, now,
                      std::move(raw), std::string(), nullptr, false, false};
        if (PeerAuth* a = internal ? nullptr : authLocked(to)) {
            // Kept to go again: its builder, or a copy, while it waits for
            // the token's answer, or if small. A large one to a peer that took
            // the token isn't kept (up to 4 KB until the reply): if the peer
            // forgot the token, the call answers "unauthorized", and the
            // token is presented again before the next one.
            if (rebuild) {
                slot.rebuild = std::move(rebuild);
            } else if (a->state != PeerAuth::Done || envelope.size() <= kKeptEnvelope) {
                slot.text = envelope;
            }
            if (a->state != PeerAuth::Done) {
                // Datagrams may overtake each other: wait for the token's answer.
                slot.held = hold = true;
                if (a->state == PeerAuth::Idle) {
                    a->state = PeerAuth::Pending;
                    startAuth = true;
                    token = a->token;
                }
            }
        }
        calls_.push_back(std::move(slot));
    }
    if (startAuth) sendAuth(to, token);
    if (hold) return true;
    // Taken by the link, not copied (a large envelope on a short heap).
    if (owned.data()) {
        if (ownedSend_(to, std::move(owned), Delivery::Reliable)) return true;
    } else if (send_ && send_(to, std::move(envelope), Delivery::Reliable)) {
        return true;
    }
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < calls_.size(); i++) {
        if (calls_[i].id == id) {
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    return false;
}

uint32_t DatagramEndpoint::nextCallId() {
    MutexGuard guard(mutex_);
    uint32_t id = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    return id;
}

// ---- credentials ---------------------------------------------------------------

DatagramEndpoint::PeerAuth* DatagramEndpoint::authLocked(const PeerAddress& peer) {
    for (PeerAuth& a : auths_) {
        if (a.peer == peer) return &a;
    }
    return nullptr;
}

void DatagramEndpoint::setPeerToken(const PeerAddress& peer, std::string_view token) {
    std::vector<Held> release;
    {
        MutexGuard guard(mutex_);
        PeerAuth* a = authLocked(peer);
        if (token.empty()) {
            if (!a) return;
            // Nothing to wait for any more.
            for (CallSlot& c : calls_) {
                if (c.held && c.to == peer) {
                    c.held = false;
                    release.push_back(Held{c.id, c.text, c.rebuild});
                }
            }
            auths_.erase(auths_.begin() + (a - auths_.data()));
        } else if (!a) {
            PeerAuth n;
            n.peer = peer;
            n.token.assign(token.data(), token.size());
            auths_.push_back(std::move(n));
        } else if (a->token != token) {
            a->token.assign(token.data(), token.size());
            a->level = Access::Public;
            if (a->state == PeerAuth::Done) a->state = PeerAuth::Idle;
            if (a->state == PeerAuth::Pending) a->renew = true;  // the answer will be the old token's
        }
    }
    for (const Held& h : release) {
        std::string t = h.rebuild ? rebuilt(*h.rebuild, h.id) : h.text;  // built outside the lock
        if (send_ && !t.empty()) send_(peer, std::move(t), Delivery::Reliable);
    }
}

Access DatagramEndpoint::grantedAccess(const PeerAddress& peer) const {
    MutexGuard guard(mutex_);
    for (const PeerAuth& a : auths_) {
        if (a.peer == peer) return a.state == PeerAuth::Done ? a.level : Access::Public;
    }
    return Access::Public;
}

void DatagramEndpoint::sendAuth(const PeerAddress& peer, const std::string& token) {
    std::string text;
    {
        StringSink sink(text);
        JsonWriter w(sink);
        w.beginObject();
        w.key("id");
        w.uinteger(0);  // replaced below
        w.key("op");
        w.string("auth");
        w.key("token");
        w.string(token);
        w.endObject();
    }
    uint32_t id = nextCallId();
    text.replace(0, 7, "{\"id\":" + std::to_string(id));
    PeerAddress to = peer;
    if (!sendRequest(
            peer, id, text, [this, to](Status s, JsonVariantConst body) { authDone(to, s, body); }, 0, this, true)) {
        authDone(peer, Status::Busy, JsonVariantConst());
    }
}

// The token's answer: what waited for it goes now, whatever it was (a
// refused request comes back "unauthorized").
void DatagramEndpoint::authDone(const PeerAddress& peer, Status status, JsonVariantConst body) {
    Access level = Access::Public;
    if (status == Status::Ok) parseAccess(body["access"] | "", level);
    std::vector<Held> release;
    std::string renewed;
    Pinned routed(*this);  // the nodes with a route through the peer
    {
        MutexGuard guard(mutex_);
        PeerAuth* a = authLocked(peer);
        if (a && a->renew) {
            // The token changed meanwhile: present the new one first.
            a->renew = false;
            renewed = a->token;
        } else {
            if (a) {
                a->state = status == Status::Ok ? PeerAuth::Done : PeerAuth::Idle;
                a->level = level;
            }
            for (CallSlot& c : calls_) {
                if (c.held && c.to == peer) {
                    c.held = false;
                    release.push_back(Held{c.id, c.text, c.rebuild});
                }
            }
            for (RemoteNode* r : remotes_) {
                if (r->hasRoute(this, peer)) routed.add(r);
            }
        }
    }
    if (!renewed.empty()) return sendAuth(peer, renewed);
    for (const Held& h : release) {
        std::string t = h.rebuild ? rebuilt(*h.rebuild, h.id) : h.text;  // built outside the lock
        if (send_ && !t.empty()) send_(peer, std::move(t), Delivery::Reliable);
    }
    if (onAuth_) onAuth_(peer, status, level);
    for (RemoteNode* r : routed.nodes) r->tokenAnswered(status, level);
}

bool DatagramEndpoint::requestRaw(const PeerAddress& to, Op op, std::string_view path, JsonVariantConst body,
                                  RawHandler raw, ResponseHandler done, const Query& query, uint32_t timeoutMs,
                                  const void* owner) {
    std::string json;
    if (!body.isNull()) serializeJson(body, json);
    uint32_t id = nextCallId();
    return sendRequest(to, id, buildRequestEnvelope(id, op, path, query, json), std::move(done), timeoutMs, owner,
                       false, std::move(raw));
}

bool DatagramEndpoint::request(const PeerAddress& to, Op op, std::string_view path, std::string_view bodyJson,
                               ResponseHandler done, const Query& query, uint32_t timeoutMs, const void* owner) {
    uint32_t id;
    {
        MutexGuard guard(mutex_);
        id = nextId_++;
        if (nextId_ == 0) nextId_ = 1;
    }
    return sendRequest(to, id, buildRequestEnvelope(id, op, path, query, bodyJson), std::move(done), timeoutMs,
                       owner);
}

bool DatagramEndpoint::request(const PeerAddress& to, Op op, std::string_view path, JsonVariantConst body,
                               ResponseHandler done, const Query& query, uint32_t timeoutMs, const void* owner) {
    std::string json;
    if (!body.isNull()) serializeJson(body, json);
    return request(to, op, path, std::string_view(json), std::move(done), query, timeoutMs, owner);
}

bool DatagramEndpoint::request(const PeerAddress& to, JsonObjectConst envelope, ResponseHandler done,
                               uint32_t timeoutMs, const void* owner) {
    uint32_t id;
    {
        MutexGuard guard(mutex_);
        id = nextId_++;
        if (nextId_ == 0) nextId_ = 1;
    }
    std::string text;
    StringSink sink(text);
    JsonWriter w(sink);
    w.beginObject();
    w.key("id");
    w.uinteger(id);
    for (JsonPairConst kv : envelope) {
        if (kv.key() == "id") continue;
        w.key(std::string_view(kv.key().c_str(), kv.key().size()));
        w.variant(kv.value());
    }
    w.endObject();
    return sendRequest(to, id, text, std::move(done), timeoutMs, owner);
}

bool DatagramEndpoint::requestFields(const PeerAddress& to, std::string_view fields, ResponseHandler done,
                                     uint32_t timeoutMs, const void* owner) {
    uint32_t id;
    {
        MutexGuard guard(mutex_);
        id = nextId_++;
        if (nextId_ == 0) nextId_ = 1;
    }
    char head[20];
    int n = snprintf(head, sizeof(head), "{\"id\":%lu,", static_cast<unsigned long>(id));
    // One allocation of its exact size, if the heap can spare it (docs/DESIGN.md
    // section 15): a relayed file chunk is several KB.
    size_t size = static_cast<size_t>(n) + fields.size() + 1;
    Reservation room = memoryBudget().reserve(size, 0, true, true, mallocPool(size));
    if (!room) return false;
    std::string text;
    text.reserve(size);
    text.append(head, static_cast<size_t>(n));
    text.append(fields.data(), fields.size());
    text += '}';
    return sendRequest(to, id, std::move(text), std::move(done), timeoutMs, owner);
}

bool DatagramEndpoint::broadcast(const PeerAddress& to, Op op, std::string_view path, std::string_view bodyJson,
                                 BroadcastHandler onReply, uint32_t windowMs, const void* owner) {
    if (!onReply) {
        return send_ && send_(to, buildRequestEnvelope(0, op, path, Query(), bodyJson), Delivery::BestEffort);
    }
    uint32_t id;
    uint32_t now = clock_ ? clock_() : 0;
    {
        MutexGuard guard(mutex_);
        if (calls_.size() >= options_.maxCalls) return false;
        id = nextId_++;
        if (nextId_ == 0) nextId_ = 1;
        calls_.push_back(CallSlot{id, to, now + (windowMs ? windowMs : options_.timeoutMs), nullptr, owner,
                                  std::move(onReply), now, nullptr, std::string(), nullptr, false, false});
    }
    if (send_ && send_(to, buildRequestEnvelope(id, op, path, Query(), bodyJson), Delivery::BestEffort)) {
        return true;
    }
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < calls_.size(); i++) {
        if (calls_[i].id == id) {
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    return false;
}

void DatagramEndpoint::cancelCalls(const void* owner) {
    // Destroyed after the lock: a handler's captures may answer a client
    // or take other locks as they go (a forwarded request's, a relay's).
    std::vector<CallSlot> dropped;
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < calls_.size();) {
        if (calls_[i].owner == owner) {
            dropped.push_back(std::move(calls_[i]));
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
}

bool DatagramEndpoint::hasCalls(const void* owner) const {
    MutexGuard guard(mutex_);
    for (const CallSlot& c : calls_) {
        if (c.owner == owner) return true;
    }
    return false;
}

void DatagramEndpoint::sendFailed(const PeerAddress& peer, uint32_t callId, SendFailure failure) {
    {
        MutexGuard guard(mutex_);
        failures_.push_back(Failure{peer, callId, failure});
    }
    if (onQueued_) onQueued_();
}

uint32_t DatagramEndpoint::callIdOf(std::string_view m) {
    // Our requests start {"id":N,"op": (buildRequestEnvelope(), "auth",
    // file envelopes); replies have a "status" after the id instead.
    if (m.compare(0, 6, "{\"id\":") != 0) return 0;
    uint32_t id = 0;
    size_t i = 6;
    while (i < m.size() && m[i] >= '0' && m[i] <= '9' && i < 16) id = id * 10 + uint32_t(m[i++] - '0');
    return m.compare(i, 6, ",\"op\":") == 0 ? id : 0;
}

void DatagramEndpoint::failCall(uint32_t id, Status status, Reason reason, const char* message) {
    if (!id) return;
    CallSlot call;
    bool found = false;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < calls_.size(); i++) {
            if (calls_[i].id != id || calls_[i].each) continue;
            call = std::move(calls_[i]);
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
            found = true;
            break;
        }
    }
    if (!found || !call.done) return;
    JsonDocument body;
    body["error"] = toString(status);
    body["reason"] = toString(reason);
    body["path"] = "/";
    body["message"] = message;
    call.done(status, body.as<JsonVariantConst>());
}

void DatagramEndpoint::expire(uint32_t now, bool all, const PeerAddress* peer, Reason reason) {
    std::vector<CallSlot> expired;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < calls_.size();) {
            bool match = peer ? calls_[i].to == *peer
                              : (all || static_cast<int32_t>(now - calls_[i].deadline) >= 0);
            if (match) {
                // A broadcast call's window closing is its normal end.
                if (!calls_[i].each) stats_.timeouts++;
                expired.push_back(std::move(calls_[i]));
                calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                i++;
            }
        }
    }
    // Why, when the link said so: a gateway passes it on (docs/DESIGN.md
    // section 10.1.2).
    JsonDocument body;
    if (reason != Reason::None) {
        body["error"] = toString(Status::Timeout);
        body["reason"] = toString(reason);
        body["path"] = "/";
        body["message"] = reason == Reason::AuthFailed ? "the remote node holds another pairing key"
                                                       : "the link couldn't deliver to the remote node";
    }
    for (auto& call : expired) {
        if (call.done) call.done(Status::Timeout, body.as<JsonVariantConst>());
        if (call.each) call.each(PeerAddress(), Status::Timeout, JsonVariantConst());
    }
}

size_t DatagramEndpoint::pendingCalls() const {
    MutexGuard guard(mutex_);
    return calls_.size();
}

}  // namespace tesser

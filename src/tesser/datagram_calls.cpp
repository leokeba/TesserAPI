#include "tesser/datagram.h"

#include <stdio.h>

#include "tesser/envelope.h"
#include "tesser/json_writer.h"
#include "tesser/remote.h"

// A datagram endpoint's calls to peers (detail::CallTable): requests and their
// replies, timeouts and failures, and the tokens it presents (docs/DESIGN.md
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
        } else if (!query.eventHint.empty()) {
            w.key("events");
            w.string(query.eventHint);
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

namespace detail {

// An "ok" reply to a requestRaw() call: its body goes as the peer wrote it.
bool CallTable::takeRaw(const PeerAddress& from, std::string_view text) {
    {
        // At most maxCalls: cheaper than parsing every reply twice.
        MutexGuard guard(ep_.mutex_);
        bool any = false;
        for (const Slot& c : calls_) any = any || c.raw;
        if (!any) return false;
    }
    JsonDocument filter;
    filter["id"] = true;
    filter["status"] = true;
    JsonDocument head;  // without the body: small whatever its size
    if (parseJson(head, text, kWrittenNesting, filter) ||
        head["status"] != "ok" || !head["id"].is<uint32_t>()) {
        return false;
    }
    uint32_t id = head["id"].as<uint32_t>();
    RawHandler raw;
    {
        MutexGuard guard(ep_.mutex_);
        for (size_t i = 0; i < calls_.size(); i++) {
            Slot& c = calls_[i];
            if (c.id != id || c.to != from || !c.raw || c.held || c.each) continue;
            raw = std::move(c.raw);
            // The round trip, for the routes through this peer: under the
            // lock, which a node's destruction takes.
            uint32_t rtt = ep_.now() - c.sentMs;
            for (RemoteNode* r : ep_.mounts_.remotes()) r->routeRoundTrip(&ep_, from, rtt);
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
            ep_.stats_.responses++;
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
void CallTable::handleResponse(JsonObjectConst env, const PeerAddress& from) {
    uint32_t id = env["id"].as<uint32_t>();
    Status s = Status::Internal;
    parseStatus(env["status"].as<const char*>(), s);
    bool reauth = false, again = false;
    std::string token;
    ResponseHandler done;
    BroadcastHandler each;  // a broadcast call stays open for more replies
    {
        MutexGuard guard(ep_.mutex_);
        ep_.stats_.responses++;
        for (size_t i = 0; i < calls_.size(); i++) {
            if (calls_[i].id != id) continue;
            if (calls_[i].each) {
                each = calls_[i].each;
                break;
            }
            if (calls_[i].to == from) {
                if (calls_[i].held) break;  // not sent yet: a stray reply
                PeerTable::Presented* a = ep_.peers_.presented(from);
                if (s == Status::Unauthorized && a && !calls_[i].retried &&
                    (!calls_[i].text.empty() || calls_[i].rebuild)) {
                    // The peer may have forgotten our token (it restarted
                    // unseen, or dropped it): present it again, then this.
                    calls_[i].retried = true;
                    calls_[i].held = true;
                    again = true;
                    if (a->state != PeerTable::Presented::Pending) {
                        a->state = PeerTable::Presented::Pending;
                        reauth = true;
                        token = a->token;
                    }
                    break;
                }
                if (s == Status::Unauthorized && a && a->state == PeerTable::Presented::Done) a->state = PeerTable::Presented::Idle;
                done = std::move(calls_[i].done);
                if (done) {
                    // The round trip, for the routes through this peer:
                    // under the lock, which a node's destruction takes.
                    uint32_t rtt = ep_.now() - calls_[i].sentMs;
                    for (RemoteNode* r : ep_.mounts_.remotes()) r->routeRoundTrip(&ep_, from, rtt);
                }
                calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        if (!done && !each && !again) ep_.stats_.unmatched++;
    }
    if (reauth) sendAuth(from, token);
    if (again) return;
    if (done) done(s, env["body"]);
    if (each) each(from, s, env["body"]);
}

std::string CallTable::rebuilt(const Builder& build, uint32_t id) {
    std::string text;
    StringOut out(text);
    if (!build(id, out)) text.clear();
    return text;
}

bool CallTable::send(const PeerAddress& to, uint32_t id, std::string envelope, ResponseHandler done, uint32_t timeoutMs,
                     const void* owner, bool internal, RawHandler raw, std::shared_ptr<Builder> rebuild, Bytes owned) {
    // Larger than the peer takes: not sent, rather than lost on the way or
    // refused there.
    size_t size = owned.data() ? owned.size() : envelope.size();
    size_t limit = ep_.peerLimit(to);
    if (limit && size > limit) {
        MutexGuard guard(ep_.mutex_);
        ep_.stats_.tooLarge++;
        return false;
    }
    uint32_t now = ep_.now();
    bool hold = false, startAuth = false;
    std::string token;
    {
        MutexGuard guard(ep_.mutex_);
        // A token's own "auth" doesn't count: it would hold up what waits for it.
        if (!internal && calls_.size() >= ep_.options_.maxCalls) return false;
        Slot slot{id, to, now + (timeoutMs ? timeoutMs : ep_.options_.timeoutMs), std::move(done), owner, nullptr, now,
                      std::move(raw), std::string(), nullptr, false, false};
        if (PeerTable::Presented* a = internal ? nullptr : ep_.peers_.presented(to)) {
            // Kept to go again: its builder, or a copy, while it waits for
            // the token's answer, or if small. A large one to a peer that took
            // the token isn't kept (up to 4 KB until the reply): if the peer
            // forgot the token, the call answers "unauthorized", and the
            // token is presented again before the next one.
            if (rebuild) {
                slot.rebuild = std::move(rebuild);
            } else if (a->state != PeerTable::Presented::Done || envelope.size() <= kKeptEnvelope) {
                slot.text = envelope;
            }
            if (a->state != PeerTable::Presented::Done) {
                // Datagrams may overtake each other: wait for the token's answer.
                slot.held = hold = true;
                if (a->state == PeerTable::Presented::Idle) {
                    a->state = PeerTable::Presented::Pending;
                    startAuth = true;
                    token = a->token;
                }
            }
        }
        calls_.push_back(std::move(slot));
    }
    if (startAuth) sendAuth(to, token);
    if (hold) {
        ep_.wake();  // nothing goes on the link yet, but its timeout counts
        return true;
    }
    // Taken by the link, not copied (a large envelope on a short heap).
    if (owned.data()) {
        if (ep_.ownedSend_(to, std::move(owned), Delivery::Reliable)) return true;
    } else if (ep_.send_ && ep_.send_(to, std::move(envelope), Delivery::Reliable)) {
        return true;
    }
    MutexGuard guard(ep_.mutex_);
    for (size_t i = 0; i < calls_.size(); i++) {
        if (calls_[i].id == id) {
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    return false;
}

uint32_t CallTable::nextId() {
    MutexGuard guard(ep_.mutex_);
    uint32_t id = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    return id;
}

// ---- credentials ---------------------------------------------------------------

void CallTable::setToken(const PeerAddress& peer, std::string_view token) {
    std::vector<Held> released;
    {
        MutexGuard guard(ep_.mutex_);
        PeerTable::Presented* a = ep_.peers_.presented(peer);
        if (token.empty()) {
            if (!a) return;
            // Nothing to wait for any more.
            for (Slot& c : calls_) {
                if (c.held && c.to == peer) {
                    c.held = false;
                    released.push_back(Held{c.id, c.text, c.rebuild});
                }
            }
            ep_.peers_.stopPresenting(peer);
        } else if (!a) {
            ep_.peers_.present(peer).token.assign(token.data(), token.size());
        } else if (a->token != token) {
            a->token.assign(token.data(), token.size());
            a->level = Access::Public;
            if (a->state == PeerTable::Presented::Done) a->state = PeerTable::Presented::Idle;
            if (a->state == PeerTable::Presented::Pending) a->renew = true;  // the answer will be the old token's
        }
    }
    release(peer, released);
}

void CallTable::release(const PeerAddress& peer, const std::vector<Held>& held) {
    for (const Held& h : held) {
        std::string t = h.rebuild ? rebuilt(*h.rebuild, h.id) : h.text;  // built outside the lock
        if (ep_.send_ && !t.empty()) ep_.send_(peer, std::move(t), Delivery::Reliable);
    }
}

Access CallTable::granted(const PeerAddress& peer) const {
    MutexGuard guard(ep_.mutex_);
    const PeerTable::Presented* a = ep_.peers_.presented(peer);
    return a && a->state == PeerTable::Presented::Done ? a->level : Access::Public;
}

void CallTable::sendAuth(const PeerAddress& peer, const std::string& token) {
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
    uint32_t id = nextId();
    text.replace(0, 7, "{\"id\":" + std::to_string(id));
    PeerAddress to = peer;
    if (!send(
            peer, id, text, [this, to](Status s, JsonVariantConst body) { authDone(to, s, body); }, 0, this, true)) {
        authDone(peer, Status::Busy, JsonVariantConst());
    }
}

// The token's answer: what waited for it goes now, whatever it was (a
// refused request comes back "unauthorized").
void CallTable::authDone(const PeerAddress& peer, Status status, JsonVariantConst body) {
    Access level = Access::Public;
    if (status == Status::Ok) parseAccess(body["access"] | "", level);
    std::vector<Held> released;
    std::string renewed;
    Mounter::Pinned routed(ep_.mounts_);  // the nodes with a route through the peer
    {
        MutexGuard guard(ep_.mutex_);
        PeerTable::Presented* a = ep_.peers_.presented(peer);
        if (a && a->renew) {
            // The token changed meanwhile: present the new one first.
            a->renew = false;
            renewed = a->token;
        } else {
            if (a) {
                a->state = status == Status::Ok ? PeerTable::Presented::Done : PeerTable::Presented::Idle;
                a->level = level;
            }
            for (Slot& c : calls_) {
                if (c.held && c.to == peer) {
                    c.held = false;
                    released.push_back(Held{c.id, c.text, c.rebuild});
                }
            }
            for (RemoteNode* r : ep_.mounts_.remotes()) {
                if (r->hasRoute(&ep_, peer)) routed.add(r);
            }
        }
    }
    if (!renewed.empty()) return sendAuth(peer, renewed);
    release(peer, released);
    if (ep_.onAuth_) ep_.onAuth_(peer, status, level);
    for (RemoteNode* r : routed.nodes) r->tokenAnswered(status, level);
}

bool CallTable::broadcast(const PeerAddress& to, std::string_view path, Op op, std::string_view bodyJson,
                          BroadcastHandler onReply, uint32_t windowMs, const void* owner) {
    uint32_t id;
    uint32_t now = ep_.now();
    {
        MutexGuard guard(ep_.mutex_);
        if (calls_.size() >= ep_.options_.maxCalls) return false;
        id = nextId_++;
        if (nextId_ == 0) nextId_ = 1;
        calls_.push_back(Slot{id, to, now + (windowMs ? windowMs : ep_.options_.timeoutMs), nullptr, owner,
                              std::move(onReply), now, nullptr, std::string(), nullptr, false, false});
    }
    if (ep_.send_ && ep_.send_(to, buildRequestEnvelope(id, op, path, Query(), bodyJson), Delivery::BestEffort)) {
        ep_.wake();  // the window's end is due; a broadcast leaves the link nothing to wait for
        return true;
    }
    MutexGuard guard(ep_.mutex_);
    for (size_t i = 0; i < calls_.size(); i++) {
        if (calls_[i].id == id) {
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    return false;
}

void CallTable::cancel(const void* owner) {
    // Destroyed after the lock: a handler's captures may answer a client
    // or take other locks as they go (a forwarded request's, a relay's).
    std::vector<Slot> dropped;
    MutexGuard guard(ep_.mutex_);
    for (size_t i = 0; i < calls_.size();) {
        if (calls_[i].owner == owner) {
            dropped.push_back(std::move(calls_[i]));
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
}

bool CallTable::has(const void* owner) const {
    MutexGuard guard(ep_.mutex_);
    for (const Slot& c : calls_) {
        if (c.owner == owner) return true;
    }
    return false;
}

uint32_t CallTable::matching(const PeerAddress& peer, uint32_t partial, uint32_t mask) const {
    uint32_t id = 0;  // none: a reply or a notification, or the call is gone
    for (const Slot& c : calls_) {
        if (c.to == peer && (c.id & mask) == partial) id = c.id;
    }
    return id;
}

void CallTable::nextTick(Deadline& d) const {
    for (const Slot& c : calls_) d.at(c.deadline);
}

void CallTable::fail(uint32_t id, Status status, Reason reason, const char* message) {
    if (!id) return;
    Slot call;
    bool found = false;
    {
        MutexGuard guard(ep_.mutex_);
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

void CallTable::expire(uint32_t now, bool all, const PeerAddress* peer, Reason reason) {
    std::vector<Slot> expired;
    {
        MutexGuard guard(ep_.mutex_);
        for (size_t i = 0; i < calls_.size();) {
            bool match = peer ? calls_[i].to == *peer
                              : (all || static_cast<int32_t>(now - calls_[i].deadline) >= 0);
            if (match) {
                // A broadcast call's window closing is its normal end.
                if (!calls_[i].each) ep_.stats_.timeouts++;
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

size_t CallTable::pending() const {
    MutexGuard guard(ep_.mutex_);
    return calls_.size();
}

}  // namespace detail

bool DatagramEndpoint::requestBuilt(const PeerAddress& to, Builder build, ResponseHandler done, uint32_t timeoutMs,
                                    const void* owner, RawHandler raw) {
    if (!build) return false;
    uint32_t id = calls_.nextId();
    auto keep = std::make_shared<Builder>(std::move(build));
    if (ownedSend_) {
        Bytes envelope;
        BytesOut out(envelope, ownedSlack_);
        if (!(*keep)(id, out)) return false;
        return calls_.send(to, id, std::string(), std::move(done), timeoutMs, owner, false, std::move(raw), keep,
                           std::move(envelope));
    }
    std::string envelope;
    StringOut out(envelope);
    if (!(*keep)(id, out)) return false;
    return calls_.send(to, id, std::move(envelope), std::move(done), timeoutMs, owner, false, std::move(raw), keep);
}

bool DatagramEndpoint::requestRaw(const PeerAddress& to, Op op, std::string_view path, JsonVariantConst body,
                                  RawHandler raw, ResponseHandler done, const Query& query, uint32_t timeoutMs,
                                  const void* owner) {
    std::string json;
    if (!body.isNull()) serializeJson(body, json);
    uint32_t id = calls_.nextId();
    return calls_.send(to, id, buildRequestEnvelope(id, op, path, query, json), std::move(done), timeoutMs, owner,
                       false, std::move(raw));
}

bool DatagramEndpoint::request(const PeerAddress& to, Op op, std::string_view path, std::string_view bodyJson,
                               ResponseHandler done, const Query& query, uint32_t timeoutMs, const void* owner) {
    uint32_t id = calls_.nextId();
    return calls_.send(to, id, buildRequestEnvelope(id, op, path, query, bodyJson), std::move(done), timeoutMs, owner);
}

bool DatagramEndpoint::request(const PeerAddress& to, Op op, std::string_view path, JsonVariantConst body,
                               ResponseHandler done, const Query& query, uint32_t timeoutMs, const void* owner) {
    std::string json;
    if (!body.isNull()) serializeJson(body, json);
    return request(to, op, path, std::string_view(json), std::move(done), query, timeoutMs, owner);
}

bool DatagramEndpoint::request(const PeerAddress& to, JsonObjectConst envelope, ResponseHandler done,
                               uint32_t timeoutMs, const void* owner) {
    uint32_t id = calls_.nextId();
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
    return calls_.send(to, id, text, std::move(done), timeoutMs, owner);
}

bool DatagramEndpoint::requestFields(const PeerAddress& to, std::string_view fields, ResponseHandler done,
                                     uint32_t timeoutMs, const void* owner) {
    uint32_t id = calls_.nextId();
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
    return calls_.send(to, id, std::move(text), std::move(done), timeoutMs, owner);
}

bool DatagramEndpoint::broadcast(const PeerAddress& to, Op op, std::string_view path, std::string_view bodyJson,
                                 BroadcastHandler onReply, uint32_t windowMs, const void* owner) {
    if (!onReply) {
        return send_ && send_(to, buildRequestEnvelope(0, op, path, Query(), bodyJson), Delivery::BestEffort);
    }
    return calls_.broadcast(to, path, op, bodyJson, std::move(onReply), windowMs, owner);
}

void DatagramEndpoint::sendFailed(const PeerAddress& peer, uint32_t callId, SendFailure failure, uint32_t idMask) {
    {
        MutexGuard guard(mutex_);
        if (idMask != 0xffffffffu && callId) {
            callId = calls_.matching(peer, callId, idMask);
        }
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

}  // namespace tesser

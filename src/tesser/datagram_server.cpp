#include "tesser/datagram.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/api.h"
#include "tesser/call.h"
#include "tesser/envelope.h"
#include "tesser/file_session.h"
#include "tesser/json_writer.h"

// Serving a datagram endpoint's peers (detail::EndpointServer): their
// requests, subscriptions, file transfers and tokens.

namespace tesser {
namespace detail {

using SendFn = DatagramTypes::SendFn;

namespace {

// Sends a reply to a peer: one type, for one std::function manager. The
// link's send is copied: a deferred reply can outlast this call.
struct ReplySender {
    SendFn send;
    PeerAddress to;
    bool silent;
    void operator()(std::string& m) const {
        if (send && !silent) send(to, std::move(m), Delivery::Reliable);
    }
};

}  // namespace

EndpointServer::EndpointServer(DatagramEndpoint& endpoint) : ep_(endpoint) {}
EndpointServer::~EndpointServer() = default;

bool EndpointServer::PeerSubscriber::notify(const std::string& message, Delivery delivery) {
    DatagramEndpoint& ep = server_.ep_;
    if (ep.options_.maxMessage && message.size() > ep.options_.maxMessage) return false;
    return ep.send_ && ep.send_(peer, message, delivery);
}

bool EndpointServer::PeerSubscriber::notifyOwned(std::string&& message, Delivery delivery) {
    DatagramEndpoint& ep = server_.ep_;
    if (ep.options_.maxMessage && message.size() > ep.options_.maxMessage) return false;
    return ep.send_ && ep.send_(peer, std::move(message), delivery);
}

bool EndpointServer::PeerSubscriber::recheckLevel(Access& level) {
    DatagramEndpoint& ep = server_.ep_;
    if (ep.trust_ && ep.trust_(peer)) return false;
    level = server_.levelOf(peer);
    return true;
}

void EndpointServer::dropAll() {
    files_.clear();
    MutexGuard guard(subscribersMutex_);
    for (PeerSubscriber* s : subscribers_) {
        if (ep_.api_) ep_.api_->dropSubscriber(s);
        delete s;
    }
    subscribers_.clear();
}

void EndpointServer::dropSubscriber(const PeerAddress& peer) {
    MutexGuard guard(subscribersMutex_);
    for (size_t i = 0; i < subscribers_.size(); i++) {
        if (subscribers_[i]->peer != peer) continue;
        if (ep_.api_) ep_.api_->dropSubscriber(subscribers_[i]);
        delete subscribers_[i];
        subscribers_.erase(subscribers_.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
}

void EndpointServer::checkIdle() {
    if (idleCheck_.exchange(false)) dropIdleSubscribers(nullptr);
}

void EndpointServer::nextTick(Deadline& d) const {
    uint32_t idleMs = ep_.api_ ? ep_.api_->config().transferIdleMs : 0;
    for (const PeerFiles& f : files_) {
        if (!f.session->busy()) {
            d.in(0);
        } else if (idleMs) {
            d.at(f.usedMs + idleMs);
        }
    }
    if (idleCheck_.load()) d.in(DatagramEndpoint::kRetryMs);
}

size_t EndpointServer::subscribers() const {
    MutexGuard guard(subscribersMutex_);
    return subscribers_.size();
}

Subscriber* EndpointServer::subscriberFor(const PeerAddress& peer, bool create) {
    MutexGuard guard(subscribersMutex_);
    for (PeerSubscriber* s : subscribers_) {
        if (s->peer == peer) return s;
    }
    if (!create) return nullptr;
    auto* s = new PeerSubscriber(*this, peer);
    subscribers_.push_back(s);
    return s;
}

void EndpointServer::ownReplies(EnvelopeReply& reply, const PeerAddress& to, bool silent) {
    if (!ep_.ownedSend_) return;
    DatagramTypes::OwnedSendFn fn = ep_.ownedSend_;
    reply.sendOwned(
        [fn, to, silent](Bytes& m) {
            if (!silent) fn(to, std::move(m), Delivery::Reliable);
        },
        ep_.ownedSlack_);
}

// A message that can't be taken or parsed now: a request is answered busy,
// and a reply fails its call, at once, rather than either waiting for a
// timeout and a resend (docs/DESIGN.md section 15).
void EndpointServer::refuse(const PeerAddress& from, std::string_view text, bool broadcast, Reason reason) {
    JsonDocument filter;
    filter["id"] = true;
    filter["status"] = true;
    JsonDocument doc;
    if (parseJson(doc, text, kWrittenNesting, filter)) {
        return;
    }
    if (doc["status"].is<const char*>()) {
        ep_.calls_.fail(doc["id"].as<uint32_t>(), Status::Busy, reason,
                 reason == Reason::LowMemory ? "not enough memory for the reply" : "no room for the reply");
    } else {
        reject(from, text, broadcast, Status::Busy, reason,
               reason == Reason::LowMemory ? "not enough memory for the request" : "request queue full");
    }
}

void EndpointServer::reject(const PeerAddress& from, std::string_view text, bool broadcast, Status status,
                              Reason reason, const char* message) {
    // Only requests get an error reply; responses and notifications are dropped.
    JsonDocument filter;
    filter["id"] = true;
    filter["op"] = true;
    JsonDocument doc;
    if (parseJson(doc, text, kWrittenNesting, filter)) {
        return;
    }
    if (!doc["op"].is<const char*>()) return;
    // A broadcast request without an id is never answered.
    if (broadcast && doc["id"].isNull()) return;
    EnvelopeId id;
    JsonVariantConst idv = doc["id"];
    if (!idv.isNull()) {
        std::string raw;
        serializeJson(idv, raw);
        if (raw.size() <= EnvelopeId::kMax) {
            memcpy(id.json, raw.data(), raw.size());
            id.length = raw.size();
        }
    }
    PeerAddress to = from;
    DatagramTypes::SendFn send = ep_.send_;
    EnvelopeReply reply(id, 0, [send, to](std::string& m) {
        if (send) send(to, std::move(m), Delivery::Reliable);
    });
    writeError(reply, status, reason, std::string_view(), message);
}

namespace {

// Levels of nesting in a parsed value; scalars have none.
int nesting(JsonVariantConst v) {
    int deepest = 0;
    if (v.is<JsonObjectConst>()) {
        for (JsonPairConst kv : v.as<JsonObjectConst>()) {
            int n = nesting(kv.value());
            if (n > deepest) deepest = n;
        }
    } else if (v.is<JsonArrayConst>()) {
        for (JsonVariantConst e : v.as<JsonArrayConst>()) {
            int n = nesting(e);
            if (n > deepest) deepest = n;
        }
    } else {
        return 0;
    }
    return deepest + 1;
}

}  // namespace

// A peer's token, from "auth": kept until the next one or until the peer is
// forgotten. A trusted peer is admin whatever it sends.
struct EndpointServer::PeerToken : TokenClient {
    PeerToken(EndpointServer& s, const PeerAddress& p) : server(s), peer(p) {}
    Access tokenLevel() override {
        DatagramEndpoint& ep = server.ep_;
        return ep.trust_ && ep.trust_(peer) ? Access::Admin : server.levelOf(peer);
    }
    void setToken(std::string_view token) override {
        DatagramEndpoint& ep = server.ep_;
        MutexGuard guard(ep.mutex_);
        ep.peers_.holdToken(peer, token, ep.options_.maxTokens);
    }
    void levelFell() override {
        {
            MutexGuard guard(server.subscribersMutex_);
            for (PeerSubscriber* sub : server.subscribers_) {
                if (sub->peer == peer) server.ep_.api_->dropSubscriber(sub);
            }
        }
        for (size_t i = 0; i < server.files_.size(); i++) {
            if (server.files_[i].peer == peer) {
                server.files_.erase(server.files_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
    EndpointServer& server;
    const PeerAddress& peer;
};

void EndpointServer::serve(JsonObjectConst env, const Incoming& msg) {
    {
        MutexGuard guard(ep_.mutex_);
        ep_.stats_.requests++;
    }
    EnvelopeCall call;
    Request& req = call.request;
    req.client = clientOf(msg.from);
    // A trusted peer is admin; any other is where the token check puts its
    // token, like an HTTP request's, and at least at setPeerAccess().
    if (ep_.trust_ && ep_.trust_(msg.from)) {
        req.client.authenticated = true;
    } else {
        req.client.access = levelOf(msg.from);
        req.client.authenticated = req.client.access != Access::Public;
    }
    readEnvelope(env, kTakesAuth | kTakesFiles, call);
    using Kind = EnvelopeCall::Kind;
    if (call.status == Status::Ok && call.kind != Kind::File &&
        nesting(env) > envelopeNesting(ep_.api_->config().maxDepth)) {
        call.status = Status::BadRequest;
        call.message = "envelope nested too deeply";
    }
    // A broadcast request without an id is never answered.
    bool silent = msg.broadcast && call.id.length == 0;
    EnvelopeReply reply(call.id, ep_.replyLimitFor(msg.from), ReplySender{ep_.send_, msg.from, silent});
    ownReplies(reply, msg.from, silent);
    if (call.failed(reply)) return;
    if (call.kind == Kind::File) {
        handleFile(env, msg, req.client, reply);
        return;
    }
    if (call.kind == Kind::Auth) {
        PeerToken token(*this, msg.from);
        answerAuth(env, token, reply);
        return;
    }
    bool reliable = msg.reliable || (req.op == Op::Set && msg.broadcast && ep_.options_.broadcastSet);
    if ((req.op == Op::Set || req.op == Op::Subscribe) && ep_.options_.requireReliableSet && !reliable) {
        writeError(reply, Status::NotAllowed, req.path, "set and sub requests must be sent reliably");
        return;
    }
    if (req.op == Op::Subscribe || req.op == Op::Unsubscribe) {
        req.subscriber = subscriberFor(msg.from, req.op == Op::Subscribe);
    }
    ep_.api_->handle(req, reply);
    if (req.subscriber) dropIdleSubscribers(req.subscriber);
}

// A peer holds a subscriber only while it has a subscription: one refused,
// or the last one ended, leaves nothing behind. In queued mode the request
// may still wait for poll() with the subscriber: it stays, and a later
// process() looks again. Queued requests are counted first: one that
// poll() took out runs under the API lock, which counting subscriptions
// waits for.
void EndpointServer::dropIdleSubscribers(const Subscriber* only) {
    MutexGuard guard(subscribersMutex_);
    for (size_t i = 0; i < subscribers_.size();) {
        PeerSubscriber* s = subscribers_[i];
        if (only && s != only) {
            i++;
            continue;
        }
        if (ep_.api_->queuedRequests(s)) {
            idleCheck_ = true;
        } else if (!ep_.api_->subscriptionsOf(s)) {
            delete s;
            subscribers_.erase(subscribers_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        i++;
    }
}

Client EndpointServer::clientOf(const PeerAddress& peer) const {
    Client client;
    client.transport = ep_.kind_;
    memcpy(client.address, peer.bytes, peer.length < 16 ? peer.length : 16);
    client.addressLength = peer.length < 16 ? peer.length : 16;
    PeerAddress device = ep_.deviceOf(peer);
    if (device.length == 6) {
        memcpy(client.device, device.bytes, 6);
        client.deviceLength = 6;
    }
    return client;
}

Access EndpointServer::levelOf(const PeerAddress& peer) {
    std::string token;
    {
        MutexGuard guard(ep_.mutex_);
        ep_.peers_.heldToken(peer, token);
    }
    Access level = ep_.api_->tokenAccess(token, clientOf(peer));
    Access least = ep_.peerAccess_.load();
    return level > least ? level : least;
}

// "upload" and "download" (docs/DESIGN.md section 10.1.1): each peer has a
// session, kept while it transfers.
void EndpointServer::handleFile(JsonObjectConst envelope, const Incoming& msg, const Client& client,
                                EnvelopeReply& reply) {
    std::string_view op;
    Access ceiling;
    splitOp(envelope["op"] | "", op, ceiling);
    if (op == "upload" && ep_.options_.requireReliableSet && !msg.reliable) {
        writeError(reply, Status::NotAllowed, std::string_view(), "uploads must be sent reliably");
        return;
    }
    uint32_t now = ep_.now();
    PeerFiles* files = nullptr;
    for (PeerFiles& f : files_) {
        if (f.peer == msg.from) files = &f;
    }
    if (!files) {
        // A peer's first transfer: only so many at once (Options::
        // maxFileSessions); one with nothing in progress makes room.
        if (ep_.options_.maxFileSessions && files_.size() >= ep_.options_.maxFileSessions) {
            for (size_t i = 0; i < files_.size(); i++) {
                if (files_[i].session->done()) {
                    files_.erase(files_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        if (ep_.options_.maxFileSessions && files_.size() >= ep_.options_.maxFileSessions) {
            writeError(reply, Status::Busy, Reason::TransferBusy, std::string_view(), "too many transfers in progress");
            return;
        }
        files_.push_back(PeerFiles{msg.from, std::unique_ptr<FileSession>(new FileSession(*ep_.api_)), now});
        files = &files_.back();
    }
    files->usedMs = now;
    files->session->handle(envelope, client, reply, msg.view());
}

// Drops the sessions of `peer`, or, without one, those with nothing open
// and those idle for Config::transferIdleMs.
void EndpointServer::dropFiles(const PeerAddress* peer, uint32_t now) {
    uint32_t idleMs = ep_.api_ ? ep_.api_->config().transferIdleMs : 0;
    for (size_t i = 0; i < files_.size();) {
        PeerFiles& f = files_[i];
        bool drop = peer ? f.peer == *peer
                         : !f.session->busy() || (idleMs && now - f.usedMs >= idleMs);
        if (drop) {
            files_.erase(files_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
}

}  // namespace detail
}  // namespace tesser

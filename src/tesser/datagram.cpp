#include "tesser/datagram.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/api.h"
#include "tesser/call.h"
#include "tesser/client.h"
#include "tesser/envelope.h"
#include "tesser/file_session.h"
#include "tesser/json_writer.h"
#include "tesser/remote.h"

// A datagram endpoint's core: taking messages from the link, process(), and
// serving the requests of peers. Calls to peers are in datagram_calls.cpp,
// the peers this endpoint knows of in datagram_peers.cpp (docs/DESIGN.md
// section 16).

namespace tesser {

DatagramEndpoint::DatagramEndpoint(Api* api, TransportKind kind, SendFn send, ClockFn clock, Options options)
    : api_(api),
      kind_(kind),
      incoming_("incoming", checked(options).queueBytes, checked(options).queueBytesPerPeer),
      send_(std::move(send)),
      clock_(std::move(clock)),
      options_(checked(options)) {}

DatagramEndpoint::Options DatagramEndpoint::checked(Options o) {
    if (o.maxMessage && o.queueBytesPerPeer && o.queueBytesPerPeer < o.maxMessage) o.queueBytesPerPeer = o.maxMessage;
    if (o.queueBytes && o.queueBytesPerPeer && o.queueBytes < o.queueBytesPerPeer) o.queueBytes = o.queueBytesPerPeer;
    return o;
}

size_t DatagramEndpoint::replyLimit() const {
    size_t limit = api_ ? api_->config().maxResponse : 0;
    size_t link = options_.maxMessage;
    if (!limit || (link && link < limit)) limit = link;
    return limit;
}

size_t DatagramEndpoint::peerLimit(const PeerAddress& peer) const {
    size_t limit = options_.maxMessage;
    auto lower = [&limit](size_t l) {
        if (l && (!limit || l < limit)) limit = l;
    };
    {
        MutexGuard guard(mutex_);
        for (const Known& k : known_) {
            if (k.peer == peer) lower(k.maxMessage);
        }
    }
    if (peerLimitFn_) lower(peerLimitFn_(peer));  // the link's lock: not under ours
    return limit;
}

size_t DatagramEndpoint::replyLimitFor(const PeerAddress& peer) const {
    size_t limit = replyLimit();
    size_t theirs = peerLimit(peer);
    if (theirs && (!limit || theirs < limit)) limit = theirs;
    return limit;
}

// Pending handlers are dropped without being called: whatever they capture
// may already be gone. (A captured Pending still answers its own client.)
DatagramEndpoint::~DatagramEndpoint() {
    std::vector<CallSlot> dropped;  // destroyed after the lock, as by cancelCalls()
    {
        MutexGuard guard(mutex_);
        dropped.swap(calls_);
        for (RemoteNode* r : remotes_) r->detach(this);
        remotes_.clear();
    }
    files_.clear();
    MutexGuard guard(subscribersMutex_);
    for (PeerSubscriber* s : subscribers_) {
        if (api_) api_->dropSubscriber(s);
        delete s;
    }
    subscribers_.clear();
}

Subscriber* DatagramEndpoint::subscriberFor(const PeerAddress& peer, bool create) {
    MutexGuard guard(subscribersMutex_);
    for (PeerSubscriber* s : subscribers_) {
        if (s->peer == peer) return s;
    }
    if (!create) return nullptr;
    auto* s = new PeerSubscriber(*this, peer);
    subscribers_.push_back(s);
    return s;
}

void DatagramEndpoint::ownReplies(EnvelopeReply& reply, const PeerAddress& to, bool silent) {
    if (!ownedSend_) return;
    OwnedSendFn fn = ownedSend_;
    reply.sendOwned(
        [fn, to, silent](Bytes& m) {
            if (!silent) fn(to, std::move(m), Delivery::Reliable);
        },
        ownedSlack_);
}

uintptr_t budgetKey(const PeerAddress& p) {
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < p.length; i++) h = (h ^ p.bytes[i]) * 16777619u;
    return h ? h : 1;
}

Reservation DatagramEndpoint::hold(const PeerAddress& from, std::string_view text, bool reliable, bool broadcast,
                                   bool heap) {
    Reservation r = incoming_.reserve(text.size(), budgetKey(from), true, heap, mallocPool(text.size()));
    if (!r) {
        {
            MutexGuard guard(mutex_);
            stats_.dropped++;
        }
        if (refusable(reliable, broadcast)) return r;  // the link refuses it: sent again
        // Answered, or its call failed, so nobody waits for a timeout: the
        // queue's own limit is queue_full, the heap low_memory (a relay then
        // asks for less, docs/DESIGN.md section 14).
        bool queueRoom = static_cast<bool>(incoming_.reserve(text.size(), budgetKey(from), true, false));
        refuse(from, text, broadcast, heap && queueRoom ? Reason::LowMemory : Reason::QueueFull);
    }
    return r;
}

bool DatagramEndpoint::receive(const PeerAddress& from, const char* data, size_t len, bool reliable,
                               bool broadcast) {
    // The link's buffer (NowTP's) is only lent: copied if the budget, the
    // heap floor and the largest block allow it (docs/DESIGN.md section 15).
    // If not, the link refuses it (linkRefuses()), or a request is answered
    // busy and a reply fails its call.
    Reservation held = hold(from, std::string_view(data, len), reliable, broadcast, true);
    if (!held) return !refusable(reliable, broadcast);
    Incoming msg;
    msg.from = from;
    msg.text.assign(data, len);
    held.settle();
    msg.held = std::move(held);
    msg.reliable = reliable;
    msg.broadcast = broadcast;
    return queue(std::move(msg));
}

bool DatagramEndpoint::receive(const PeerAddress& from, std::string&& text, bool reliable, bool broadcast) {
    // Allocated already: counted, not checked against the heap.
    Reservation held = hold(from, text, reliable, broadcast, false);
    if (!held) return !refusable(reliable, broadcast);
    Incoming msg;
    msg.from = from;
    msg.text = std::move(text);
    msg.held = std::move(held);
    msg.reliable = reliable;
    msg.broadcast = broadcast;
    return queue(std::move(msg));
}

bool DatagramEndpoint::receiveOwned(const PeerAddress& from, void* buffer, size_t len, bool reliable,
                                    bool broadcast) {
    Incoming msg;
    msg.owned.reset(static_cast<char*>(buffer));  // freed even when refused
    Reservation held = hold(from, std::string_view(msg.owned.get(), len), reliable, broadcast, false);
    if (!held) return !refusable(reliable, broadcast);
    msg.from = from;
    msg.ownedLen = len;
    msg.held = std::move(held);
    msg.reliable = reliable;
    msg.broadcast = broadcast;
    return queue(std::move(msg));
}

bool DatagramEndpoint::queue(Incoming&& msg) {
    bool queued = false;
    {
        MutexGuard guard(mutex_);
        if (queue_.size() < options_.queueLimit) {
            queue_.push_back(std::move(msg));
            queued = true;
        } else {
            stats_.dropped++;
        }
    }
    if (queued) {
        if (onQueued_) onQueued_();
        return true;
    }
    if (refusable(msg.reliable, msg.broadcast)) return false;  // the link refuses it: sent again
    // Full: answer requests with "busy" so clients needn't wait for a timeout.
    reject(msg.from, msg.view(), msg.broadcast, Status::Busy, Reason::QueueFull, "request queue full");
    return true;
}

bool DatagramEndpoint::process() {
    bool worked = false;
    std::vector<std::pair<PeerAddress, PeerLoss>> lost;
    std::vector<PeerAddress> unheard;
    std::vector<Seen> seen;
    std::vector<Failure> failures;
    {
        MutexGuard guard(mutex_);
        lost.swap(lostPeers_);
        unheard.swap(unheardPeers_);
        seen.swap(seenPeers_);
        failures.swap(failures_);
    }
    for (const auto& p : lost) {
        dropPeer(p.first, p.second);
        worked = true;
    }
    for (const PeerAddress& p : unheard) {
        if (!deferLoss(p)) dropPeer(p, PeerLoss::Lost);
        worked = true;
    }
    for (const Failure& f : failures) {
        if (f.failure == SendFailure::Rejected) {
            failCall(f.callId, Status::Internal, Reason::Rejected,
                     "the remote node refused the message (no listener on the port, or too large)");
        } else if (f.failure == SendFailure::AuthFailed) {
            dropPeer(f.peer, PeerLoss::AuthFailed);
        } else {
            unreachable(f.peer);
        }
        worked = true;
    }
    for (const Seen& s : seen) {
        applySeen(s);
        worked = true;
    }
    for (;;) {
        Incoming msg;
        {
            MutexGuard guard(mutex_);
            if (queue_.empty()) break;
            msg = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        markOnline(msg.from, true);
        handle(msg);
        worked = true;
    }
    size_t before = pendingCalls();
    uint32_t now = clock_ ? clock_() : 0;
    expire(now, false, nullptr);
    {
        Pinned remotes(*this);
        {
            MutexGuard guard(mutex_);
            remotes.all();
        }
        for (RemoteNode* r : remotes.nodes) r->tick(*this, now);
    }
    dropFiles(nullptr, now);
    dropDeferred(now);
    checkAdvertisement(now);
    if (idleCheck_.exchange(false)) dropIdleSubscribers(nullptr);
    return worked || pendingCalls() != before;
}

// A message that can't be taken or parsed now: a request is answered busy,
// and a reply fails its call, at once, rather than either waiting for a
// timeout and a resend (docs/DESIGN.md section 15).
void DatagramEndpoint::refuse(const PeerAddress& from, std::string_view text, bool broadcast, Reason reason) {
    JsonDocument filter;
    filter["id"] = true;
    filter["status"] = true;
    JsonDocument doc;
    if (parseJson(doc, text, kWrittenNesting, filter)) {
        return;
    }
    if (doc["status"].is<const char*>()) {
        failCall(doc["id"].as<uint32_t>(), Status::Busy, reason,
                 reason == Reason::LowMemory ? "not enough memory for the reply" : "no room for the reply");
    } else {
        reject(from, text, broadcast, Status::Busy, reason,
               reason == Reason::LowMemory ? "not enough memory for the request" : "request queue full");
    }
}

void DatagramEndpoint::reject(const PeerAddress& from, std::string_view text, bool broadcast, Status status,
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
    SendFn send = send_;
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

namespace {

constexpr size_t kNone = std::string_view::npos;

size_t skipSpace(std::string_view s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

// Past the string whose opening quote is at `i`.
size_t skipString(std::string_view s, size_t i) {
    for (i++; i < s.size(); i++) {
        if (s[i] == '\\') {
            i++;
        } else if (s[i] == '"') {
            return i + 1;
        }
    }
    return kNone;
}

// Past the value starting at `i`.
size_t skipValue(std::string_view s, size_t i) {
    if (i >= s.size()) return kNone;
    if (s[i] == '"') return skipString(s, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == '"') {
                i = skipString(s, i);
                if (i == kNone) return kNone;
                continue;
            }
            if (c == '{' || c == '[') depth++;
            if ((c == '}' || c == ']') && --depth == 0) return i + 1;
            i++;
        }
        return kNone;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' &&
           s[i] != '\r' && s[i] != '\t') {
        i++;
    }
    return i;
}

}  // namespace

// Each key of the JSON object `json` (valid JSON) to `each`, in order, until
// it returns false; false if `json` isn't an object.
bool jsonKeys(std::string_view json, const std::function<bool(std::string_view key)>& each) {
    size_t i = skipSpace(json, 0);
    if (i >= json.size() || json[i] != '{') return false;
    i = skipSpace(json, i + 1);
    if (i < json.size() && json[i] == '}') return true;
    for (;;) {
        if (i >= json.size() || json[i] != '"') return false;
        size_t end = skipString(json, i);
        if (end == kNone) return false;
        if (!each(json.substr(i + 1, end - i - 2))) return true;
        i = skipSpace(json, end);
        if (i >= json.size() || json[i] != ':') return false;
        size_t valueEnd = skipValue(json, skipSpace(json, i + 1));
        if (valueEnd == kNone) return false;
        i = skipSpace(json, valueEnd);
        if (i < json.size() && json[i] == '}') return true;
        if (i >= json.size() || json[i] != ',') return false;
        i = skipSpace(json, i + 1);
    }
}

// The text of member `key` of the JSON object `json` (valid JSON).
bool jsonMember(std::string_view json, std::string_view key, std::string_view& out) {
    size_t i = skipSpace(json, 0);
    if (i >= json.size() || json[i] != '{') return false;
    i = skipSpace(json, i + 1);
    if (i < json.size() && json[i] == '}') return false;
    for (;;) {
        if (i >= json.size() || json[i] != '"') return false;
        size_t end = skipString(json, i);
        if (end == kNone) return false;
        std::string_view k = json.substr(i + 1, end - i - 2);
        i = skipSpace(json, end);
        if (i >= json.size() || json[i] != ':') return false;
        i = skipSpace(json, i + 1);
        size_t valueEnd = skipValue(json, i);
        if (valueEnd == kNone) return false;
        if (k == key) {
            out = json.substr(i, valueEnd - i);
            return true;
        }
        i = skipSpace(json, valueEnd);
        if (i >= json.size() || json[i] != ',') return false;
        i = skipSpace(json, i + 1);
    }
}

void DatagramEndpoint::handle(Incoming& msg) {
    if (takeRaw(msg)) return;
    // Parsed, a large message takes about as much again: only above the
    // heap floor (dropped otherwise, like a lost datagram).
    std::string_view text = msg.view();
    Reservation parsing;  // the document, about as large as the text
    if (text.size() > 1024) {
        // An upload's document leaves its data out: small. In PSRAM when
        // the chip has it: a remote's whole tree or schema, say.
        parsing = memoryBudget().reserve(isUploadEnvelope(text) ? 512 : 2 * text.size(), 0, false, true, Pool::Large);
        if (!parsing) {
            {
                MutexGuard guard(mutex_);
                stats_.dropped++;
            }
            refuse(msg.from, msg.view(), msg.broadcast, Reason::LowMemory);
            return;
        }
    }
    // Responses and notifications nest as deep as the peer wrote them;
    // requests are held to the same limit as on other transports below.
    JsonDocument doc(jsonAllocator(parsing ? parsing.pool() : Pool::Internal));
    // An upload's data is read from the text, not copied into the document.
    DeserializationError err =
        isUploadEnvelope(text)
            ? parseJson(doc, text, kWrittenNesting, uploadFilter())
            : parseJson(doc, text, kWrittenNesting);
    if (err || !doc.is<JsonObjectConst>()) {
        {
            MutexGuard guard(mutex_);
            stats_.dropped++;
        }
        // The heap, not the message: answered busy (or its call failed).
        if (err == DeserializationError::NoMemory) refuse(msg.from, text, msg.broadcast, Reason::LowMemory);
        return;
    }
    JsonObjectConst env = doc.as<JsonObjectConst>();

    // Response to one of our calls.
    if (env["status"].is<const char*>()) return handleResponse(env, msg.from);

    // Notification from a node we subscribed to: never answered.
    std::string_view op = env["op"].is<const char*>() ? std::string_view(env["op"].as<const char*>()) : "";
    if (op == "change" || op == "event") {
        {
            Pinned remotes(*this);
            {
                MutexGuard guard(mutex_);
                remotes.all();
            }
            // Remote nodes take what concerns their mirror; the application's
            // handler and clients of the peer still see everything.
            for (RemoteNode* r : remotes.nodes) {
                if (r->handleNotification(*this, msg.from, env)) break;
            }
        }
        std::vector<std::shared_ptr<ClientInbox>> inboxes;
        {
            MutexGuard guard(mutex_);
            for (auto& c : clients_) {
                if (c.first == msg.from) inboxes.push_back(c.second);
            }
        }
        for (auto& inbox : inboxes) inbox->notification(std::string(text));
        if (onNotification_) onNotification_(msg.from, env);
        return;
    }

    // Request.
    if (op.empty() || !api_) {
        MutexGuard guard(mutex_);
        stats_.dropped++;
        return;
    }
    {
        MutexGuard guard(mutex_);
        stats_.requests++;
    }
    Request req;
    req.client = clientOf(msg.from);
    // A trusted peer is admin; any other is where the token check puts its
    // token, like an HTTP request's, and at least at setPeerAccess().
    if (trust_ && trust_(msg.from)) {
        req.client.authenticated = true;
    } else {
        req.client.access = levelOf(msg.from);
        req.client.authenticated = req.client.access != Access::Public;
    }
    if (FileSession::handles(env)) {
        handleFile(env, msg, req.client);
        return;
    }
    EnvelopeId id;
    const char* message = nullptr;
    Reason reason = Reason::None;
    bool auth = op == "auth";
    Status s = auth ? envelopeId(env, id, message) : requestFromEnvelope(env, req, id, message, &reason);
    if (s == Status::Ok && nesting(env) > envelopeNesting(api_->config().maxDepth)) {
        s = Status::BadRequest;
        message = "envelope nested too deeply";
    }
    PeerAddress to = msg.from;
    SendFn send = send_;
    // A broadcast request without an id is never answered.
    bool silent = msg.broadcast && id.length == 0;
    EnvelopeReply reply(id, replyLimitFor(to), [send, to, silent](std::string& m) {
        if (send && !silent) send(to, std::move(m), Delivery::Reliable);
    });
    ownReplies(reply, to, silent);
    if (s == Status::Ok && auth) s = checkStrict(env, kAuthKeys, message, &reason);
    if (s != Status::Ok) {
        writeError(reply, s, reason, std::string_view(), message);
        return;
    }
    if (auth) {
        handleAuth(env, msg, reply);
        return;
    }
    bool reliable = msg.reliable || (req.op == Op::Set && msg.broadcast && options_.broadcastSet);
    if ((req.op == Op::Set || req.op == Op::Subscribe) && options_.requireReliableSet && !reliable) {
        writeError(reply, Status::NotAllowed, req.path, "set and sub requests must be sent reliably");
        return;
    }
    if (req.op == Op::Subscribe || req.op == Op::Unsubscribe) {
        req.subscriber = subscriberFor(msg.from, req.op == Op::Subscribe);
    }
    api_->handle(req, reply);
    if (req.subscriber) dropIdleSubscribers(req.subscriber);
}

// A peer holds a subscriber only while it has a subscription: one refused,
// or the last one ended, leaves nothing behind. In queued mode the request
// may still wait for poll() with the subscriber: it stays, and a later
// process() looks again. Queued requests are counted first: one that
// poll() took out runs under the API lock, which counting subscriptions
// waits for.
void DatagramEndpoint::dropIdleSubscribers(const Subscriber* only) {
    MutexGuard guard(subscribersMutex_);
    for (size_t i = 0; i < subscribers_.size();) {
        PeerSubscriber* s = subscribers_[i];
        if (only && s != only) {
            i++;
            continue;
        }
        if (api_->queuedRequests(s)) {
            idleCheck_ = true;
        } else if (!api_->subscriptionsOf(s)) {
            delete s;
            subscribers_.erase(subscribers_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        i++;
    }
}

Client DatagramEndpoint::clientOf(const PeerAddress& peer) const {
    Client client;
    client.transport = kind_;
    memcpy(client.address, peer.bytes, peer.length < 16 ? peer.length : 16);
    client.addressLength = peer.length < 16 ? peer.length : 16;
    PeerAddress device = deviceOf(peer);
    if (device.length == 6) {
        memcpy(client.device, device.bytes, 6);
        client.deviceLength = 6;
    }
    return client;
}

Access DatagramEndpoint::levelOf(const PeerAddress& peer) {
    std::string token;
    {
        MutexGuard guard(mutex_);
        for (auto& t : tokens_) {
            if (t.first == peer) token = t.second;
        }
    }
    Access level = api_->tokenAccess(token, clientOf(peer));
    Access least = peerAccess_.load();
    return level > least ? level : least;
}

// {"op":"auth","token":"..."} sets the peer's token until the next one or
// until the peer is forgotten; an empty or missing token signs out. As on a
// line (docs/DESIGN.md section 10.4), the reply gives the level, and a lower
// one drops the peer's subscriptions and file transfer.
void DatagramEndpoint::handleAuth(JsonObjectConst envelope, const Incoming& msg, Reply& reply) {
    JsonVariantConst token = envelope["token"];
    if (!token.isNull() && !token.is<const char*>()) {
        writeError(reply, Status::BadRequest, std::string_view(), "token must be a string");
        return;
    }
    bool trusted = trust_ && trust_(msg.from);
    Access before = trusted ? Access::Admin : levelOf(msg.from);
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < tokens_.size(); i++) {
            if (tokens_[i].first == msg.from) {
                tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        JsonString t = token.as<JsonString>();
        if (t.size()) {
            if (tokens_.size() >= options_.maxTokens && !tokens_.empty()) tokens_.erase(tokens_.begin());
            if (options_.maxTokens) tokens_.emplace_back(msg.from, std::string(t.c_str(), t.size()));
        }
    }
    Access after = trusted ? Access::Admin : levelOf(msg.from);
    if (after < before) {
        // Subscriptions and a transfer keep the level they were opened at.
        {
            MutexGuard guard(subscribersMutex_);
            for (PeerSubscriber* sub : subscribers_) {
                if (sub->peer == msg.from) api_->dropSubscriber(sub);
            }
        }
        for (size_t i = 0; i < files_.size(); i++) {
            if (files_[i].peer == msg.from) {
                files_.erase(files_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
    std::string body = std::string("{\"access\":\"") + toString(after) + "\"}";
    reply.begin(Status::Ok).write(body.data(), body.size());
    reply.end();
}

// "upload" and "download" (docs/DESIGN.md section 10.1.1): each peer has a
// session, kept while it transfers.
void DatagramEndpoint::handleFile(JsonObjectConst envelope, const Incoming& msg, const Client& client) {
    EnvelopeId id;
    const char* message = nullptr;
    Status s = envelopeId(envelope, id, message);
    PeerAddress to = msg.from;
    SendFn send = send_;
    bool silent = msg.broadcast && id.length == 0;
    EnvelopeReply reply(id, replyLimitFor(to), [send, to, silent](std::string& m) {
        if (send && !silent) send(to, std::move(m), Delivery::Reliable);
    });
    ownReplies(reply, to, silent);
    if (s != Status::Ok) {
        writeError(reply, s, std::string_view(), message);
        return;
    }
    std::string_view op;
    Access ceiling;
    splitOp(envelope["op"] | "", op, ceiling);
    if (op == "upload" && options_.requireReliableSet && !msg.reliable) {
        writeError(reply, Status::NotAllowed, std::string_view(), "uploads must be sent reliably");
        return;
    }
    uint32_t now = clock_ ? clock_() : 0;
    PeerFiles* files = nullptr;
    for (PeerFiles& f : files_) {
        if (f.peer == msg.from) files = &f;
    }
    if (!files) {
        // A peer's first transfer: only so many at once (Options::
        // maxFileSessions); one with nothing in progress makes room.
        if (options_.maxFileSessions && files_.size() >= options_.maxFileSessions) {
            for (size_t i = 0; i < files_.size(); i++) {
                if (files_[i].session->done()) {
                    files_.erase(files_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        if (options_.maxFileSessions && files_.size() >= options_.maxFileSessions) {
            writeError(reply, Status::Busy, Reason::TransferBusy, std::string_view(), "too many transfers in progress");
            return;
        }
        files_.push_back(PeerFiles{msg.from, std::unique_ptr<FileSession>(new FileSession(*api_)), now});
        files = &files_.back();
    }
    files->usedMs = now;
    files->session->handle(envelope, client, reply, msg.view());
}

// Drops the sessions of `peer`, or, without one, those with nothing open
// and those idle for Config::transferIdleMs.
void DatagramEndpoint::dropFiles(const PeerAddress* peer, uint32_t now) {
    uint32_t idleMs = api_ ? api_->config().transferIdleMs : 0;
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

size_t DatagramEndpoint::peerSubscribers() const {
    MutexGuard guard(subscribersMutex_);
    return subscribers_.size();
}

void DatagramEndpoint::addClient(const PeerAddress& peer, std::shared_ptr<ClientInbox> inbox) {
    MutexGuard guard(mutex_);
    clients_.emplace_back(peer, std::move(inbox));
}

void DatagramEndpoint::removeClient(const ClientInbox* inbox) {
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < clients_.size(); i++) {
        if (clients_[i].second.get() == inbox) {
            clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

DatagramEndpoint::Stats DatagramEndpoint::stats() const {
    MutexGuard guard(mutex_);
    return stats_;
}

}  // namespace tesser

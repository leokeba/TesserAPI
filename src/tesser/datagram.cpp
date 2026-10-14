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

std::string buildAdvertisement(uint8_t port, uint32_t schemaHash) {
    char meta[48];
    int n = snprintf(meta, sizeof(meta), "{\"tesser\":%u,\"schema\":\"%08lx\",\"v\":%d}", unsigned(port),
                     static_cast<unsigned long>(schemaHash), kProtocolVersion);
    return std::string(meta, n > 0 ? static_cast<size_t>(n) : 0);
}

bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash) {
    uint8_t protocol;
    return parseAdvertisement(metadata, port, schemaHash, protocol);
}

bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash, uint8_t& protocol) {
    JsonDocument doc;
    if (deserializeJson(doc, metadata.data(), metadata.size())) return false;
    if (!doc["tesser"].is<uint8_t>() || !doc["schema"].is<const char*>()) return false;
    port = doc["tesser"].as<uint8_t>();
    schemaHash = static_cast<uint32_t>(strtoul(doc["schema"].as<const char*>(), nullptr, 16));
    protocol = doc["v"].is<uint8_t>() && doc["v"].as<uint8_t>() ? doc["v"].as<uint8_t>() : uint8_t(1);
    return true;
}

DatagramEndpoint::DatagramEndpoint(Api* api, TransportKind kind, SendFn send, ClockFn clock, Options options)
    : api_(api), kind_(kind), send_(std::move(send)), clock_(std::move(clock)), options_(options) {}

// Pending handlers are dropped without being called: whatever they capture
// may already be gone. (A captured Pending still answers its own client.)
DatagramEndpoint::~DatagramEndpoint() {
    {
        MutexGuard guard(mutex_);
        calls_.clear();
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

const char* toString(PeerLoss reason) {
    switch (reason) {
        case PeerLoss::Lost: return "lost";
        case PeerLoss::Failed: return "failed";
        case PeerLoss::Goodbye: return "goodbye";
        case PeerLoss::Restarted: return "restarted";
        case PeerLoss::Silent: return "silent";
        case PeerLoss::Stopped: return "stopped";
    }
    return "lost";
}

void DatagramEndpoint::dropPeer(const PeerAddress& peer, PeerLoss reason) {
    // Offline first: a remote node whose call fails below goes by another
    // route at once.
    markOnline(peer, false);
    expire(0, true, &peer);
    dropFiles(&peer, 0);
    bool wasHeard = false;
    PeerAddress device;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < tokens_.size(); i++) {
            if (tokens_[i].first == peer) {
                tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        if (PeerAuth* a = authLocked(peer)) {
            // It no longer knows our token.
            a->state = PeerAuth::Idle;
            a->level = Access::Public;
            a->renew = false;
        }
        if (Known* k = knownLocked(peer)) {
            wasHeard = k->online;
            k->online = false;
            device = k->device;
        }
    }
    {
        MutexGuard guard(subscribersMutex_);
        for (size_t i = 0; i < subscribers_.size(); i++) {
            if (subscribers_[i]->peer != peer) continue;
            if (api_) api_->dropSubscriber(subscribers_[i]);
            delete subscribers_[i];
            subscribers_.erase(subscribers_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (wasHeard && onPeerLost_) onPeerLost_(peer, device, reason);
}

DatagramEndpoint::Known* DatagramEndpoint::knownLocked(const PeerAddress& peer) {
    for (Known& k : known_) {
        if (k.peer == peer) return &k;
    }
    return nullptr;
}

// Remembers a peer heard from; past 32, the one silent longest is forgotten.
DatagramEndpoint::Known& DatagramEndpoint::learnLocked(const PeerAddress& peer, uint32_t now) {
    if (Known* k = knownLocked(peer)) {
        k->online = true;
        k->heardMs = now;
        return *k;
    }
    if (known_.size() >= 32) {
        size_t oldest = 0;
        for (size_t i = 1; i < known_.size(); i++) {
            bool older = known_[i].online == known_[oldest].online
                             ? static_cast<int32_t>(known_[i].heardMs - known_[oldest].heardMs) < 0
                             : !known_[i].online;
            if (older) oldest = i;
        }
        known_.erase(known_.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    // A NowTP address is the device's MAC.
    known_.push_back(Known{peer, kind_ == TransportKind::NowTP ? peer : PeerAddress(), true, now});
    return known_.back();
}

bool DatagramEndpoint::deviceOnline(const PeerAddress& device) const {
    if (!device.length) return false;
    MutexGuard guard(mutex_);
    for (const Known& k : known_) {
        if (k.online && k.device == device) return true;
    }
    return false;
}

PeerAddress DatagramEndpoint::deviceOf(const PeerAddress& peer) const {
    MutexGuard guard(mutex_);
    for (const Known& k : known_) {
        if (k.peer == peer) return k.device;
    }
    return kind_ == TransportKind::NowTP ? peer : PeerAddress();
}

void watchDevices(std::vector<DatagramEndpoint*> endpoints, DatagramEndpoint::PeerLossHandler lost) {
    auto shared = std::make_shared<std::vector<DatagramEndpoint*>>(std::move(endpoints));
    for (DatagramEndpoint* ep : *shared) {
        ep->onPeerLost(DatagramEndpoint::PeerLossHandler(
            [shared, lost](const PeerAddress& peer, const PeerAddress& device, PeerLoss reason) {
                // A restart lost the device's state even though it is back.
                if (device.length && reason != PeerLoss::Restarted) {
                    for (DatagramEndpoint* other : *shared) {
                        if (other->deviceOnline(device)) return;
                    }
                }
                if (lost) lost(peer, device, reason);
            }));
    }
}

void watchDevices(std::vector<DatagramEndpoint*> endpoints, DatagramEndpoint::PeerLostHandler lost) {
    watchDevices(std::move(endpoints),
                 DatagramEndpoint::PeerLossHandler([lost](const PeerAddress& peer, const PeerAddress& device, PeerLoss) {
                     if (lost) lost(peer, device);
                 }));
}

void DatagramEndpoint::receive(const PeerAddress& from, const char* data, size_t len, bool reliable,
                               bool broadcast) {
    // The link's buffer (NowTP's) is only lent: a large message is copied,
    // if the heap can spare it. If not, it is dropped like a lost datagram,
    // and the call it belongs to times out.
    if (len > 256 && (!heapAbove(2 * len) || largestFreeBlock() < len + 1024)) {
        MutexGuard guard(mutex_);
        stats_.dropped++;
        return;
    }
    receive(from, std::string(data, len), reliable, broadcast);
}

void DatagramEndpoint::receive(const PeerAddress& from, std::string&& text, bool reliable, bool broadcast) {
    Incoming msg;
    msg.from = from;
    msg.text = std::move(text);
    msg.reliable = reliable;
    msg.broadcast = broadcast;
    queue(std::move(msg));
}

void DatagramEndpoint::receiveOwned(const PeerAddress& from, void* buffer, size_t len, bool reliable,
                                    bool broadcast) {
    Incoming msg;
    msg.from = from;
    msg.owned.reset(static_cast<char*>(buffer));
    msg.ownedLen = len;
    msg.reliable = reliable;
    msg.broadcast = broadcast;
    queue(std::move(msg));
}

void DatagramEndpoint::queue(Incoming&& msg) {
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
        return;
    }
    // Full: answer requests with "busy" so clients needn't wait for a timeout.
    reject(msg, Status::Busy, Reason::QueueFull, "request queue full");
}

bool DatagramEndpoint::process() {
    bool worked = false;
    std::vector<std::pair<PeerAddress, PeerLoss>> lost;
    std::vector<Seen> seen;
    {
        MutexGuard guard(mutex_);
        lost.swap(lostPeers_);
        seen.swap(seenPeers_);
    }
    for (const auto& p : lost) {
        dropPeer(p.first, p.second);
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
    std::vector<RemoteNode*> remotes;
    {
        MutexGuard guard(mutex_);
        remotes = remotes_;
    }
    for (RemoteNode* r : remotes) r->tick(*this, now);
    dropFiles(nullptr, now);
    checkAdvertisement(now);
    return worked || pendingCalls() != before;
}

void DatagramEndpoint::autoAdvertise(std::function<void(uint32_t)> publish, uint32_t current, uint32_t debounceMs) {
    publish_ = std::move(publish);
    publishedHash_ = current;
    seenRevision_ = schemaRevision();
    advertiseDebounceMs_ = debounceMs;
    revisionPending_ = false;
}

void DatagramEndpoint::checkAdvertisement(uint32_t now) {
    if (!publish_ || !api_) return;
    uint32_t rev = schemaRevision();
    if (rev != seenRevision_) {
        // Changes come in bursts (several peers mounted at once): wait for quiet.
        seenRevision_ = rev;
        revisionAtMs_ = now;
        revisionPending_ = true;
        return;
    }
    if (!revisionPending_ || now - revisionAtMs_ < advertiseDebounceMs_) return;
    revisionPending_ = false;
    uint32_t hash = api_->schemaHash();
    if (hash == publishedHash_) return;
    publishedHash_ = hash;
    publish_(hash);
}

void DatagramEndpoint::reject(const Incoming& msg, Status status, Reason reason, const char* message) {
    // Only requests get an error reply; responses and notifications are dropped.
    JsonDocument filter;
    filter["id"] = true;
    filter["op"] = true;
    JsonDocument doc;
    std::string_view text = msg.view();
    if (deserializeJson(doc, text.data(), text.size(), DeserializationOption::Filter(filter),
                        DeserializationOption::NestingLimit(kWrittenNesting))) {
        return;
    }
    if (!doc["op"].is<const char*>()) return;
    // A broadcast request without an id is never answered.
    if (msg.broadcast && doc["id"].isNull()) return;
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
    PeerAddress to = msg.from;
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
    if (deserializeJson(head, text.data(), text.size(), DeserializationOption::Filter(filter),
                        DeserializationOption::NestingLimit(kWrittenNesting)) ||
        head["status"] != "ok" || !head["id"].is<uint32_t>()) {
        return false;
    }
    uint32_t id = head["id"].as<uint32_t>();
    RawHandler raw;
    uint32_t sentMs = 0;
    std::vector<RemoteNode*> remotes;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < calls_.size(); i++) {
            CallSlot& c = calls_[i];
            if (c.id != id || c.to != msg.from || !c.raw || c.held || c.each) continue;
            raw = std::move(c.raw);
            sentMs = c.sentMs;
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
            stats_.responses++;
            remotes = remotes_;
            break;
        }
    }
    if (!raw) return false;
    uint32_t rtt = (clock_ ? clock_() : 0) - sentMs;
    for (RemoteNode* r : remotes) r->routeRoundTrip(this, msg.from, rtt);
    std::string_view body;
    if (!jsonMember(text, "body", body)) body = "null";
    raw(body);
    return true;
}

void DatagramEndpoint::handle(Incoming& msg) {
    if (takeRaw(msg)) return;
    // Parsed, a large message takes about as much again: only above the
    // heap floor (dropped otherwise, like a lost datagram).
    std::string_view text = msg.view();
    if (text.size() > 1024 && !heapAbove(2 * text.size())) {
        MutexGuard guard(mutex_);
        stats_.dropped++;
        return;
    }
    // Responses and notifications nest as deep as the peer wrote them;
    // requests are held to the same limit as on other transports below.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, text.data(), text.size(),
                                               DeserializationOption::NestingLimit(kWrittenNesting));
    if (err || !doc.is<JsonObjectConst>()) {
        MutexGuard guard(mutex_);
        stats_.dropped++;
        return;
    }
    JsonObjectConst env = doc.as<JsonObjectConst>();

    // Response to one of our calls.
    if (env["status"].is<const char*>()) {
        uint32_t id = env["id"].as<uint32_t>();
        Status s = Status::Internal;
        parseStatus(env["status"].as<const char*>(), s);
        bool reauth = false, again = false;
        std::string token;
        ResponseHandler done;
        BroadcastHandler each;  // a broadcast call stays open for more replies
        uint32_t sentMs = 0;
        std::vector<RemoteNode*> remotes;
        {
            MutexGuard guard(mutex_);
            stats_.responses++;
            for (size_t i = 0; i < calls_.size(); i++) {
                if (calls_[i].id != id) continue;
                if (calls_[i].each) {
                    each = calls_[i].each;
                    break;
                }
                if (calls_[i].to == msg.from) {
                    if (calls_[i].held) break;  // not sent yet: a stray reply
                    PeerAuth* a = authLocked(msg.from);
                    if (s == Status::Unauthorized && a && !calls_[i].retried && !calls_[i].text.empty()) {
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
                    done = std::move(calls_[i].done);
                    sentMs = calls_[i].sentMs;
                    remotes = remotes_;
                    calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            if (!done && !each && !again) stats_.unmatched++;
        }
        if (reauth) sendAuth(msg.from, token);
        if (again) return;
        if (done) {
            // The round trip, for the routes through this peer.
            uint32_t rtt = (clock_ ? clock_() : 0) - sentMs;
            for (RemoteNode* r : remotes) r->routeRoundTrip(this, msg.from, rtt);
        }
        if (done) done(s, env["body"]);
        if (each) each(msg.from, s, env["body"]);
        return;
    }

    // Notification from a node we subscribed to: never answered.
    std::string_view op = env["op"].is<const char*>() ? std::string_view(env["op"].as<const char*>()) : "";
    if (op == "change" || op == "event") {
        std::vector<RemoteNode*> remotes;
        {
            MutexGuard guard(mutex_);
            remotes = remotes_;
        }
        // Remote nodes take what concerns their mirror; the application's
        // handler and clients of the peer still see everything.
        for (RemoteNode* r : remotes) {
            if (r->handleNotification(*this, msg.from, env)) break;
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
    req.client.transport = kind_;
    memcpy(req.client.address, msg.from.bytes, msg.from.length < 16 ? msg.from.length : 16);
    req.client.addressLength = msg.from.length < 16 ? msg.from.length : 16;
    PeerAddress device = deviceOf(msg.from);
    if (device.length == 6) {
        memcpy(req.client.device, device.bytes, 6);
        req.client.deviceLength = 6;
    }
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
    EnvelopeReply reply(id, api_->config().maxResponse, [send, to, silent](std::string& m) {
        if (send && !silent) send(to, std::move(m), Delivery::Reliable);
    });
    if (s == Status::Ok && auth) s = checkStrict(env, {"v", "id", "op", "token"}, message, &reason);
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
}

Access DatagramEndpoint::levelOf(const PeerAddress& peer) {
    std::string token;
    {
        MutexGuard guard(mutex_);
        for (auto& t : tokens_) {
            if (t.first == peer) token = t.second;
        }
    }
    Access level = api_->tokenAccess(token);
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
    EnvelopeReply reply(id, api_->config().maxResponse, [send, to, silent](std::string& m) {
        if (send && !silent) send(to, std::move(m), Delivery::Reliable);
    });
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
        files_.push_back(PeerFiles{msg.from, std::unique_ptr<FileSession>(new FileSession(*api_)), now});
        files = &files_.back();
    }
    files->usedMs = now;
    files->session->handle(envelope, client, reply);
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

bool DatagramEndpoint::sendRequest(const PeerAddress& to, uint32_t id, const std::string& envelope,
                                   ResponseHandler done, uint32_t timeoutMs, const void* owner, bool internal,
                                   RawHandler raw) {
    uint32_t now = clock_ ? clock_() : 0;
    bool hold = false, startAuth = false;
    std::string token;
    {
        MutexGuard guard(mutex_);
        // A token's own "auth" doesn't count: it would hold up what waits for it.
        if (!internal && calls_.size() >= options_.maxCalls) return false;
        CallSlot slot{id, to, now + (timeoutMs ? timeoutMs : options_.timeoutMs), std::move(done), owner, nullptr, now,
                      std::move(raw), std::string(), false, false};
        if (PeerAuth* a = internal ? nullptr : authLocked(to)) {
            slot.text = envelope;
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
    if (send_ && send_(to, envelope, Delivery::Reliable)) return true;
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
    std::vector<std::string> release;
    {
        MutexGuard guard(mutex_);
        PeerAuth* a = authLocked(peer);
        if (token.empty()) {
            if (!a) return;
            // Nothing to wait for any more.
            for (CallSlot& c : calls_) {
                if (c.held && c.to == peer) {
                    c.held = false;
                    release.push_back(c.text);
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
    for (const std::string& t : release) {
        if (send_) send_(peer, t, Delivery::Reliable);
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
    std::vector<std::string> release;
    std::string renewed;
    std::vector<RemoteNode*> remotes;
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
                    release.push_back(c.text);
                }
            }
            remotes = remotes_;
        }
    }
    if (!renewed.empty()) return sendAuth(peer, renewed);
    for (const std::string& t : release) {
        if (send_) send_(peer, t, Delivery::Reliable);
    }
    if (onAuth_) onAuth_(peer, status, level);
    for (RemoteNode* r : remotes) {
        if (r->hasRoute(this, peer)) r->tokenAnswered(status, level);
    }
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
    std::string text;
    text.reserve(static_cast<size_t>(n) + fields.size() + 1);
    text.append(head, static_cast<size_t>(n));
    text.append(fields.data(), fields.size());
    text += '}';
    return sendRequest(to, id, text, std::move(done), timeoutMs, owner);
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
                                  std::move(onReply), now, nullptr, std::string(), false, false});
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
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < calls_.size();) {
        if (calls_[i].owner == owner) {
            calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
}

void DatagramEndpoint::addRemote(RemoteNode* remote) {
    MutexGuard guard(mutex_);
    for (RemoteNode* r : remotes_) {
        if (r == remote) return;
    }
    remotes_.push_back(remote);
}

void DatagramEndpoint::routeAdded(RemoteNode* remote, const PeerAddress& peer) {
    MutexGuard guard(mutex_);
    for (auto& a : advertised_) {
        if (a.peer != peer) continue;
        remote->setAdvertisedSchema(a.schemaHash);
        remote->setProtocol(a.protocol);
    }
    // What this endpoint knows of the peer holds for the new route too.
    if (Known* k = knownLocked(peer)) {
        remote->setRouteOnline(this, peer, k->online, k->online ? k->heardMs : 0);
    }
}

void DatagramEndpoint::removeRemote(RemoteNode* remote) {
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < remotes_.size(); i++) {
        if (remotes_[i] == remote) {
            remotes_.erase(remotes_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
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

void DatagramEndpoint::forgetPeer(const PeerAddress& peer, PeerLoss reason) {
    {
        MutexGuard guard(mutex_);
        lostPeers_.emplace_back(peer, reason);
    }
    if (onQueued_) onQueued_();
}

void DatagramEndpoint::mountPeers(Object& parent, uint32_t mirrorIntervalMs) {
    MutexGuard guard(mutex_);
    for (auto& m : mounts_) {
        if (m.first == &parent) {
            m.second = mirrorIntervalMs;
            return;
        }
    }
    mounts_.emplace_back(&parent, mirrorIntervalMs);
}

void DatagramEndpoint::peerSeen(const PeerAddress& peer, std::string_view name, uint32_t schemaHash,
                                const PeerAddress& device, uint8_t protocol) {
    {
        MutexGuard guard(mutex_);
        for (Seen& s : seenPeers_) {
            if (s.peer == peer) {  // coalesce repeated announcements
                s.name.assign(name.data(), name.size());
                s.schemaHash = schemaHash;
                s.device = device;
                s.protocol = protocol;
                return;
            }
        }
        seenPeers_.push_back(Seen{peer, std::string(name), schemaHash, device, protocol});
    }
    if (onQueued_) onQueued_();
}

void DatagramEndpoint::markOnline(const PeerAddress& peer, bool online) {
    uint32_t now = clock_ ? clock_() : 0;
    MutexGuard guard(mutex_);
    if (online) learnLocked(peer, now);
    for (RemoteNode* r : remotes_) r->setRouteOnline(this, peer, online, now);
}

namespace {

// A node name from an advertised name: characters names can't hold become
// '-', and leading or trailing dashes go.
std::string nodeName(std::string_view advertised) {
    std::string n;
    for (char c : advertised) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-' || c == '.';
        n += ok ? c : '-';
    }
    while (!n.empty() && n.front() == '-') n.erase(n.begin());
    while (!n.empty() && n.back() == '-') n.pop_back();
    return n;
}

}  // namespace

void DatagramEndpoint::applySeen(const Seen& seen) {
    std::vector<std::pair<Object*, uint32_t>> mounts;
    // The device: a NowTP address is its MAC; other links advertise it.
    const PeerAddress& device = seen.device.length || kind_ != TransportKind::NowTP ? seen.device : seen.peer;
    std::vector<RemoteNode*> routed;  // nodes with a route through the peer
    {
        MutexGuard guard(mutex_);
        uint32_t now = clock_ ? clock_() : 0;
        Known& k = learnLocked(seen.peer, now);
        if (device.length) k.device = device;
        for (RemoteNode* r : remotes_) {
            if (!r->hasRoute(this, seen.peer)) continue;
            r->setRouteOnline(this, seen.peer, true, now);
            r->setAdvertisedSchema(seen.schemaHash);
            r->setProtocol(seen.protocol);
            routed.push_back(r);
        }
        bool found = false;
        for (auto& a : advertised_) {
            if (a.peer == seen.peer) {
                a.schemaHash = seen.schemaHash;
                a.protocol = seen.protocol;
                found = true;
            }
        }
        if (!found) advertised_.push_back(Advertised{seen.peer, seen.schemaHash, seen.protocol});
        mounts = mounts_;
    }
    if (!api_) return;
    MutexGuard apiGuard(api_->mutex());
    // A node declared with this address learns the device it reaches.
    if (device.length) {
        for (RemoteNode* r : routed) {
            if (!r->device().length) r->setDevice(device);
        }
    }
    for (auto& m : mounts) mount(*m.first, m.second, seen, device);
}

// Under the API lock.
void DatagramEndpoint::mount(Object& parentRef, uint32_t mirrorMs, const Seen& seen, const PeerAddress& device) {
    Object* parent = &parentRef;
    // A parent mounts each device once: the first transport to announce it
    // mounts it, and the others add their routes to that node.
    for (const Node* c = parent->first(); c; c = c->next()) {
        if (c->type() != NodeType::Remote) continue;
        auto* r = const_cast<RemoteNode*>(static_cast<const RemoteNode*>(c));
        if (r->hasRoute(this, seen.peer)) return;
        if (device.length && r->device() == device) {
            r->addRoute(*this, seen.peer);
            r->setAdvertisedSchema(seen.schemaHash);
            r->setProtocol(seen.protocol);
            return;
        }
    }
    // Named after the peer; when that is taken or empty, the last three bytes
    // of its device's address (or its own) are added.
    std::string name = nodeName(seen.name);
    if (name.empty() || parent->child(name)) {
        char suffix[8] = "";
        const uint8_t* b = device.length ? device.bytes : seen.peer.bytes;
        uint8_t n = device.length ? device.length : seen.peer.length;
        if (n >= 3) snprintf(suffix, sizeof(suffix), "%02x%02x%02x", b[n - 3], b[n - 2], b[n - 1]);
        name = name.empty() ? std::string(suffix) : name + "-" + suffix;
    }
    if (name.empty() || parent->child(name)) return;
    RemoteNode& node = parent->add(new RemoteNode(std::move(name), *this, seen.peer, "/"));
    node.setDevice(device);
    if (mirrorMs) node.mirror(mirrorMs);
    node.changed();  // subscribers of the parent see the new key
}

void DatagramEndpoint::expire(uint32_t now, bool all, const PeerAddress* peer) {
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
    for (auto& call : expired) {
        if (call.done) call.done(Status::Timeout, JsonVariantConst());
        if (call.each) call.each(PeerAddress(), Status::Timeout, JsonVariantConst());
    }
}

DatagramEndpoint::Stats DatagramEndpoint::stats() const {
    MutexGuard guard(mutex_);
    return stats_;
}

size_t DatagramEndpoint::pendingCalls() const {
    MutexGuard guard(mutex_);
    return calls_.size();
}

}  // namespace tesser

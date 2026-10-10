#include "tesser/datagram.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/api.h"
#include "tesser/call.h"
#include "tesser/client.h"
#include "tesser/envelope.h"
#include "tesser/json_writer.h"
#include "tesser/remote.h"

namespace tesser {

std::string buildRequestEnvelope(uint32_t id, Op op, std::string_view path, const Query& query,
                                 std::string_view bodyJson) {
    std::string out;
    StringSink sink(out);
    JsonWriter w(sink);
    w.beginObject();
    w.key("id");
    w.uinteger(id);
    w.key("op");
    w.string(toString(op));
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
        w.string(query.view == View::Schema ? "schema" : "hash");
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
        if (!query.events) {
            w.key("events");
            w.boolean(false);
        }
        if (!query.snapshot) {
            w.key("snapshot");
            w.boolean(false);
        }
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
    int n = snprintf(meta, sizeof(meta), "{\"tesser\":%u,\"schema\":\"%08lx\"}", unsigned(port),
                     static_cast<unsigned long>(schemaHash));
    return std::string(meta, n > 0 ? static_cast<size_t>(n) : 0);
}

bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash) {
    JsonDocument doc;
    if (deserializeJson(doc, metadata.data(), metadata.size())) return false;
    if (!doc["tesser"].is<uint8_t>() || !doc["schema"].is<const char*>()) return false;
    port = doc["tesser"].as<uint8_t>();
    schemaHash = static_cast<uint32_t>(strtoul(doc["schema"].as<const char*>(), nullptr, 16));
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
        for (RemoteNode* r : remotes_) r->detach();
        remotes_.clear();
    }
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

void DatagramEndpoint::dropPeer(const PeerAddress& peer) {
    expire(0, true, &peer);
    markOnline(peer, false);
    MutexGuard guard(subscribersMutex_);
    for (size_t i = 0; i < subscribers_.size(); i++) {
        if (subscribers_[i]->peer != peer) continue;
        if (api_) api_->dropSubscriber(subscribers_[i]);
        delete subscribers_[i];
        subscribers_.erase(subscribers_.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
}

void DatagramEndpoint::receive(const PeerAddress& from, const char* data, size_t len, bool reliable) {
    bool queued = false;
    {
        MutexGuard guard(mutex_);
        if (queue_.size() < options_.queueLimit) {
            queue_.push_back(Incoming{from, std::string(data, len), reliable});
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
    Incoming msg{from, std::string(data, len), reliable};
    reject(msg, Status::Busy, "request queue full");
}

bool DatagramEndpoint::process() {
    bool worked = false;
    std::vector<PeerAddress> lost;
    std::vector<Seen> seen;
    {
        MutexGuard guard(mutex_);
        lost.swap(lostPeers_);
        seen.swap(seenPeers_);
    }
    for (const PeerAddress& p : lost) {
        dropPeer(p);
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
    for (RemoteNode* r : remotes) r->tick(now);
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

void DatagramEndpoint::reject(const Incoming& msg, Status status, const char* message) {
    // Only requests get an error reply; responses and notifications are dropped.
    JsonDocument filter;
    filter["id"] = true;
    filter["op"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, msg.text, DeserializationOption::Filter(filter),
                        DeserializationOption::NestingLimit(kWrittenNesting))) {
        return;
    }
    if (!doc["op"].is<const char*>()) return;
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
    EnvelopeReply reply(id, 0, [send, to](const std::string& m) {
        if (send) send(to, m, Delivery::Reliable);
    });
    writeError(reply, status, std::string_view(), message);
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

void DatagramEndpoint::handle(Incoming& msg) {
    // Responses and notifications nest as deep as the peer wrote them;
    // requests are held to the same limit as on other transports below.
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg.text.data(), msg.text.size(),
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
        ResponseHandler done;
        {
            MutexGuard guard(mutex_);
            stats_.responses++;
            for (size_t i = 0; i < calls_.size(); i++) {
                if (calls_[i].id == id && calls_[i].to == msg.from) {
                    done = std::move(calls_[i].done);
                    calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
            if (!done) stats_.unmatched++;
        }
        if (done) {
            Status s = Status::Internal;
            parseStatus(env["status"].as<const char*>(), s);
            done(s, env["body"]);
        }
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
            if (r->handleNotification(msg.from, env)) break;
        }
        std::vector<std::shared_ptr<ClientInbox>> inboxes;
        {
            MutexGuard guard(mutex_);
            for (auto& c : clients_) {
                if (c.first == msg.from) inboxes.push_back(c.second);
            }
        }
        for (auto& inbox : inboxes) inbox->notification(msg.text);
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
    req.client.authenticated = trust_ && trust_(msg.from);
    EnvelopeId id;
    const char* message = nullptr;
    Status s = requestFromEnvelope(env, req, id, message);
    if (s == Status::Ok && nesting(env) > envelopeNesting(api_->config().maxDepth)) {
        s = Status::BadRequest;
        message = "envelope nested too deeply";
    }
    PeerAddress to = msg.from;
    SendFn send = send_;
    EnvelopeReply reply(id, api_->config().maxResponse, [send, to](const std::string& m) {
        if (send) send(to, m, Delivery::Reliable);
    });
    if (s != Status::Ok) {
        writeError(reply, s, std::string_view(), message);
        return;
    }
    if ((req.op == Op::Set || req.op == Op::Subscribe) && options_.requireReliableSet && !msg.reliable) {
        writeError(reply, Status::NotAllowed, req.path, "set and sub requests must be sent reliably");
        return;
    }
    if (req.op == Op::Subscribe || req.op == Op::Unsubscribe) {
        req.subscriber = subscriberFor(msg.from, req.op == Op::Subscribe);
    }
    api_->handle(req, reply);
}

bool DatagramEndpoint::sendRequest(const PeerAddress& to, uint32_t id, const std::string& envelope,
                                   ResponseHandler done, uint32_t timeoutMs, const void* owner) {
    uint32_t now = clock_ ? clock_() : 0;
    {
        MutexGuard guard(mutex_);
        if (calls_.size() >= options_.maxCalls) return false;
        calls_.push_back(
            CallSlot{id, to, now + (timeoutMs ? timeoutMs : options_.timeoutMs), std::move(done), owner});
    }
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
    remotes_.push_back(remote);
    for (auto& a : advertised_) {
        if (a.first == remote->peer()) remote->setAdvertisedSchema(a.second);
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

void DatagramEndpoint::forgetPeer(const PeerAddress& peer) {
    {
        MutexGuard guard(mutex_);
        lostPeers_.push_back(peer);
    }
    if (onQueued_) onQueued_();
}

void DatagramEndpoint::mountPeers(Object& parent, uint32_t mirrorIntervalMs) {
    MutexGuard guard(mutex_);
    mountParent_ = &parent;
    mountMirrorMs_ = mirrorIntervalMs;
}

void DatagramEndpoint::peerSeen(const PeerAddress& peer, std::string_view name, uint32_t schemaHash) {
    {
        MutexGuard guard(mutex_);
        for (Seen& s : seenPeers_) {
            if (s.peer == peer) {  // coalesce repeated announcements
                s.name.assign(name.data(), name.size());
                s.schemaHash = schemaHash;
                return;
            }
        }
        seenPeers_.push_back(Seen{peer, std::string(name), schemaHash});
    }
    if (onQueued_) onQueued_();
}

void DatagramEndpoint::markOnline(const PeerAddress& peer, bool online) {
    MutexGuard guard(mutex_);
    for (RemoteNode* r : remotes_) {
        if (r->peer() == peer) r->setOnline(online);
    }
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
    Object* parent;
    uint32_t mirrorMs;
    {
        MutexGuard guard(mutex_);
        for (RemoteNode* r : remotes_) {
            if (r->peer() != seen.peer) continue;
            r->setOnline(true);
            r->setAdvertisedSchema(seen.schemaHash);
        }
        bool found = false;
        for (auto& a : advertised_) {
            if (a.first == seen.peer) {
                a.second = seen.schemaHash;
                found = true;
            }
        }
        if (!found) advertised_.emplace_back(seen.peer, seen.schemaHash);
        parent = mountParent_;
        mirrorMs = mountMirrorMs_;
    }
    if (!parent || !api_) return;

    MutexGuard apiGuard(api_->mutex());
    for (const Node* c = parent->first(); c; c = c->next()) {
        if (c->type() == NodeType::Remote && static_cast<const RemoteNode*>(c)->peer() == seen.peer) return;
    }
    // Named after the peer; when that is taken or empty, the last three bytes
    // of its address are added.
    std::string name = nodeName(seen.name);
    if (name.empty() || parent->child(name)) {
        char suffix[8] = "";
        const uint8_t* b = seen.peer.bytes;
        uint8_t n = seen.peer.length;
        if (n >= 3) snprintf(suffix, sizeof(suffix), "%02x%02x%02x", b[n - 3], b[n - 2], b[n - 1]);
        name = name.empty() ? std::string(suffix) : name + "-" + suffix;
    }
    if (name.empty() || parent->child(name)) return;
    RemoteNode& node = parent->add(new RemoteNode(std::move(name), *this, seen.peer, "/"));
    if (mirrorMs) node.mirror(mirrorMs);
    node.changed();  // subscribers of the parent see the new key
}

void DatagramEndpoint::expire(uint32_t now, bool all, const PeerAddress* peer) {
    std::vector<ResponseHandler> expired;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < calls_.size();) {
            bool match = peer ? calls_[i].to == *peer
                              : (all || static_cast<int32_t>(now - calls_[i].deadline) >= 0);
            if (match) {
                expired.push_back(std::move(calls_[i].done));
                calls_.erase(calls_.begin() + static_cast<std::ptrdiff_t>(i));
                stats_.timeouts++;
            } else {
                i++;
            }
        }
    }
    for (auto& done : expired) {
        if (done) done(Status::Timeout, JsonVariantConst());
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

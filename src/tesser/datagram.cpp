#include "tesser/datagram.h"

#include "tesser/api.h"
#include "tesser/call.h"
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
    if (query.view == View::Schema) {
        w.key("view");
        w.string("schema");
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
    {
        MutexGuard guard(mutex_);
        lost.swap(lostPeers_);
    }
    for (const PeerAddress& p : lost) {
        dropPeer(p);
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
    return worked || pendingCalls() != before;
}

void DatagramEndpoint::reject(const Incoming& msg, Status status, const char* message) {
    // Only requests get an error reply; responses and notifications are dropped.
    JsonDocument filter;
    filter["id"] = true;
    filter["op"] = true;
    JsonDocument doc;
    if (deserializeJson(doc, msg.text, DeserializationOption::Filter(filter))) return;
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

void DatagramEndpoint::handle(Incoming& msg) {
    JsonDocument doc;
    uint8_t maxDepth = api_ ? api_->config().maxDepth : 16;
    DeserializationError err = deserializeJson(doc, msg.text.data(), msg.text.size(),
                                               DeserializationOption::NestingLimit(static_cast<uint8_t>(maxDepth + 1)));
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
        // handler still sees everything.
        for (RemoteNode* r : remotes) {
            if (r->handleNotification(msg.from, env)) break;
        }
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

void DatagramEndpoint::forgetPeer(const PeerAddress& peer) {
    {
        MutexGuard guard(mutex_);
        lostPeers_.push_back(peer);
    }
    if (onQueued_) onQueued_();
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

#include "tesser/remote.h"

#include "tesser/api.h"
#include "tesser/call.h"

namespace tesser {

namespace {

// Merges a change notification into a copy (objects merge recursively,
// anything else replaces).
void merge(JsonVariant dst, JsonVariantConst patch) {
    if (patch.is<JsonObjectConst>() && dst.is<JsonObject>()) {
        for (JsonPairConst kv : patch.as<JsonObjectConst>()) {
            JsonVariant child = dst[kv.key().c_str()];
            if (kv.value().is<JsonObjectConst>() && child.is<JsonObject>()) {
                merge(child, kv.value());
            } else {
                dst[kv.key().c_str()].set(kv.value());
            }
        }
        return;
    }
    dst.set(patch);
}

// A copy rendered like the value view: objects below `depth` levels as {}.
void writeLimited(JsonWriter& w, JsonVariantConst v, int depth) {
    if (v.is<JsonObjectConst>()) {
        w.beginObject();
        if (depth > 0) {
            for (JsonPairConst kv : v.as<JsonObjectConst>()) {
                w.key(std::string_view(kv.key().c_str(), kv.key().size()));
                writeLimited(w, kv.value(), depth - 1);
            }
        }
        w.endObject();
    } else if (v.is<JsonArrayConst>()) {
        w.beginArray();
        if (depth > 0) {
            for (JsonVariantConst e : v.as<JsonArrayConst>()) writeLimited(w, e, depth);
        }
        w.endArray();
    } else {
        w.variant(v);
    }
}

bool inList(std::string_view list, std::string_view name) {
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string_view::npos) comma = list.size();
        std::string_view item = list.substr(pos, comma - pos);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (item == name) return true;
        pos = comma + 1;
    }
    return false;
}

bool passes(const Subscription& sub, JsonString key) {
    std::string_view k(key.c_str(), key.size());
    if (!sub.keys.empty() && !inList(sub.keys, k)) return false;
    if (!sub.exclude.empty() && inList(sub.exclude, k)) return false;
    return true;
}

int depthOf(const Subscription& sub) { return sub.depth < 0 ? 255 : sub.depth; }

// A subscription's snapshot, cut from the shared copy: keys and exclude at the
// top level, then depth, as for a get.
void writeSnapshot(JsonWriter& w, JsonVariantConst v, const Subscription& sub) {
    int depth = depthOf(sub);
    if (!v.is<JsonObjectConst>() || (sub.keys.empty() && sub.exclude.empty())) return writeLimited(w, v, depth);
    w.beginObject();
    if (depth > 0) {
        for (JsonPairConst kv : v.as<JsonObjectConst>()) {
            if (!passes(sub, kv.key())) continue;
            w.key(std::string_view(kv.key().c_str(), kv.key().size()));
            writeLimited(w, kv.value(), depth - 1);
        }
    }
    w.endObject();
}

// Change notifications follow the local rule (subscription.cpp): values up
// to `depth` levels down, nothing from objects beyond it.
bool hasChanges(JsonVariantConst v, int depth, const Subscription* top) {
    if (depth <= 0) return false;
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
        if (top && !passes(*top, kv.key())) continue;
        if (!kv.value().is<JsonObjectConst>() || hasChanges(kv.value(), depth - 1, nullptr)) return true;
    }
    return false;
}

void writeChanges(JsonWriter& w, JsonVariantConst v, int depth, const Subscription* top) {
    w.beginObject();
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
        if (top && !passes(*top, kv.key())) continue;
        bool object = kv.value().is<JsonObjectConst>();
        if (object && !hasChanges(kv.value(), depth - 1, nullptr)) continue;
        w.key(std::string_view(kv.key().c_str(), kv.key().size()));
        if (object) {
            writeChanges(w, kv.value(), depth - 1, nullptr);
        } else {
            w.variant(kv.value());
        }
    }
    w.endObject();
}

// Sends one change notification to a forwarded subscriber.
void notifyChange(Subscription& sub, const std::function<void(JsonWriter&)>& body) {
    std::string msg;
    StringSink sink(msg);
    JsonWriter m(sink);
    m.beginObject();
    m.key("op");
    m.string("change");
    m.key("path");
    m.string(sub.path);
    m.key("body");
    body(m);
    if (sub.subscriber->missed.exchange(false)) {
        m.key("overflow");
        m.boolean(true);
    }
    m.endObject();
    // There is nothing to resend from later: "overflow" tells the client to
    // re-read.
    if (!sub.subscriber->notify(msg, Delivery::Reliable)) sub.subscriber->missed = true;
}

// A remote node's reply, written to a local reply: error paths are mapped
// back under the local path of the remote node.
void writeMapped(Reply& reply, Status s, JsonVariantConst body, const std::string& localBase,
                 const std::string& remoteBase) {
    std::string_view localPath = localBase.empty() ? std::string_view("/") : std::string_view(localBase);
    JsonWriter w(reply.begin(s));
    if (s != Status::Ok && body.isNull()) {
        w.beginObject();
        w.key("error");
        w.string(toString(s));
        w.key("path");
        w.string(localPath);
        w.key("message");
        w.string(s == Status::Timeout ? "remote node didn't answer" : "couldn't reach the remote node");
        w.endObject();
    } else if (s != Status::Ok && body.is<JsonObjectConst>()) {
        w.beginObject();
        for (JsonPairConst kv : body.as<JsonObjectConst>()) {
            w.key(std::string_view(kv.key().c_str(), kv.key().size()));
            if (kv.key() == "path" && kv.value().is<const char*>()) {
                std::string_view p(kv.value().as<const char*>());
                if (p.compare(0, remoteBase.size(), remoteBase) == 0) p.remove_prefix(remoteBase.size());
                if (p == "/") p = std::string_view();
                std::string local = localBase + std::string(p);
                w.string(local.empty() ? std::string_view("/") : std::string_view(local));
            } else {
                w.variant(kv.value());
            }
        }
        w.endObject();
    } else {
        w.variant(body);
    }
    detail::finishBody(reply, w, localPath);
}

std::string serialized(JsonVariantConst v) {
    std::string s;
    serializeJson(v, s);
    return s;
}

}  // namespace

RemoteNode::RemoteNode(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer, std::string remotePath)
    : Annotated(name, NodeType::Remote), endpoint_(&endpoint), peer_(peer), remotePath_(std::move(remotePath)) {
    if (remotePath_.empty() || remotePath_.front() != '/') remotePath_.insert(0, "/");
    while (remotePath_.size() > 1 && remotePath_.back() == '/') remotePath_.pop_back();
    endpoint_->addRemote(this);
}

RemoteNode::RemoteNode(std::string name, DatagramEndpoint& endpoint, const PeerAddress& peer, std::string remotePath)
    : RemoteNode(static_cast<const char*>(nullptr), endpoint, peer, std::move(remotePath)) {
    ownedName_ = std::move(name);
    name_ = ownedName_.c_str();
}

void RemoteNode::setOnline(bool online) {
    if (online_.exchange(online) != online) changed();
}

void RemoteNode::setAdvertisedSchema(uint32_t hash) {
    if (advertisedSchema_.exchange(hash) != hash) changed();
}

RemoteNode::~RemoteNode() {
    if (!endpoint_) return;
    endpoint_->removeRemote(this);
    endpoint_->cancelCalls(this);
}

RemoteNode& RemoteNode::mirror(uint32_t intervalMs, uint32_t refreshMs) {
    mirror_ = true;
    mirrorIntervalMs_ = intervalMs;
    refreshMs_ = refreshMs;
    return *this;
}

bool RemoteNode::hasCopy() const {
    MutexGuard guard(mutex_);
    return hasCopy_;
}

size_t RemoteNode::upstreamCount() const {
    MutexGuard guard(mutex_);
    return upstreams_.size();
}

void RemoteNode::writeCopy(JsonWriter& w, int depth) const {
    MutexGuard guard(mutex_);
    if (hasCopy_) {
        writeLimited(w, copy_.as<JsonVariantConst>(), depth);
    } else {
        w.null();
    }
}

std::string RemoteNode::remotePathOf(std::string_view rest) const {
    std::string p = remotePath_ == "/" ? std::string() : remotePath_;
    p.append(rest.data(), rest.size());
    if (p.empty()) p = "/";
    return p;
}

RemoteNode::Upstream* RemoteNode::findUpstream(std::string_view rest) {
    for (auto& u : upstreams_) {
        if (u->rest == rest) return u.get();
    }
    return nullptr;
}

void RemoteNode::tick(uint32_t nowMs) {
    // Forwarded subscriptions: renewed every refreshMs in case the remote
    // node restarted, every 2 s after a failed renewal.
    std::vector<std::pair<std::string, uint32_t>> renew;
    {
        MutexGuard guard(mutex_);
        for (auto& u : upstreams_) {
            if (u->subscribing || !u->ready) continue;
            uint32_t every = u->stale ? 2000 : refreshMs_;
            if (nowMs - u->lastSubscribeMs < every) continue;
            u->subscribing = true;
            u->lastSubscribeMs = nowMs;
            renew.emplace_back(u->rest, u->interval);
        }
    }
    for (auto& r : renew) {
        if (!sendUpstream(r.first, r.second, true)) {
            MutexGuard guard(mutex_);
            if (Upstream* u = findUpstream(r.first)) u->subscribing = false;
        }
    }

    if (!mirror_ || subscribing_ || !endpoint_) return;
    if (subscribedOnce_) {
        // Renew: after refreshMs with a copy, every 2 s while there is none.
        uint32_t every = hasCopy() ? refreshMs_ : 2000;
        if (nowMs - lastSubscribeMs_ < every) return;
    }
    subscribedOnce_ = true;
    lastSubscribeMs_ = nowMs;
    subscribing_ = true;
    // Drop any previous subscription first, so renewals don't pile up.
    endpoint_->request(
        peer_, Op::Unsubscribe, remotePath_, std::string_view(), [](Status, JsonVariantConst) {}, Query(), timeoutMs_,
        this);
    Query q;
    q.interval = mirrorIntervalMs_;
    q.remotes = false;  // never mirror the peer's own mirrors: no loops between gateways
    bool sent = endpoint_->request(
        peer_, Op::Subscribe, remotePath_, std::string_view(),
        [this](Status s, JsonVariantConst body) {
            subscribing_ = false;
            if (s != Status::Ok) return;
            {
                MutexGuard guard(mutex_);
                copy_.set(body);
                hasCopy_ = true;
            }
            changed();
        },
        q, timeoutMs_, this);
    if (!sent) subscribing_ = false;
}

bool RemoteNode::localPath(std::string& out) {
    if (!api_) api_ = Api::owner(*this, localPath_);
    out = localPath_;
    return api_ != nullptr;
}

bool RemoteNode::handleNotification(const PeerAddress& from, JsonObjectConst envelope) {
    if (from != peer_) return false;
    JsonVariantConst p = envelope["path"];
    if (!p.is<const char*>()) return false;
    std::string_view path(p.as<const char*>());
    std::string_view base(remotePath_);
    if (base == "/") base = std::string_view();
    if (path.compare(0, base.size(), base) != 0 || (path.size() > base.size() && path[base.size()] != '/')) {
        return false;
    }
    std::string_view rest = path.substr(base.size());
    if (rest == "/") rest = std::string_view();
    std::string_view op(envelope["op"] | "");
    if (op == "change") {
        if (!mirror_ || !rest.empty()) return relayChange(rest, envelope["body"]);
        {
            MutexGuard guard(mutex_);
            if (hasCopy_) {
                merge(copy_.as<JsonVariant>(), envelope["body"]);
            } else {
                copy_.set(envelope["body"]);
                hasCopy_ = true;
            }
        }
        changed();
        return true;
    }
    if (op == "event") {
        if (!mirror_ && upstreamCount() == 0) return false;
        std::string local;
        if (!localPath(local)) return true;
        local.append(rest.data(), rest.size());
        JsonVariantConst body = envelope["body"];
        api_->emitEvent(local, [body](JsonWriter& w) { w.variant(body); });
        return true;
    }
    return false;
}

void RemoteNode::forward(Api& api, const Request& request, std::string_view rest, std::string_view base,
                         Reply& reply) {
    std::string_view basePath = base.empty() ? std::string_view("/") : base;
    if (!endpoint_) return writeError(reply, Status::Internal, basePath, "no endpoint");
    if (!api.pendingBegin()) return writeError(reply, Status::Busy, basePath, "too many pending calls");
    Reply* detached = reply.detach();
    if (!detached) {
        api.pendingDone();
        return writeError(reply, Status::NotAllowed, basePath, "this transport can't wait for remote nodes");
    }
    std::string remoteBase = remotePath_ == "/" ? std::string() : remotePath_;
    // The local path of this node: error paths in the remote's replies are
    // mapped back under it.
    std::string localBase(base.substr(0, base.size() - rest.size()));
    Api* a = &api;
    auto done = [detached, a, localBase, remoteBase](Status s, JsonVariantConst body) {
        writeMapped(*detached, s, body, localBase, remoteBase);
        detached->release();
        a->pendingDone();
    };
    if (!endpoint_->request(peer_, request.op, remotePathOf(rest), request.body, done, request.query, timeoutMs_,
                            this)) {
        writeError(*detached, Status::Busy, basePath, "couldn't send to the remote node");
        detached->release();
        api.pendingDone();
    }
}

// ---- forwarded subscriptions ------------------------------------------------

void RemoteNode::subscribe(Api& api, const Request& req, std::string_view rest, std::string_view base,
                           Reply& reply) {
    std::string_view basePath = base.empty() ? std::string_view("/") : base;
    if (!req.subscriber) return writeError(reply, Status::NotAllowed, basePath, "this transport can't deliver notifications");
    if (!req.body.isNull()) {
        return writeError(reply, Status::BadRequest, basePath, "subscriptions take keys, exclude and depth, not shapes");
    }
    if (req.query.view != View::Value) return writeError(reply, Status::BadRequest, basePath, "subscriptions are on values");
    if (!endpoint_) return writeError(reply, Status::Internal, basePath, "no endpoint");
    Status allowed = api.subscriptionAllowed(req.subscriber);
    if (allowed != Status::Ok) return writeError(reply, allowed, basePath, "too many subscriptions");

    api_ = &api;
    if (localPath_.empty()) localPath_.assign(base.data(), base.size() - rest.size());

    Subscription sub;
    sub.subscriber = req.subscriber;
    sub.node = this;
    sub.path.assign(base.data(), base.size());
    sub.keys.assign(req.query.keys.data(), req.query.keys.size());
    sub.exclude.assign(req.query.exclude.data(), req.query.exclude.size());
    sub.depth = req.query.depth;
    sub.interval = req.query.interval;
    sub.events = req.query.events;
    sub.remotes = req.query.remotes;
    sub.forwarded = true;
    sub.snapshot = req.query.snapshot;

    bool start = false;
    {
        MutexGuard guard(mutex_);
        Upstream* u = findUpstream(rest);
        if (u && u->ready) {
            // Shared: the snapshot comes from the copy, right away.
            JsonWriter w(reply.begin(Status::Ok));
            if (sub.snapshot) {
                writeSnapshot(w, u->copy.as<JsonVariantConst>(), sub);
            } else {
                w.null();
            }
            detail::finishBody(reply, w, basePath);
            api.addSubscription(std::move(sub));
            return;
        }
        if (!api.pendingBegin()) return writeError(reply, Status::Busy, basePath, "too many pending calls");
        Reply* detached = reply.detach();
        if (!detached) {
            api.pendingDone();
            return writeError(reply, Status::NotAllowed, basePath, "this transport can't wait for remote nodes");
        }
        sub.waiting = detached;
        if (!u) {
            upstreams_.emplace_back(new Upstream());
            u = upstreams_.back().get();
            u->rest.assign(rest.data(), rest.size());
            u->interval = req.query.interval;
        }
        if (!u->subscribing) {
            u->subscribing = true;
            u->lastSubscribeMs = endpoint_->now();
            start = true;
        }
    }
    uint32_t interval = req.query.interval;
    std::string r(rest);
    api.addSubscription(std::move(sub));  // waiting for the upstream snapshot
    if (start && !sendUpstream(r, interval, false)) upstreamReply(r, Status::Busy, JsonVariantConst());
}

bool RemoteNode::sendUpstream(const std::string& rest, uint32_t interval, bool renew) {
    if (!endpoint_) return false;
    std::string path = remotePathOf(rest);
    if (renew) {
        endpoint_->request(
            peer_, Op::Unsubscribe, path, std::string_view(), [](Status, JsonVariantConst) {}, Query(), timeoutMs_,
            this);
    }
    Query q;
    q.interval = interval;
    return endpoint_->request(
        peer_, Op::Subscribe, path, std::string_view(),
        [this, rest](Status s, JsonVariantConst body) { upstreamReply(rest, s, body); }, q, timeoutMs_, this);
}

void RemoteNode::upstreamReply(const std::string& rest, Status s, JsonVariantConst body) {
    Api* api = api_;
    if (!api) return;
    // Lock order: the API, then this node.
    MutexGuard apiGuard(api->mutex());
    MutexGuard guard(mutex_);
    Upstream* u = findUpstream(rest);
    if (!u) {
        // Released while subscribing: drop the subscription it made.
        if (s == Status::Ok && endpoint_) {
            endpoint_->request(
                peer_, Op::Unsubscribe, remotePathOf(rest), std::string_view(), [](Status, JsonVariantConst) {},
                Query(), timeoutMs_, this);
        }
        return;
    }
    u->subscribing = false;
    bool wasReady = u->ready;
    bool changedCopy = false;
    if (s == Status::Ok) {
        changedCopy = !wasReady || serialized(u->copy.as<JsonVariantConst>()) != serialized(body);
        u->copy.set(body);
        u->ready = true;
        u->stale = false;
    } else if (wasReady) {
        u->stale = true;  // keep the copy and the subscribers; retry soon
    }

    std::string localPath = localPath_ + rest;
    std::string remoteBase = remotePath_ == "/" ? std::string() : remotePath_;
    std::string localBase = localPath_;
    bool anyLeft = false;
    JsonVariantConst copy = u->copy.as<JsonVariantConst>();
    api->forwardedSubscriptions(this, [&](Subscription& sub) {
        if (sub.path != localPath) return true;
        if (sub.waiting) {
            if (s == Status::Ok) {
                JsonWriter w(sub.waiting->begin(Status::Ok));
                if (sub.snapshot) {
                    writeSnapshot(w, copy, sub);
                } else {
                    w.null();
                }
                detail::finishBody(*sub.waiting, w, sub.path);
            } else {
                writeMapped(*sub.waiting, s, body, localBase, remoteBase);
            }
            sub.waiting->release();
            sub.waiting = nullptr;
            api->pendingDone();
            if (s != Status::Ok) return false;
        } else if (s == Status::Ok && wasReady && changedCopy) {
            // Renewed after the remote changed without telling us (it
            // restarted, or notifications were lost): send the whole state.
            notifyChange(sub, [&](JsonWriter& w) { writeSnapshot(w, copy, sub); });
        }
        anyLeft = true;
        return true;
    });
    if (!anyLeft && !u->ready) {
        for (size_t i = 0; i < upstreams_.size(); i++) {
            if (upstreams_[i].get() == u) {
                upstreams_.erase(upstreams_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
    }
}

bool RemoteNode::relayChange(std::string_view rest, JsonVariantConst patch) {
    Api* api = api_;
    if (!api) return false;
    MutexGuard apiGuard(api->mutex());
    MutexGuard guard(mutex_);
    Upstream* u = findUpstream(rest);
    if (!u || !u->ready) return false;
    merge(u->copy.as<JsonVariant>(), patch);
    std::string localPath = localPath_ + std::string(rest);
    api->forwardedSubscriptions(this, [&](Subscription& sub) {
        if (sub.path != localPath || sub.waiting) return true;
        if (!patch.is<JsonObjectConst>()) {
            notifyChange(sub, [&](JsonWriter& w) { w.variant(patch); });
        } else if (hasChanges(patch, depthOf(sub), &sub)) {
            notifyChange(sub, [&](JsonWriter& w) { writeChanges(w, patch, depthOf(sub), &sub); });
        }
        return true;
    });
    return true;
}

void RemoteNode::released(std::string_view localPath) {
    if (localPath.size() < localPath_.size()) return;
    std::string rest(localPath.substr(localPath_.size()));
    bool found = false;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < upstreams_.size(); i++) {
            if (upstreams_[i]->rest != rest) continue;
            found = upstreams_[i]->ready;  // still subscribing: upstreamReply() unsubscribes
            upstreams_.erase(upstreams_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (found && endpoint_) {
        endpoint_->request(
            peer_, Op::Unsubscribe, remotePathOf(rest), std::string_view(), [](Status, JsonVariantConst) {}, Query(),
            timeoutMs_, this);
    }
}

RemoteNode& Object::remote(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer,
                           const char* remotePath) {
    return add(new RemoteNode(name, endpoint, peer, remotePath ? remotePath : "/"));
}

}  // namespace tesser

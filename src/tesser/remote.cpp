#include "tesser/remote.h"

#include <stdio.h>

#include "tesser/api.h"
#include "tesser/call.h"
#include "tesser/detail/lazy_patch.h"
#include "tesser/file.h"

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
        // A remote list or array: one value, whole at any depth.
        int d = depth > detail::kWholeDepth ? depth : detail::kWholeDepth;
        w.beginArray();
        for (JsonVariantConst e : v.as<JsonArrayConst>()) writeLimited(w, e, d);
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
    flags_ &= static_cast<uint8_t>(~kTemporary);  // named after all
}

void RemoteNode::setOnline(bool online) {
    // Shown in the schema, but not part of its hash (docs/DESIGN.md section
    // 7): subscribers of the parent hear about it as a change.
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
    touch();
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
    sub.changes = req.query.changes;
    sub.events = req.query.events;
    sub.remotes = req.query.remotes;
    sub.forwarded = true;
    sub.snapshot = req.query.snapshot;

    bool start = false;
    uint32_t interval = 0;
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
            // An events-only subscriber doesn't pace the others' changes.
            u->interval = sub.changes ? req.query.interval : 0;
        }
        interval = u->interval;
        if (!u->subscribing) {
            u->subscribing = true;
            u->lastSubscribeMs = endpoint_->now();
            start = true;
        }
    }
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
        } else if (s == Status::Ok && wasReady && changedCopy && sub.changes) {
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
        if (sub.path != localPath || sub.waiting || !sub.changes) return true;
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

// ---- relayed file transfers --------------------------------------------------

namespace {

constexpr int kAttempts = 3;            // a call whose reply timed out is sent twice more
constexpr size_t kFallbackChunk = 192;  // a peer that gave no chunk size

std::string fieldsOf(const std::function<void(JsonWriter&)>& write) {
    std::string text;
    StringSink sink(text);
    JsonWriter w(sink);
    w.beginObject();
    write(w);
    w.endObject();
    return text.substr(1, text.size() - 2);  // the members, without the braces
}

class RemoteFile : public FileRelay, public TransferHolder, public std::enable_shared_from_this<RemoteFile> {
public:
    RemoteFile(Api& api, RemoteNode& node, std::string remotePath, std::string localBase, std::string remoteBase,
               std::string localPath)
        : api_(api),
          node_(node),
          remotePath_(std::move(remotePath)),
          localBase_(std::move(localBase)),
          remoteBase_(std::move(remoteBase)),
          localPath_(std::move(localPath)) {
        // The claim is on the peer, under a key no path can have.
        char hex[24];
        snprintf(hex, sizeof(hex), "@%lx:", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(node.endpoint())));
        key_ = hex;
        for (uint8_t i = 0; i < node.peer().length; i++) {
            snprintf(hex, sizeof(hex), "%02x", node.peer().bytes[i]);
            key_ += hex;
        }
    }
    // Owners close() first; this only covers a relay dropped while claimed.
    ~RemoteFile() override {
        if (claimed_) api_.releaseTransfer(key_);
    }

    bool claim(Reply& reply) override {
        MutexGuard guard(mutex_);
        if (!claimed_ && !api_.claimTransfer(key_, this)) {
            writeError(reply, Status::Busy, localPath_, "another transfer to this node is in progress");
            return false;
        }
        claimed_ = true;
        activeMs_ = millis32();
        return true;
    }

    void probe(Done done) override {
        std::string path = remotePath_;
        call(std::make_shared<const std::string>(fieldsOf([&](JsonWriter& w) {
                 w.key("op");
                 w.string("get");
                 w.key("path");
                 w.string(path);
                 w.key("view");
                 w.string("schema");
                 w.key("depth");
                 w.integer(0);
             })),
             std::move(done));
    }

    void openUpload(uint64_t size, std::string_view filename, Done done) override {
        {
            MutexGuard guard(mutex_);
            size_ = size;
            received_ = 0;
            pending_.clear();
        }
        std::string path = remotePath_;
        auto self = shared_from_this();
        call(std::make_shared<const std::string>(fieldsOf([&](JsonWriter& w) {
                 w.key("op");
                 w.string("upload");
                 w.key("path");
                 w.string(path);
                 w.key("size");
                 w.uinteger(size);
                 if (!filename.empty()) {
                     w.key("filename");
                     w.string(filename);
                 }
             })),
             [self, done](Status s, JsonVariantConst body) {
                 if (s == Status::Ok) {
                     MutexGuard guard(self->mutex_);
                     if (self->size_ == 0) {
                         self->finishLocked();  // nothing to send: that was the upload's reply
                     } else {
                         self->chunk_ = body["chunk"].as<size_t>();
                         self->received_ = body["received"].as<uint64_t>();
                     }
                 } else {
                     self->failed(s);
                 }
                 done(s, body);
             });
    }

    void write(std::vector<uint8_t> data, Done done) override {
        {
            MutexGuard guard(mutex_);
            pending_.insert(pending_.end(), data.begin(), data.end());
        }
        pump(std::move(done));
    }

    void download(uint64_t offset, size_t length, Done done) override {
        std::string path = remotePath_;
        auto self = shared_from_this();
        call(std::make_shared<const std::string>(fieldsOf([&](JsonWriter& w) {
                 w.key("op");
                 w.string("download");
                 w.key("path");
                 w.string(path);
                 if (offset) {
                     w.key("offset");
                     w.uinteger(offset);
                 }
                 if (length) {
                     w.key("length");
                     w.uinteger(length);
                 }
             })),
             [self, done](Status s, JsonVariantConst body) {
                 if (s == Status::Ok) {
                     MutexGuard guard(self->mutex_);
                     if (body["chunk"].is<size_t>()) self->chunk_ = body["chunk"].as<size_t>();
                     // The end: the peer is free again, though the last chunk
                     // can still be asked for again.
                     if (body["end"] == true) {
                         self->finished_ = true;
                         self->release();
                     }
                 } else {
                     self->failed(s);
                 }
                 done(s, body);
             });
    }

    void close() override {
        MutexGuard guard(mutex_);
        closed_ = true;
        release();
    }

    void writeReply(Reply& reply, Status status, JsonVariantConst body) const override {
        writeMapped(reply, status, body, localBase_, remoteBase_);
    }

    uint64_t received() const override {
        MutexGuard guard(mutex_);
        return received_;
    }
    size_t chunk() const override {
        MutexGuard guard(mutex_);
        return chunk_;
    }
    bool busy() const override {
        MutexGuard guard(mutex_);
        return inFlight_;
    }
    bool finished() const override {
        MutexGuard guard(mutex_);
        return finished_;
    }
    bool open() const override {
        MutexGuard guard(mutex_);
        return !closed_;
    }
    bool expired() const override {
        MutexGuard guard(mutex_);
        return expired_;
    }
    uint32_t patienceMs() const override {
        uint32_t t = node_.timeoutMs();
        if (!t) {
            DatagramEndpoint* ep = node_.endpoint();
            t = ep ? ep->timeoutMs() : 3000;
        }
        return t * kAttempts + 1000;
    }

    bool expireIfIdle(uint32_t nowMs, uint32_t idleMs) override {
        // Never waits: the caller holds the API lock, which claim() and
        // release() take after this mutex.
        if (!idleMs || !mutex_.tryLock()) return false;
        bool idle = claimed_ && !inFlight_ && nowMs - activeMs_ >= idleMs;
        if (idle) {
            closed_ = true;
            expired_ = true;
            release();
        }
        mutex_.unlock();
        return idle;
    }

private:
    // Sends one envelope, again on a timeout while the relay is open and
    // the peer online. An offline peer isn't sent to.
    void call(std::shared_ptr<const std::string> fields, Done done, int attempt = 0) {
        if (!node_.online()) {
            JsonDocument error;
            error["error"] = "timeout";
            error["path"] = remoteBase_.empty() ? std::string("/") : remoteBase_;
            error["message"] = "peer offline";
            done(Status::Timeout, error.as<JsonVariantConst>());
            return;
        }
        {
            MutexGuard guard(mutex_);
            inFlight_ = true;
            activeMs_ = millis32();
        }
        DatagramEndpoint* ep = node_.endpoint();
        auto self = shared_from_this();
        bool sent = ep && ep->requestFields(
                              node_.peer(), *fields,
                              [self, fields, done, attempt](Status s, JsonVariantConst body) {
                                  if (s == Status::Timeout && attempt + 1 < kAttempts && self->open() &&
                                      self->node_.online()) {
                                      self->call(fields, done, attempt + 1);
                                      return;
                                  }
                                  self->landed();
                                  done(s, body);
                              },
                              node_.timeoutMs(), &node_);
        if (!sent) {
            landed();
            done(Status::Busy, JsonVariantConst());
        }
    }

    void landed() {
        MutexGuard guard(mutex_);
        inFlight_ = false;
        activeMs_ = millis32();
    }

    // Upload: sends the next part of pending_.
    void pump(Done done) {
        std::shared_ptr<const std::string> fields;
        uint64_t offset = 0;
        size_t part = 0;
        bool closed, idle;
        {
            MutexGuard guard(mutex_);
            closed = closed_;
            idle = pending_.empty();
            if (!closed && !idle) {
                offset = received_;
                part = chunk_ ? chunk_ : kFallbackChunk;
                if (part > pending_.size()) part = pending_.size();
                std::string data = "\"";
                detail::base64Encode(pending_.data(), part, data);
                data += '"';
                fields = std::make_shared<const std::string>(fieldsOf([&](JsonWriter& w) {
                    w.key("op");
                    w.string("upload");
                    w.key("path");
                    w.string(remotePath_);
                    w.key("offset");
                    w.uinteger(offset);
                    w.key("data");
                    w.raw(data);
                }));
            }
        }
        if (closed || idle) {
            done(closed ? Status::Busy : Status::Ok, JsonVariantConst());
            return;
        }
        auto self = shared_from_this();
        call(fields, [self, done, offset, part](Status s, JsonVariantConst body) {
            if (s != Status::Ok) {
                self->failed(s);
                done(s, body);
                return;
            }
            bool more = false;
            bool stuck = false;
            {
                MutexGuard guard(self->mutex_);
                if (offset + part >= self->size_) {
                    // The part that completes the file: that was the upload's reply.
                    self->received_ = self->size_;
                    self->pending_.clear();
                    self->finishLocked();
                } else {
                    uint64_t got = body["received"].as<uint64_t>();
                    if (got <= offset || got > offset + part) {
                        stuck = true;
                    } else {
                        self->pending_.erase(self->pending_.begin(),
                                             self->pending_.begin() + static_cast<std::ptrdiff_t>(got - offset));
                        self->received_ = got;
                        more = !self->pending_.empty();
                    }
                }
            }
            if (stuck) {
                self->close();
                JsonDocument error;
                error["error"] = "internal";
                error["path"] = self->remotePath_;
                error["message"] = "the remote node didn't take the chunk";
                done(Status::Internal, error.as<JsonVariantConst>());
            } else if (more) {
                self->pump(done);
            } else {
                done(Status::Ok, body);
            }
        });
    }

    // An error ends the transfer, except a bad offset, which the client can
    // correct.
    void failed(Status s) {
        if (s != Status::BadRequest) close();
    }

    void finishLocked() {
        finished_ = true;
        closed_ = true;
        release();
    }

    void release() {
        if (claimed_) api_.releaseTransfer(key_);
        claimed_ = false;
    }

    Api& api_;
    RemoteNode& node_;
    const std::string remotePath_, localBase_, remoteBase_, localPath_;
    std::string key_;
    mutable Mutex mutex_;
    bool claimed_ = false;
    bool closed_ = false;
    bool finished_ = false;
    bool inFlight_ = false;
    bool expired_ = false;
    uint32_t activeMs_ = 0;
    size_t chunk_ = 0;
    uint64_t size_ = 0;
    uint64_t received_ = 0;
    std::vector<uint8_t> pending_;  // upload bytes from received_ on
};

}  // namespace

std::shared_ptr<FileRelay> RemoteNode::relayFile(Api& api, std::string_view rest, std::string_view base) {
    if (!endpoint_) return nullptr;
    std::string remoteBase = remotePath_ == "/" ? std::string() : remotePath_;
    std::string localBase(base.substr(0, base.size() - rest.size()));
    return std::make_shared<RemoteFile>(api, *this, remotePathOf(rest), std::move(localBase), std::move(remoteBase),
                                        std::string(base));
}

RemoteNode& Object::remote(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer,
                           const char* remotePath) {
    return add(new RemoteNode(name, endpoint, peer, remotePath ? remotePath : "/"));
}

}  // namespace tesser

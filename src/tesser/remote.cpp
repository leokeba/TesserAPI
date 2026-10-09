#include "tesser/remote.h"

#include "tesser/api.h"
#include "tesser/call.h"

namespace tesser {

namespace {

// Merges a change notification into the mirrored copy (objects merge
// recursively, anything else replaces).
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

}  // namespace

RemoteNode::RemoteNode(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer, std::string remotePath)
    : Node(name, NodeType::Remote), endpoint_(&endpoint), peer_(peer), remotePath_(std::move(remotePath)) {
    if (remotePath_.empty() || remotePath_.front() != '/') remotePath_.insert(0, "/");
    while (remotePath_.size() > 1 && remotePath_.back() == '/') remotePath_.pop_back();
    endpoint_->addRemote(this);
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

namespace {
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
}  // namespace

void RemoteNode::writeCopy(JsonWriter& w, int depth) const {
    MutexGuard guard(mutex_);
    if (hasCopy_) {
        writeLimited(w, copy_.as<JsonVariantConst>(), depth);
    } else {
        w.null();
    }
}

void RemoteNode::tick(uint32_t nowMs) {
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
    if (from != peer_ || !mirror_) return false;
    JsonVariantConst p = envelope["path"];
    if (!p.is<const char*>()) return false;
    std::string_view path(p.as<const char*>());
    std::string_view base(remotePath_);
    if (base == "/") base = std::string_view();
    if (path.compare(0, base.size(), base) != 0 || (path.size() > base.size() && path[base.size()] != '/')) {
        return false;
    }
    std::string_view rest = path.substr(base.size());
    std::string_view op(envelope["op"] | "");
    if (op == "change") {
        if (!rest.empty() && rest != "/") return false;  // another subscription to this peer
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
    std::string remotePath = remoteBase + std::string(rest);
    if (remotePath.empty()) remotePath = "/";
    // The local path of this node: error paths in the remote's replies are
    // mapped back under it.
    std::string localBase(base.substr(0, base.size() - rest.size()));
    Api* a = &api;
    auto done = [detached, a, localBase, remoteBase](Status s, JsonVariantConst body) {
        JsonWriter w(detached->begin(s));
        if (s == Status::Timeout && body.isNull()) {
            w.beginObject();
            w.key("error");
            w.string("timeout");
            w.key("path");
            w.string(localBase.empty() ? std::string_view("/") : std::string_view(localBase));
            w.key("message");
            w.string("remote node didn't answer");
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
        detail::finishBody(*detached, w, localBase);
        detached->release();
        a->pendingDone();
    };
    if (!endpoint_->request(peer_, request.op, remotePath, request.body, done, request.query, timeoutMs_, this)) {
        writeError(*detached, Status::Busy, basePath, "couldn't send to the remote node");
        detached->release();
        api.pendingDone();
    }
}

RemoteNode& Object::remote(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer,
                           const char* remotePath) {
    return add(new RemoteNode(name, endpoint, peer, remotePath ? remotePath : "/"));
}

}  // namespace tesser

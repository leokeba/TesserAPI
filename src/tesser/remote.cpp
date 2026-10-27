#include "tesser/remote.h"

#include <stdio.h>

#include "tesser/api.h"
#include "tesser/budget.h"
#include "tesser/call.h"
#include "tesser/detail/remote_json.h"
#include "tesser/envelope.h"

namespace tesser {

namespace detail {

void mergeJson(JsonVariant dst, JsonVariantConst patch) {
    if (patch.is<JsonObjectConst>() && dst.is<JsonObject>()) {
        for (JsonPairConst kv : patch.as<JsonObjectConst>()) {
            JsonVariant child = dst[kv.key().c_str()];
            if (kv.value().is<JsonObjectConst>() && child.is<JsonObject>()) {
                mergeJson(child, kv.value());
            } else {
                dst[kv.key().c_str()].set(kv.value());
            }
        }
        return;
    }
    dst.set(patch);
}

void writeMapped(Reply& reply, Status s, JsonVariantConst body, const std::string& localBase,
                 const std::string& remoteBase) {
    std::string_view localPath = localBase.empty() ? std::string_view("/") : std::string_view(localBase);
    // A node that predates ceilings refuses an op like "get@user"
    // (docs/DESIGN.md section 13.1): this client may not reach it.
    std::string_view message(body["message"] | "");
    if (s == Status::BadRequest && message.compare(0, 11, "op must be ") == 0) {
        return writeError(reply, Status::Unauthorized, Reason::CeilingUnsupported, localPath,
                          "the remote node can't limit a client's access: only admins reach it through a gateway");
    }
    JsonWriter w(reply.begin(s));
    if (s != Status::Ok && body.isNull()) {
        w.beginObject();
        w.key("error");
        w.string(toString(s));
        if (s == Status::Timeout || s == Status::Busy) {
            w.key("reason");
            w.string(toString(s == Status::Timeout ? Reason::NoAnswer : Reason::Unreachable));
        }
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

}  // namespace detail

namespace {

Mutex& sharedMutex() { return detail::remoteSharedMutex(); }
RemoteNode::StateHandler& sharedStateHandler() {
    static RemoteNode::StateHandler fn;
    return fn;
}
RemoteNode::AccessHandler& sharedAccessHandler() {
    static RemoteNode::AccessHandler fn;
    return fn;
}

// A forwarded request's detached reply and pending slot, shared by the
// call's two handlers: answered once, by whichever runs. If the endpoint
// drops the call without running either (the node or the endpoint
// destroyed), the last copy to go answers it, so the client hears and the
// slot comes back.
class Forwarded {
public:
    Forwarded(Reply* detached, Api* api, std::string path) : detached_(detached), api_(api), path_(std::move(path)) {}
    ~Forwarded() { fail(Status::Internal, Reason::None, "the call to the remote node was dropped"); }
    Forwarded(const Forwarded&) = delete;
    Forwarded& operator=(const Forwarded&) = delete;

    // The reply, once: null if it went already. finish() after writing.
    Reply* take() { return answered_.exchange(true) ? nullptr : detached_; }
    void finish() {
        detached_->release();
        api_->pendingDone();
    }
    void fail(Status s, Reason reason, const char* message) {
        Reply* r = take();
        if (!r) return;
        writeError(*r, s, reason, path_, message);
        finish();
    }

private:
    Reply* detached_;
    Api* api_;
    std::string path_;
    std::atomic<bool> answered_{false};
};

}  // namespace

RemoteNode::RemoteNode(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer, std::string remotePath)
    : Annotated(name, NodeType::Remote), remotePath_(std::move(remotePath)) {
    if (remotePath_.empty() || remotePath_.front() != '/') remotePath_.insert(0, "/");
    while (remotePath_.size() > 1 && remotePath_.back() == '/') remotePath_.pop_back();
    addRoute(endpoint, peer);
}

RemoteNode::RemoteNode(std::string name, DatagramEndpoint& endpoint, const PeerAddress& peer, std::string remotePath)
    : RemoteNode(static_cast<const char*>(nullptr), endpoint, peer, std::move(remotePath)) {
    ownedName_ = std::move(name);
    name_ = ownedName_.c_str();
    flags_ &= static_cast<uint8_t>(~kTemporary);  // named after all
}

RemoteNode::~RemoteNode() {
    for (DatagramEndpoint* e : routes_.endpoints()) {
        e->removeRemote(this);
        e->cancelCalls(this);
    }
}

// ---- routes -----------------------------------------------------------------

uint32_t RemoteNode::lastHeardMs() const { return routes_.lastHeardMs(); }

bool RemoteNode::onlyThrough(const DatagramEndpoint* endpoint) const { return routes_.onlyThrough(endpoint); }

size_t RemoteNode::routeCount(const DatagramEndpoint* endpoint) const { return routes_.count(endpoint); }

bool RemoteNode::replaceOfflineRoute(DatagramEndpoint& endpoint, const PeerAddress& peer) {
    if (!routes_.replaceOffline(endpoint, peer, endpoint.interfaceOf(peer))) return false;
    endpoint.routeAdded(this, peer);
    std::string token = this->token();
    if (!token.empty()) endpoint.setPeerToken(peer, token);
    routes_.announce();
    return true;
}

void RemoteNode::addRoute(DatagramEndpoint& endpoint, const PeerAddress& peer) {
    bool registered = false;
    if (!routes_.add(endpoint, peer, endpoint.interfaceOf(peer), registered)) return;
    if (!registered) endpoint.addRemote(this);
    endpoint.routeAdded(this, peer);
    std::string token = this->token();
    if (!token.empty()) endpoint.setPeerToken(peer, token);
    routes_.announce();  // mounted, or a route added: tick() tells onStateChange()
}

void RemoteNode::setToken(std::string_view token) {
    for (const Route& r : routes_.setToken(token)) r.endpoint->setPeerToken(r.peer, token);
}

std::string RemoteNode::token() const { return routes_.token(); }

Access RemoteNode::access() const {
    Route r;
    return route(r) ? r.endpoint->grantedAccess(r.peer) : Access::Public;
}

// ---- RemoteHooks -------------------------------------------------------------

RemoteHooks::Id RemoteHooks::onStateChange(StateHandler fn) {
    MutexGuard guard(mutex_);
    Id id = next_++;
    state_.emplace_back(id, std::move(fn));
    return id;
}

RemoteHooks::Id RemoteHooks::onAccess(AccessHandler fn) {
    MutexGuard guard(mutex_);
    Id id = next_++;
    access_.emplace_back(id, std::move(fn));
    return id;
}

void RemoteHooks::remove(Id id) {
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < state_.size(); i++) {
        if (state_[i].first == id) state_.erase(state_.begin() + static_cast<std::ptrdiff_t>(i--));
    }
    for (size_t i = 0; i < access_.size(); i++) {
        if (access_[i].first == id) access_.erase(access_.begin() + static_cast<std::ptrdiff_t>(i--));
    }
}

void RemoteHooks::setRouteOrder(std::vector<RouteKind> order) {
    MutexGuard guard(mutex_);
    order_ = std::move(order);
}

std::vector<RouteKind> RemoteHooks::routeOrder() const {
    MutexGuard guard(mutex_);
    return order_;
}

void RemoteHooks::stateChanged(RemoteNode& node) const {
    std::vector<StateHandler> fns;
    {
        MutexGuard guard(mutex_);
        for (const auto& s : state_) fns.push_back(s.second);
    }
    for (const StateHandler& fn : fns) {
        if (fn) fn(node);
    }
}

void RemoteHooks::accessAnswered(RemoteNode& node, Status status, Access level) const {
    std::vector<AccessHandler> fns;
    {
        MutexGuard guard(mutex_);
        for (const auto& a : access_) fns.push_back(a.second);
    }
    for (const AccessHandler& fn : fns) {
        if (fn) fn(node, status, level);
    }
}

Api* RemoteNode::api() const { return api_.load(); }

// ---- RemoteNode ----------------------------------------------------------------

void RemoteNode::onAccess(AccessHandler fn) {
    MutexGuard guard(sharedMutex());
    sharedAccessHandler() = std::move(fn);
}

void RemoteNode::tokenAnswered(Status status, Access level) {
    AccessHandler fn;
    {
        MutexGuard guard(sharedMutex());
        fn = sharedAccessHandler();
    }
    if (fn) fn(*this, status, level);
    if (Api* api = api_.load()) api->remoteHooks().accessAnswered(*this, status, level);
}

bool RemoteNode::hasRoute(const DatagramEndpoint* endpoint, const PeerAddress& peer) const {
    return routes_.has(endpoint, peer);
}

bool RemoteNode::routeUp(const DatagramEndpoint* endpoint, const PeerAddress& peer) const {
    return routes_.up(endpoint, peer);
}

std::vector<RemoteNode::Route> RemoteNode::routes() const { return routes_.list(api_.load()); }

bool RemoteNode::route(Route& out) const { return routes_.active(out, api_.load()); }

DatagramEndpoint* RemoteNode::endpoint() const {
    Route r;
    return route(r) ? r.endpoint : nullptr;
}

PeerAddress RemoteNode::peer() const {
    Route r;
    return route(r) ? r.peer : PeerAddress();
}

RemoteNode& RemoteNode::routeOrder(std::vector<RouteKind> order) {
    routes_.setOrder(std::move(order));
    return *this;
}

std::vector<RouteKind> RemoteNode::routeOrder() const { return routes_.order(); }

void RemoteNode::setDefaultRouteOrder(std::vector<RouteKind> order) {
    detail::RouteTable::setDefaultOrder(std::move(order));
}

std::vector<RouteKind> RemoteNode::defaultRouteOrder() { return detail::RouteTable::defaultOrder(); }

void RemoteNode::onStateChange(StateHandler fn) {
    MutexGuard guard(sharedMutex());
    sharedStateHandler() = std::move(fn);
}

RemoteNode* RemoteNode::byDevice(const Object& parent, const PeerAddress& device) {
    if (!device.length) return nullptr;
    for (const Node* c = parent.first(); c; c = c->next()) {
        if (c->type() != NodeType::Remote) continue;
        auto* r = const_cast<RemoteNode*>(static_cast<const RemoteNode*>(c));
        if (r->device() == device) return r;
    }
    return nullptr;
}

PeerAddress RemoteNode::device() const { return routes_.device(); }

void RemoteNode::setDevice(const PeerAddress& device) {
    if (routes_.setDevice(device)) changed();  // shown in the schema
}

// The node's online state is shown in the schema, but not part of its hash
// (docs/DESIGN.md section 7): subscribers of the parent hear about it as a
// change.
void RemoteNode::setOnline(bool online) {
    if (routes_.setAll(online)) changed();
}

void RemoteNode::setRouteOnline(const DatagramEndpoint* endpoint, const PeerAddress& peer, bool online,
                                uint32_t nowMs) {
    // Back online: the interface may have changed (addresses come and go).
    bool back = online && routes_.down(endpoint, peer);
    NetInterface iface = back ? endpoint->interfaceOf(peer) : NetInterface::Any;
    if (routes_.setOnline(endpoint, peer, online, nowMs, back, iface)) changed();
}

void RemoteNode::routeRoundTrip(const DatagramEndpoint* endpoint, const PeerAddress& peer, uint32_t rttMs) {
    routes_.roundTrip(endpoint, peer, rttMs);
}

void RemoteNode::setProtocol(uint8_t version) {
    if (version && protocol_.exchange(version) != version) changed();
}

bool RemoteNode::knownFile(std::string_view remotePath, bool& isFile) const {
    uint32_t hash = advertisedSchema_.load();
    if (!hash) return false;
    MutexGuard guard(pathsMutex_);
    for (const KnownPath& k : knownPaths_) {
        if (k.schemaHash == hash && k.path == remotePath) {
            isFile = k.file;
            return true;
        }
    }
    return false;
}

void RemoteNode::learnedFile(std::string_view remotePath, bool isFile) {
    uint32_t hash = advertisedSchema_.load();
    if (!hash) return;  // nothing would tell it went stale
    MutexGuard guard(pathsMutex_);
    for (KnownPath& k : knownPaths_) {
        if (k.path == remotePath) {
            k.schemaHash = hash;
            k.file = isFile;
            return;
        }
    }
    if (knownPaths_.size() >= kKnownPaths) knownPaths_.erase(knownPaths_.begin());
    knownPaths_.push_back(KnownPath{std::string(remotePath), hash, isFile});
}

void RemoteNode::setAdvertisedSchema(uint32_t hash) {
    if (advertisedSchema_.exchange(hash) != hash) changed();
}

void RemoteNode::detach(const DatagramEndpoint* endpoint) {
    if (routes_.detach(endpoint)) changed();
}

RemoteNode& RemoteNode::mirror(uint32_t intervalMs, uint32_t refreshMs, Access level) {
    upstream_.mirror(intervalMs, refreshMs, level);
    touch();
    return *this;
}
bool RemoteNode::hasCopy() const { return upstream_.hasCopy(); }
size_t RemoteNode::upstreamCount() const { return upstream_.count(); }
void RemoteNode::writeCopy(JsonWriter& w, int depth) const { upstream_.writeCopy(w, depth); }
std::string RemoteNode::remotePathOf(std::string_view rest) const {
    std::string p = remotePath_ == "/" ? std::string() : remotePath_;
    p.append(rest.data(), rest.size());
    if (p.empty()) p = "/";
    return p;
}

void RemoteNode::tick(const DatagramEndpoint& by, uint32_t nowMs) {
    // Its Api, for its hooks and route order: found once, walking the
    // trees, at most once a second while it isn't in one.
    if (!api_.load() && (!apiLookupMs_ || nowMs - apiLookupMs_ >= 1000)) {
        apiLookupMs_ = nowMs ? nowMs : 1;
        std::string path;
        localPath(path);
    }
    // Every endpoint of the node ticks it; the one in use does the work.
    Route cur;
    if (!route(cur) || cur.endpoint != &by) return;
    detail::RouteTable::Observed seen = routes_.observe(cur, online());
    if (seen.switched) {
        changed();  // the schema shows the route
        upstream_.rerouted(*this, seen.from, nowMs);
    }
    if (seen.stateChanged) {
        StateHandler fn;
        {
            MutexGuard guard(sharedMutex());
            fn = sharedStateHandler();
        }
        if (fn) fn(*this);
        if (Api* api = api_.load()) api->remoteHooks().stateChanged(*this);
    }

    // Offline routes: does the device answer? Any reply puts the route back.
    for (const Route& r : routes_.dueProbes(nowMs)) {
        Query q;
        q.depth = 0;
        r.endpoint->request(
            r.peer, Op::Get, remotePath_, std::string_view(), [](Status, JsonVariantConst) {}, q, timeoutMs_, this);
    }

    upstream_.renew(*this, nowMs);
    // Streamed sets that couldn't be sent (no call slot, a busy peer).
    pump();
    upstream_.refreshMirror(*this, cur, nowMs);
}

bool RemoteNode::localPath(std::string& out) {
    if (!api_.load()) api_ = Api::owner(*this, localPath_);
    out = localPath_;
    return api_ != nullptr;
}

bool RemoteNode::handleNotification(const DatagramEndpoint& by, const PeerAddress& from, JsonObjectConst envelope) {
    // Only the route in use: a device reached two ways may still notify the
    // other for a moment after a switch.
    Route cur;
    if (!route(cur) || cur.endpoint != &by || cur.peer != from) return false;
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
    // The level of the subscription it comes from ("as"; none: admin).
    Access level = Access::Admin;
    if (envelope["as"].is<const char*>() && !parseAccess(envelope["as"].as<const char*>(), level)) return false;
    std::string_view op(envelope["op"] | "");
    return upstream_.notified(*this, rest, level, op, envelope);
}

void RemoteNode::forward(Api& api, const Request& request, std::string_view rest, std::string_view base,
                         Reply& reply) {
    std::string_view basePath = base.empty() ? std::string_view("/") : base;
    Route r;
    if (!route(r)) return writeError(reply, Status::Internal, basePath, "no endpoint");
    // The local path of this node: error paths in the remote's replies are
    // mapped back under it.
    std::string localBase(base.substr(0, base.size() - rest.size()));
    // Known to be unreachable: fail now rather than hold a pending slot
    // until the timeout, as fan-out and file relays do.
    if (!online()) {
        return writeError(reply, Status::Timeout, Reason::PeerOffline, localBase.empty() ? std::string_view("/") : localBase,
                          "peer offline");
    }
    // Larger than the peer takes (docs/DESIGN.md section 14.1): too large
    // now, not sent to be lost or refused. The envelope adds its id, op,
    // path and query to the body.
    size_t takes = r.endpoint->peerLimit(r.peer);
    if (takes && !request.body.isNull() && measureJson(request.body) + request.path.size() + 128 > takes) {
        char message[64];
        snprintf(message, sizeof(message), "the remote node takes messages of up to %lu bytes",
                 static_cast<unsigned long>(takes));
        return writeError(reply, Status::TooLarge, basePath, message);
    }
    if (!api.pendingBegin()) return writeError(reply, Status::Busy, Reason::PendingLimit, basePath, "too many pending calls");
    Reply* detached = reply.detach();
    if (!detached) {
        api.pendingDone();
        return writeError(reply, Status::NotAllowed, Reason::CannotWait, basePath, "this transport can't wait for remote nodes");
    }
    std::string remoteBase = remotePath_ == "/" ? std::string() : remotePath_;
    std::string okPath = localBase.empty() ? std::string("/") : localBase;
    auto call = std::make_shared<Forwarded>(detached, &api, std::string(basePath));
    auto done = [call, localBase, remoteBase](Status s, JsonVariantConst body) {
        Reply* detached = call->take();
        if (!detached) return;
        detail::writeMapped(*detached, s, body, localBase, remoteBase);
        call->finish();
    };
    // An "ok" body goes on as the remote wrote it: parsed and rewritten, a
    // large one would be on the heap twice.
    auto raw = [call, okPath](std::string_view json) {
        Reply* detached = call->take();
        if (!detached) return;
        Sink& out = detached->begin(Status::Ok);
        if (!out.write(json.data(), json.size()) && detached->rollback()) {
            if (detached->outOfMemory()) {
                writeError(*detached, Status::Busy, Reason::LowMemory, okPath, "not enough memory for the response");
            } else {
                writeError(*detached, Status::TooLarge, okPath, "response too large");
            }
        } else {
            detached->end();
        }
        call->finish();
    };
    // No more than the client may (docs/DESIGN.md section 13.1).
    Query q = request.query;
    q.ceiling = request.client.level();
    if (!r.endpoint->requestRaw(r.peer, request.op, remotePathOf(rest), request.body, raw, done, q, timeoutMs_,
                                this)) {
        call->fail(Status::Busy, Reason::Unreachable, "couldn't send to the remote node");
    }
}

// ---- streamed sets -----------------------------------------------------------

Status RemoteNode::stream(std::string_view path, std::string_view bodyJson) {
    if (!online()) return Status::Timeout;
    JsonDocument body;
    if (deserializeJson(body, bodyJson.data(), bodyJson.size(), DeserializationOption::NestingLimit(kWrittenNesting))) {
        return Status::BadRequest;
    }
    Status s = stream_.push(path, std::move(body));
    if (s == Status::Ok) pump();
    return s;
}

void RemoteNode::onStreamError(StreamErrorHandler fn) { stream_.onError(std::move(fn)); }

RemoteNode::StreamStats RemoteNode::streamStats() const { return stream_.stats(); }

size_t RemoteNode::streaming() const { return stream_.size(); }

uint32_t RemoteNode::streamTimeoutMs() const {
    // Longer than the link takes to give up on a message (UDP's sendTimeoutMs
    // is the endpoint's default timeout): a set that times out can't still
    // arrive after the one sent in its place.
    uint32_t t = timeoutMs_;
    if (!t) {
        DatagramEndpoint* ep = endpoint();
        t = ep ? ep->timeoutMs() : 3000;
    }
    return t * 2;
}

// Sends the next streamed set if none is in flight; drops them all if the
// node is offline.
void RemoteNode::pump() {
    Route r;
    bool reachable = route(r) && online();
    std::string path, body;
    if (!stream_.next(reachable, path, body)) return;
    bool sent = r.endpoint->request(
        r.peer, Op::Set, remotePathOf(path), std::string_view(body),
        [this](Status s, JsonVariantConst reply) {
            if (stream_.done(s, reply)) pump();
        },
        Query(), streamTimeoutMs(), this);
    if (!sent) stream_.unsent();  // tick() tries again
}

// ---- forwarded subscriptions ------------------------------------------------

void RemoteNode::subscribe(Api& api, const Request& req, std::string_view rest, std::string_view base,
                           Reply& reply) {
    upstream_.subscribe(*this, api, req, rest, base, reply);
}

void RemoteNode::narrowed(std::string_view localPath, Access level) {
    if (localPath.size() < localPath_.size()) return;
    upstream_.narrowed(*this, std::string(localPath.substr(localPath_.size())), localPath, level);
}

bool RemoteNode::upstreamSpec(std::string_view localPath, std::string& keys, std::string& exclude, int& depth,
                              Access level) const {
    if (localPath.size() < localPath_.size()) return false;
    Spec sent;
    if (!upstream_.sent(localPath.substr(localPath_.size()), level, sent)) return false;
    keys = sent.keys;
    exclude = sent.exclude;
    depth = sent.depth;
    return true;
}

void RemoteNode::released(std::string_view localPath, Access level) {
    if (localPath.size() < localPath_.size()) return;
    upstream_.released(*this, std::string(localPath.substr(localPath_.size())), level);
}

RemoteNode& Object::remote(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer,
                           const char* remotePath) {
    return add(new RemoteNode(name, endpoint, peer, remotePath ? remotePath : "/"));
}

RemoteNode& Object::remote(std::string name, DatagramEndpoint& endpoint, const PeerAddress& peer,
                           const char* remotePath) {
    return add(new RemoteNode(std::move(name), endpoint, peer, remotePath ? remotePath : "/"));
}

}  // namespace tesser

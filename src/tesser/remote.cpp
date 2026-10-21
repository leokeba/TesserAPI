#include "tesser/remote.h"

#include <stdio.h>

#include <algorithm>

#include "tesser/api.h"
#include "tesser/budget.h"
#include "tesser/call.h"
#include "tesser/detail/lazy_patch.h"
#include "tesser/detail/remote_json.h"
#include "tesser/envelope.h"

namespace tesser {

namespace {

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

// ---- upstream specs ------------------------------------------------------------

using Spec = RemoteNode::Spec;

std::vector<std::string> listItems(std::string_view list) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string_view::npos) comma = list.size();
        std::string_view item = list.substr(pos, comma - pos);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (!item.empty()) out.emplace_back(item);
        pos = comma + 1;
    }
    return out;
}

std::string joinSorted(std::vector<std::string> items) {
    std::sort(items.begin(), items.end());
    items.erase(std::unique(items.begin(), items.end()), items.end());
    std::string s;
    for (const std::string& i : items) {
        if (!s.empty()) s += ',';
        s += i;
    }
    return s;
}

Spec specOf(const Subscription& sub) {
    Spec s;
    std::vector<std::string> keys = listItems(sub.keys);
    s.keys = joinSorted(keys);
    if (keys.empty()) s.exclude = joinSorted(listItems(sub.exclude));
    s.depth = sub.depth < 0 ? -1 : sub.depth;
    return s;
}

// Everything any of `specs` needs: the deepest depth; the keys of all when
// all have keys, else every child but those all of them leave out.
Spec unionOf(const std::vector<Spec>& specs) {
    Spec u;
    if (specs.empty()) return u;
    bool unlimited = false, allKeyed = true, anyUnkeyed = false;
    int depth = 0;
    std::vector<std::string> keys, exclude;
    for (const Spec& s : specs) {
        if (s.depth < 0) {
            unlimited = true;
        } else if (s.depth > depth) {
            depth = s.depth;
        }
        std::vector<std::string> k = listItems(s.keys);
        if (!k.empty()) {
            keys.insert(keys.end(), k.begin(), k.end());
            continue;
        }
        allKeyed = false;
        std::vector<std::string> e = listItems(s.exclude);
        if (!anyUnkeyed) {
            exclude = e;
            anyUnkeyed = true;
        } else {
            std::vector<std::string> both;
            for (const std::string& x : exclude) {
                if (std::find(e.begin(), e.end(), x) != e.end()) both.push_back(x);
            }
            exclude = both;
        }
    }
    u.depth = unlimited ? -1 : depth;
    if (allKeyed) {
        u.keys = joinSorted(keys);
    } else {
        std::vector<std::string> left;
        for (const std::string& x : exclude) {
            if (std::find(keys.begin(), keys.end(), x) == keys.end()) left.push_back(x);
        }
        u.exclude = joinSorted(left);
    }
    return u;
}

// `have` holds everything `need` asks for.
bool covers(const Spec& have, const Spec& need) { return unionOf({have, need}) == have; }

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
    StringSink sink(msg, 0, true);
    JsonWriter m(sink);
    m.beginObject();
    m.key("op");
    m.string("change");
    m.key("path");
    m.string(sub.path);
    m.key("body");
    body(m);
    if (sub.ceiling < Access::Admin) {  // made with one: "sub@user"
        m.key("as");
        m.string(toString(sub.ceiling));
    }
    if (sub.subscriber->missed.exchange(false)) {
        m.key("overflow");
        m.boolean(true);
    }
    m.endObject();
    // There is nothing to resend from later: "overflow" tells the client to
    // re-read. So it is when the heap couldn't hold this one.
    if (!m.ok() || !sub.subscriber->notify(msg, Delivery::Reliable)) sub.subscriber->missed = true;
}

}  // namespace

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

// The route in use changed (`from` was it before): subscriptions move to the
// new one, now.
void RemoteNode::rerouted(const Route& from, uint32_t nowMs) {
    std::vector<std::pair<std::string, Access>> paths;
    {
        MutexGuard guard(mutex_);
        for (auto& u : upstreams_) {
            if (!u->ready) continue;
            paths.emplace_back(remotePathOf(u->rest), u->level);
            u->stale = true;  // renewed below, on the new route
            u->lastSubscribeMs = nowMs - 2000;
        }
    }
    if (mirror_) {
        paths.emplace_back(remotePath_, mirrorLevel_);
        subscribedOnce_ = false;  // tick() subscribes again at once
    }
    // Back to a preferred route while the other one still works: drop what
    // the device sends that way. (A lost route's subscriptions end on their
    // own: the device fails to deliver to it.)
    if (!from.endpoint || !from.online || !hasRoute(from.endpoint, from.peer)) return;
    for (const auto& p : paths) {
        Query q;
        q.ceiling = p.second;
        from.endpoint->request(
            from.peer, Op::Unsubscribe, p.first, std::string_view(), [](Status, JsonVariantConst) {}, q, timeoutMs_,
            this);
    }
}

RemoteNode& RemoteNode::mirror(uint32_t intervalMs, uint32_t refreshMs, Access level) {
    mirror_ = true;
    mirrorLevel_ = level;
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

RemoteNode::Upstream* RemoteNode::findUpstream(std::string_view rest, Access level) {
    for (auto& u : upstreams_) {
        if (u->rest == rest && u->level == level) return u.get();
    }
    return nullptr;
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
        rerouted(seen.from, nowMs);
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

    // Forwarded subscriptions: renewed every refreshMs in case the remote
    // node restarted, every 2 s after a failed renewal.
    struct Renewal {
        std::string rest;
        Access level;
        uint32_t interval;
    };
    std::vector<Renewal> renew;
    {
        MutexGuard guard(mutex_);
        for (auto& u : upstreams_) {
            if (u->subscribing || !u->ready) continue;
            uint32_t every = u->stale ? 2000 : refreshMs_;
            if (nowMs - u->lastSubscribeMs < every) continue;
            u->subscribing = true;
            u->lastSubscribeMs = nowMs;
            renew.push_back(Renewal{u->rest, u->level, u->interval});
        }
    }
    for (auto& r : renew) {
        if (!sendUpstream(r.rest, r.level, r.interval, true)) {
            MutexGuard guard(mutex_);
            if (Upstream* u = findUpstream(r.rest, r.level)) u->subscribing = false;
        }
    }

    // Streamed sets that couldn't be sent (no call slot, a busy peer).
    pump();

    if (!mirror_ || subscribing_) return;
    if (subscribedOnce_) {
        // Renew: after refreshMs with a copy, every 2 s while there is none.
        uint32_t every = hasCopy() ? refreshMs_ : 2000;
        if (nowMs - lastSubscribeMs_ < every) return;
    }
    subscribedOnce_ = true;
    lastSubscribeMs_ = nowMs;
    subscribing_ = true;
    // Drop any previous subscription first, so renewals don't pile up.
    Query unsub;
    unsub.ceiling = mirrorLevel_;
    cur.endpoint->request(
        cur.peer, Op::Unsubscribe, remotePath_, std::string_view(), [](Status, JsonVariantConst) {}, unsub,
        timeoutMs_, this);
    Query q;
    q.interval = mirrorIntervalMs_;
    q.remotes = false;  // never mirror the peer's own mirrors: no loops between gateways
    q.ceiling = mirrorLevel_;  // the copy is what a client at that level reads
    // The snapshot is parsed once, from the reply's text, straight into the
    // copy (the old one goes first), and only with room for it in the byte
    // budget: parsed as a reply, then copied, it was held three ways at
    // once, and a peer coming back after a restart took a gateway's free
    // heap 2 KB below its floor (DEVKIT-V3 mirroring DEVKIT-R1).
    bool sent = cur.endpoint->requestRaw(
        cur.peer, Op::Subscribe, remotePath_, JsonVariantConst(),
        [this](std::string_view text) {
            subscribing_ = false;
            // A document takes up to about three times its text in small
            // values.
            Reservation room = memoryBudget().reserve(3 * text.size(), 0, false);
            if (!room) return;  // retried in 2 s
            {
                MutexGuard guard(mutex_);
                copy_.clear();
                hasCopy_ = !deserializeJson(copy_, text.data(), text.size(),
                                            DeserializationOption::NestingLimit(kWrittenNesting));
                if (!hasCopy_) copy_.clear();
            }
            changed();
        },
        [this](Status, JsonVariantConst) { subscribing_ = false; },  // retried in 2 s
        q, timeoutMs_, this);
    if (!sent) subscribing_ = false;
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
    bool forMirror = mirror_ && level == mirrorLevel_;
    std::string_view op(envelope["op"] | "");
    if (op == "change") {
        if (!forMirror || !rest.empty()) return relayChange(rest, level, envelope["body"]);
        {
            MutexGuard guard(mutex_);
            // Merged, the change takes about twice its text more in the
            // copy: only within the budget (docs/DESIGN.md section 15). Else
            // the copy goes, and is taken again whole within 2 s.
            Reservation room = memoryBudget().reserve(2 * measureJson(envelope["body"]), 0, false);
            if (!room) {
                copy_.clear();
                hasCopy_ = false;
            } else if (hasCopy_) {
                detail::mergeJson(copy_.as<JsonVariant>(), envelope["body"]);
            } else {
                copy_.set(envelope["body"]);
                hasCopy_ = true;
            }
        }
        changed();
        return true;
    }
    if (op == "event") {
        if (!forMirror && upstreamCount() == 0) return false;
        std::string local;
        if (!localPath(local)) return true;
        local.append(rest.data(), rest.size());
        JsonVariantConst body = envelope["body"];
        // It came at `level`, once for every subscription of this node's at
        // that level it concerns: for the local subscribers at that level
        // below this node, and, from the mirror, for those of the copy.
        api_.load()->emitEvent(
            local, [body](JsonWriter& w) { w.variant(body); },
            [this, level, forMirror](const Subscription& sub) {
                if (sub.forwarded) return sub.node == this && sub.access == level;
                return forMirror && sub.access >= level;
            });
        return true;
    }
    return false;
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
    Api* a = &api;
    auto done = [detached, a, localBase, remoteBase](Status s, JsonVariantConst body) {
        detail::writeMapped(*detached, s, body, localBase, remoteBase);
        detached->release();
        a->pendingDone();
    };
    // An "ok" body goes on as the remote wrote it: parsed and rewritten, a
    // large one would be on the heap twice.
    std::string okPath = localBase.empty() ? std::string("/") : localBase;
    auto raw = [detached, a, okPath](std::string_view json) {
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
        detached->release();
        a->pendingDone();
    };
    // No more than the client may (docs/DESIGN.md section 13.1).
    Query q = request.query;
    q.ceiling = request.client.level();
    if (!r.endpoint->requestRaw(r.peer, request.op, remotePathOf(rest), request.body, raw, done, q, timeoutMs_,
                                this)) {
        writeError(*detached, Status::Busy, Reason::Unreachable, basePath, "couldn't send to the remote node");
        detached->release();
        api.pendingDone();
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
    std::string_view basePath = base.empty() ? std::string_view("/") : base;
    if (!req.subscriber) return writeError(reply, Status::NotAllowed, Reason::CannotWait, basePath, "this transport can't deliver notifications");
    if (!req.body.isNull()) {
        return writeError(reply, Status::BadRequest, basePath, "subscriptions take keys, exclude and depth, not shapes");
    }
    if (req.query.view != View::Value) return writeError(reply, Status::BadRequest, basePath, "subscriptions are on values");
    Route route;
    if (!this->route(route)) return writeError(reply, Status::Internal, basePath, "no endpoint");
    Status allowed = api.subscriptionAllowed(req.subscriber);
    if (allowed != Status::Ok) return writeError(reply, allowed, Reason::SubscriptionLimit, basePath, "too many subscriptions");

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
    // Shared only with subscribers at the same level, which the upstream
    // subscription asks the remote for (docs/DESIGN.md section 13.1).
    sub.access = req.client.level();
    sub.ceiling = req.query.ceiling;
    Access level = sub.access;

    bool start = false, renew = false;
    uint32_t interval = 0;
    // What the upstream subscription must hold once this one joins.
    Spec want = wanted(sub.path, level, &sub);
    {
        MutexGuard guard(mutex_);
        Upstream* u = findUpstream(rest, level);
        if (u && u->ready && covers(u->copySpec, specOf(sub))) {
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
        if (!online()) {
            std::string localBase(base.substr(0, base.size() - rest.size()));
            return writeError(reply, Status::Timeout, Reason::PeerOffline, localBase.empty() ? std::string_view("/") : localBase,
                              "peer offline");
        }
        // A new or wider copy of the remote's state, and the snapshots
        // rendered from it: only with room to spare above the heap floor.
        if (!heapAbove(heapFloor())) {
            return writeError(reply, Status::Busy, Reason::LowMemory, basePath, "not enough memory for another remote subscription");
        }
        if (!api.pendingBegin()) return writeError(reply, Status::Busy, Reason::PendingLimit, basePath, "too many pending calls");
        Reply* detached = reply.detach();
        if (!detached) {
            api.pendingDone();
            return writeError(reply, Status::NotAllowed, Reason::CannotWait, basePath, "this transport can't wait for remote nodes");
        }
        sub.waiting = detached;
        if (!u) {
            upstreams_.emplace_back(new Upstream());
            u = upstreams_.back().get();
            u->rest.assign(rest.data(), rest.size());
            u->level = level;
            // An events-only subscriber doesn't pace the others' changes.
            u->interval = sub.changes ? req.query.interval : 0;
        }
        // A copy that doesn't hold what this one needs: subscribed again,
        // wider; this one waits for that snapshot.
        u->spec = want;
        interval = u->interval;
        if (!u->subscribing) {
            u->subscribing = true;
            u->lastSubscribeMs = route.endpoint->now();
            start = true;
            renew = u->ready;
        }
    }
    std::string r(rest);
    api.addSubscription(std::move(sub));  // waiting for the upstream snapshot
    if (start && !sendUpstream(r, level, interval, renew)) upstreamReply(r, level, Status::Busy, JsonVariantConst());
}

RemoteNode::Spec RemoteNode::wanted(const std::string& local, Access level, const Subscription* extra) {
    std::vector<Spec> specs;
    if (extra) specs.push_back(specOf(*extra));
    if (Api* api = api_) {
        api->forwardedSubscriptions(this, [&](Subscription& sub) {
            if (sub.path == local && sub.access == level) specs.push_back(specOf(sub));
            return true;
        });
    }
    return unionOf(specs);
}

void RemoteNode::narrowed(std::string_view localPath, Access level) {
    if (localPath.size() < localPath_.size()) return;
    std::string rest(localPath.substr(localPath_.size()));
    Spec want = wanted(std::string(localPath), level, nullptr);
    uint32_t interval;
    {
        MutexGuard guard(mutex_);
        Upstream* u = findUpstream(rest, level);
        if (!u || u->spec == want) return;
        u->spec = want;
        // In flight: its reply sees the new spec and asks again.
        if (u->subscribing || !u->ready) return;
        u->subscribing = true;
        interval = u->interval;
    }
    if (!sendUpstream(rest, level, interval, true)) {
        MutexGuard guard(mutex_);
        if (Upstream* u = findUpstream(rest, level)) u->subscribing = false;
    }
}

bool RemoteNode::upstreamSpec(std::string_view localPath, std::string& keys, std::string& exclude, int& depth,
                              Access level) const {
    if (localPath.size() < localPath_.size()) return false;
    std::string_view rest = localPath.substr(localPath_.size());
    MutexGuard guard(mutex_);
    for (const auto& u : upstreams_) {
        if (u->rest != rest || u->level != level) continue;
        keys = u->sent.keys;
        exclude = u->sent.exclude;
        depth = u->sent.depth;
        return true;
    }
    return false;
}

bool RemoteNode::sendUpstream(const std::string& rest, Access level, uint32_t interval, bool renew) {
    Route r;
    if (!route(r)) return false;
    std::string path = remotePathOf(rest);
    if (renew) {
        Query unsub;
        unsub.ceiling = level;
        r.endpoint->request(
            r.peer, Op::Unsubscribe, path, std::string_view(), [](Status, JsonVariantConst) {}, unsub, timeoutMs_,
            this);
    }
    Spec spec;
    {
        MutexGuard guard(mutex_);
        if (Upstream* u = findUpstream(rest, level)) {
            u->sent = u->spec;
            spec = u->spec;
        }
    }
    // Only what the local subscribers need (keys, depth): a whole tree
    // would cost the remote, the link and this node's heap.
    Query q;
    q.interval = interval;
    q.keys = spec.keys;
    q.exclude = spec.exclude;
    q.depth = spec.depth;
    q.ceiling = level;  // the remote filters as for these subscribers
    return r.endpoint->request(
        r.peer, Op::Subscribe, path, std::string_view(),
        [this, rest, level](Status s, JsonVariantConst body) { upstreamReply(rest, level, s, body); }, q, timeoutMs_,
        this);
}

void RemoteNode::upstreamReply(const std::string& rest, Access level, Status s, JsonVariantConst body) {
    Api* api = api_;
    if (!api) return;
    bool again = false;
    uint32_t interval = 0;
    {
        // Lock order: the API, then this node.
        MutexGuard apiGuard(api->mutex());
        MutexGuard guard(mutex_);
        Upstream* u = findUpstream(rest, level);
        if (!u) {
            // Released while subscribing: drop the subscription it made.
            Route r;
            if (s == Status::Ok && route(r)) {
                Query unsub;
                unsub.ceiling = level;
                r.endpoint->request(
                    r.peer, Op::Unsubscribe, remotePathOf(rest), std::string_view(), [](Status, JsonVariantConst) {},
                    unsub, timeoutMs_, this);
            }
            return;
        }
        u->subscribing = false;
        // A copy of the remote's state takes about twice its text: only
        // above the heap floor; else as if the remote were busy (kept stale,
        // tried again soon).
        JsonDocument noRoom;
        if (s == Status::Ok && !heapAbove(2 * measureJson(body))) {
            noRoom["error"] = "busy";
            noRoom["reason"] = "low_memory";
            noRoom["path"] = remotePathOf(rest);
            noRoom["message"] = "not enough memory for the remote's state";
            s = Status::Busy;
            body = noRoom.as<JsonVariantConst>();
        }
        bool wasReady = u->ready;
        bool changedCopy = false;
        bool reshaped = false;  // the copy now holds more or less, not newer state
        if (s == Status::Ok) {
            reshaped = wasReady && u->copySpec != u->sent;
            // Compared in place: two serialized copies of a large state could
            // exhaust a fragmented heap.
            changedCopy = !wasReady || (!reshaped && u->copy.as<JsonVariantConst>() != body);
            u->copy.set(body);
            u->copySpec = u->sent;
            u->ready = true;
            u->stale = false;
        } else if (wasReady) {
            u->stale = true;  // keep the copy and the subscribers; retry soon
        }

        std::string localPath = localPath_ + rest;
        std::string remoteBase = remotePath_ == "/" ? std::string() : remotePath_;
        std::string localBase = localPath_;
        bool anyLeft = false, stillWaiting = false;
        JsonVariantConst copy = u->copy.as<JsonVariantConst>();
        api->forwardedSubscriptions(this, [&](Subscription& sub) {
            if (sub.path != localPath || sub.access != level) return true;
            if (sub.waiting) {
                if (s == Status::Ok && !covers(u->copySpec, specOf(sub))) {
                    // Joined while a narrower request was in flight: next one.
                    stillWaiting = true;
                    anyLeft = true;
                    return true;
                }
                if (s == Status::Ok) {
                    JsonWriter w(sub.waiting->begin(Status::Ok));
                    if (sub.snapshot) {
                        writeSnapshot(w, copy, sub);
                    } else {
                        w.null();
                    }
                    detail::finishBody(*sub.waiting, w, sub.path);
                } else {
                    detail::writeMapped(*sub.waiting, s, body, localBase, remoteBase);
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
            return;
        }
        // The subscribers' needs changed while it was in flight.
        if (s == Status::Ok && (stillWaiting || u->spec != u->sent)) {
            u->subscribing = true;
            interval = u->interval;
            again = true;
        }
    }
    if (again && !sendUpstream(rest, level, interval, true)) {
        MutexGuard guard(mutex_);
        if (Upstream* u = findUpstream(rest, level)) {
            u->subscribing = false;
            u->stale = true;  // tick() tries again
        }
    }
}

bool RemoteNode::relayChange(std::string_view rest, Access level, JsonVariantConst patch) {
    Api* api = api_;
    if (!api) return false;
    MutexGuard apiGuard(api->mutex());
    MutexGuard guard(mutex_);
    Upstream* u = findUpstream(rest, level);
    if (!u || !u->ready) return false;
    // Only within the budget (docs/DESIGN.md section 15): else the change
    // isn't relayed, and the subscription is renewed soon, its subscribers
    // getting the whole state if it differs.
    Reservation room = memoryBudget().reserve(2 * measureJson(patch), 0, false);
    if (!room) {
        u->stale = true;  // renewed within 2 s
        return true;
    }
    detail::mergeJson(u->copy.as<JsonVariant>(), patch);
    std::string localPath = localPath_ + std::string(rest);
    api->forwardedSubscriptions(this, [&](Subscription& sub) {
        if (sub.path != localPath || sub.access != level || sub.waiting || !sub.changes) return true;
        if (!patch.is<JsonObjectConst>()) {
            notifyChange(sub, [&](JsonWriter& w) { w.variant(patch); });
        } else if (hasChanges(patch, depthOf(sub), &sub)) {
            notifyChange(sub, [&](JsonWriter& w) { writeChanges(w, patch, depthOf(sub), &sub); });
        }
        return true;
    });
    return true;
}

void RemoteNode::released(std::string_view localPath, Access level) {
    if (localPath.size() < localPath_.size()) return;
    std::string rest(localPath.substr(localPath_.size()));
    bool found = false;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < upstreams_.size(); i++) {
            if (upstreams_[i]->rest != rest || upstreams_[i]->level != level) continue;
            found = upstreams_[i]->ready;  // still subscribing: upstreamReply() unsubscribes
            upstreams_.erase(upstreams_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    Route r;
    if (found && route(r)) {
        Query unsub;
        unsub.ceiling = level;
        r.endpoint->request(
            r.peer, Op::Unsubscribe, remotePathOf(rest), std::string_view(), [](Status, JsonVariantConst) {}, unsub,
            timeoutMs_, this);
    }
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

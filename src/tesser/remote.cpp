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

// Routes: the library-wide order and state handler.
Mutex& sharedMutex() {
    static Mutex m;
    return m;
}
std::vector<RouteKind>& sharedOrder() {
    static std::vector<RouteKind> order;
    return order;
}
RemoteNode::StateHandler& sharedStateHandler() {
    static RemoteNode::StateHandler fn;
    return fn;
}
RemoteNode::AccessHandler& sharedAccessHandler() {
    static RemoteNode::AccessHandler fn;
    return fn;
}

}  // namespace

TransportKind RemoteNode::Route::kind() const { return endpoint ? endpoint->kind() : TransportKind::Local; }

std::string toString(const RouteKind& kind) {
    std::string s = toString(kind.transport);
    if (kind.iface != NetInterface::Any) {
        s += '/';
        s += toString(kind.iface);
    }
    return s;
}

bool parseRouteOrder(std::string_view text, std::vector<RouteKind>& out) {
    std::vector<RouteKind> order;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t comma = text.find(',', pos);
        if (comma == std::string_view::npos) comma = text.size();
        std::string_view item = text.substr(pos, comma - pos);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        pos = comma + 1;
        if (item.empty()) {
            if (comma == text.size() && order.empty()) break;  // "" is the empty order
            return false;
        }
        size_t slash = item.find('/');
        RouteKind k;
        if (!parseTransportKind(item.substr(0, slash), k.transport)) return false;
        if (slash != std::string_view::npos && !parseNetInterface(item.substr(slash + 1), k.iface)) return false;
        order.push_back(k);
    }
    out = std::move(order);
    return true;
}

std::string formatRouteOrder(const std::vector<RouteKind>& order) {
    std::string s;
    for (const RouteKind& k : order) {
        if (!s.empty()) s += ", ";
        s += toString(k);
    }
    return s;
}

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
    std::vector<DatagramEndpoint*> endpoints;
    {
        MutexGuard guard(routesMutex_);
        for (const RouteSlot& r : routes_) {
            bool known = false;
            for (DatagramEndpoint* e : endpoints) known = known || e == r.route.endpoint;
            if (!known) endpoints.push_back(r.route.endpoint);
        }
    }
    for (DatagramEndpoint* e : endpoints) {
        e->removeRemote(this);
        e->cancelCalls(this);
    }
}

// ---- routes -----------------------------------------------------------------

uint32_t RemoteNode::lastHeardMs() const {
    MutexGuard guard(routesMutex_);
    uint32_t last = 0;
    for (const RouteSlot& r : routes_) {
        if (r.route.heardMs && (!last || static_cast<int32_t>(r.route.heardMs - last) > 0)) last = r.route.heardMs;
    }
    return last;
}

bool RemoteNode::onlyThrough(const DatagramEndpoint* endpoint) const {
    MutexGuard guard(routesMutex_);
    for (const RouteSlot& r : routes_) {
        if (r.route.endpoint != endpoint) return false;
    }
    return true;
}

size_t RemoteNode::routeCount(const DatagramEndpoint* endpoint) const {
    MutexGuard guard(routesMutex_);
    size_t n = 0;
    for (const RouteSlot& r : routes_) n += r.route.endpoint == endpoint;
    return n;
}

bool RemoteNode::replaceOfflineRoute(DatagramEndpoint& endpoint, const PeerAddress& peer) {
    {
        MutexGuard guard(routesMutex_);
        RouteSlot* oldest = nullptr;
        for (RouteSlot& r : routes_) {
            if (r.route.endpoint != &endpoint || r.route.online) continue;
            if (!oldest || static_cast<int32_t>(r.route.heardMs - oldest->route.heardMs) < 0) oldest = &r;
        }
        if (!oldest) return false;
        if (lastEndpoint_ == &endpoint && lastPeer_ == oldest->route.peer) lastPeer_ = peer;
        oldest->route.peer = peer;
        oldest->route.heardMs = 0;
        oldest->route.rttMs = 0;
        oldest->route.iface = endpoint.interfaceOf(peer);
    }
    endpoint.routeAdded(this, peer);
    std::string token = this->token();
    if (!token.empty()) endpoint.setPeerToken(peer, token);
    announce_ = true;
    return true;
}

void RemoteNode::addRoute(DatagramEndpoint& endpoint, const PeerAddress& peer) {
    bool registered = false;
    NetInterface iface = endpoint.interfaceOf(peer);
    {
        MutexGuard guard(routesMutex_);
        for (const RouteSlot& r : routes_) {
            if (r.route.endpoint != &endpoint) continue;
            if (r.route.peer == peer) return;
            registered = true;
        }
        RouteSlot slot;
        slot.route.endpoint = &endpoint;
        slot.route.peer = peer;
        slot.route.iface = iface;
        slot.added = nextRoute_++;
        routes_.push_back(slot);
        if (!lastEndpoint_) {
            lastEndpoint_ = &endpoint;
            lastPeer_ = peer;
        }
    }
    if (!registered) endpoint.addRemote(this);
    endpoint.routeAdded(this, peer);
    std::string token = this->token();
    if (!token.empty()) endpoint.setPeerToken(peer, token);
    announce_ = true;
}

void RemoteNode::setToken(std::string_view token) {
    std::vector<RouteSlot> routes;
    {
        MutexGuard guard(routesMutex_);
        token_.assign(token.data(), token.size());
        routes = routes_;
    }
    for (const RouteSlot& r : routes) r.route.endpoint->setPeerToken(r.route.peer, token);
}

std::string RemoteNode::token() const {
    MutexGuard guard(routesMutex_);
    return token_;
}

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

std::vector<RouteKind> RemoteNode::inheritedOrder() const {
    if (Api* api = api_.load()) {
        std::vector<RouteKind> order = api->remoteHooks().routeOrder();
        if (!order.empty()) return order;
    }
    return defaultRouteOrder();
}

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
    MutexGuard guard(routesMutex_);
    for (const RouteSlot& r : routes_) {
        if (r.route.endpoint == endpoint && r.route.peer == peer) return true;
    }
    return false;
}

bool RemoteNode::routeUp(const DatagramEndpoint* endpoint, const PeerAddress& peer) const {
    MutexGuard guard(routesMutex_);
    for (const RouteSlot& r : routes_) {
        if (r.route.endpoint == endpoint && r.route.peer == peer) return r.route.online;
    }
    return false;
}

// The index of the route in use: the best ranked online one, or the best
// ranked one when none is online.
int RemoteNode::activeLocked() const {
    std::vector<RouteKind> order = hasOrder_ ? order_ : inheritedOrder();
    auto rank = [&](const RouteSlot& r) {
        size_t i = 0;
        while (i < order.size() && !order[i].covers(r.route.kind(), r.route.iface)) i++;
        return (uint64_t(i) << 32) | r.added;
    };
    int best = -1, bestOnline = -1;
    for (size_t i = 0; i < routes_.size(); i++) {
        int n = static_cast<int>(i);
        if (best < 0 || rank(routes_[i]) < rank(routes_[static_cast<size_t>(best)])) best = n;
        if (routes_[i].route.online &&
            (bestOnline < 0 || rank(routes_[i]) < rank(routes_[static_cast<size_t>(bestOnline)]))) {
            bestOnline = n;
        }
    }
    return bestOnline >= 0 ? bestOnline : best;
}

std::vector<RemoteNode::Route> RemoteNode::routes() const {
    std::vector<RouteSlot> slots;
    std::vector<RouteKind> order;
    {
        MutexGuard guard(routesMutex_);
        slots = routes_;
        order = hasOrder_ ? order_ : inheritedOrder();
    }
    auto rank = [&](const RouteSlot& r) {
        size_t i = 0;
        while (i < order.size() && !order[i].covers(r.route.kind(), r.route.iface)) i++;
        return (uint64_t(i) << 32) | r.added;
    };
    // A handful of routes: insertion sort.
    for (size_t i = 1; i < slots.size(); i++) {
        for (size_t j = i; j > 0 && rank(slots[j]) < rank(slots[j - 1]); j--) std::swap(slots[j], slots[j - 1]);
    }
    std::vector<Route> out;
    for (const RouteSlot& r : slots) out.push_back(r.route);
    return out;
}

bool RemoteNode::route(Route& out) const {
    MutexGuard guard(routesMutex_);
    int i = activeLocked();
    if (i < 0) return false;
    out = routes_[static_cast<size_t>(i)].route;
    return true;
}

DatagramEndpoint* RemoteNode::endpoint() const {
    Route r;
    return route(r) ? r.endpoint : nullptr;
}

PeerAddress RemoteNode::peer() const {
    Route r;
    return route(r) ? r.peer : PeerAddress();
}

RemoteNode& RemoteNode::routeOrder(std::vector<RouteKind> order) {
    {
        MutexGuard guard(routesMutex_);
        hasOrder_ = !order.empty();
        order_ = std::move(order);
    }
    return *this;
}

std::vector<RouteKind> RemoteNode::routeOrder() const {
    MutexGuard guard(routesMutex_);
    return order_;
}

void RemoteNode::setDefaultRouteOrder(std::vector<RouteKind> order) {
    MutexGuard guard(sharedMutex());
    sharedOrder() = std::move(order);
}

std::vector<RouteKind> RemoteNode::defaultRouteOrder() {
    MutexGuard guard(sharedMutex());
    return sharedOrder();
}

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

PeerAddress RemoteNode::device() const {
    MutexGuard guard(routesMutex_);
    return device_;
}

void RemoteNode::setDevice(const PeerAddress& device) {
    {
        MutexGuard guard(routesMutex_);
        if (device_ == device) return;
        device_ = device;
    }
    changed();  // shown in the schema
}

// Under routesMutex_: online if any route is. True if that changed.
void RemoteNode::updateOnlineLocked() {
    bool any = false;
    for (const RouteSlot& r : routes_) any = any || r.route.online;
    // Shown in the schema, but not part of its hash (docs/DESIGN.md section
    // 7): subscribers of the parent hear about it as a change.
    if (online_.exchange(any) != any) changed();
}

void RemoteNode::setOnline(bool online) {
    MutexGuard guard(routesMutex_);
    for (RouteSlot& r : routes_) r.route.online = online;
    updateOnlineLocked();
}

void RemoteNode::setRouteOnline(const DatagramEndpoint* endpoint, const PeerAddress& peer, bool online,
                                uint32_t nowMs) {
    // Back online: the interface may have changed (addresses come and go).
    bool back = false;
    if (online) {
        MutexGuard guard(routesMutex_);
        for (const RouteSlot& r : routes_) back = back || (r.route.endpoint == endpoint && r.route.peer == peer && !r.route.online);
    }
    NetInterface iface = back ? endpoint->interfaceOf(peer) : NetInterface::Any;
    MutexGuard guard(routesMutex_);
    for (RouteSlot& r : routes_) {
        if (r.route.endpoint != endpoint || r.route.peer != peer) continue;
        if (back) r.route.iface = iface;
        if (r.route.online && !online) {
            lastProbeMs_ = nowMs;  // the first probe waits probeMs
            probeWaitMs_ = probeMs_;
        }
        r.route.online = online;
        if (online && nowMs) r.route.heardMs = nowMs;
    }
    updateOnlineLocked();
}

void RemoteNode::routeRoundTrip(const DatagramEndpoint* endpoint, const PeerAddress& peer, uint32_t rttMs) {
    if (!rttMs) rttMs = 1;
    MutexGuard guard(routesMutex_);
    for (RouteSlot& r : routes_) {
        if (r.route.endpoint != endpoint || r.route.peer != peer) continue;
        // Smoothed like TCP's: an eighth of each new sample.
        r.route.rttMs = r.route.rttMs ? (r.route.rttMs * 7 + rttMs + 4) / 8 : rttMs;
    }
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
    MutexGuard guard(routesMutex_);
    for (size_t i = 0; i < routes_.size();) {
        if (routes_[i].route.endpoint == endpoint) {
            routes_.erase(routes_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
    if (lastEndpoint_ == endpoint) lastEndpoint_ = nullptr;
    if (routes_.empty()) {
        online_ = false;
    } else {
        updateOnlineLocked();
    }
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
    bool on = online();
    bool switched = false, stateChanged = false;
    Route from;
    {
        MutexGuard guard(routesMutex_);
        if (!lastEndpoint_) {
            lastEndpoint_ = cur.endpoint;
            lastPeer_ = cur.peer;
            lastOnline_ = on;
        }
        if (lastEndpoint_ != cur.endpoint || lastPeer_ != cur.peer) {
            switched = true;
            from.endpoint = lastEndpoint_;
            from.peer = lastPeer_;
            for (const RouteSlot& r : routes_) {
                if (r.route.endpoint == from.endpoint && r.route.peer == from.peer) from.online = r.route.online;
            }
            lastEndpoint_ = cur.endpoint;
            lastPeer_ = cur.peer;
        }
        stateChanged = switched || lastOnline_ != on || announce_.exchange(false);
        lastOnline_ = on;
    }
    if (switched) {
        changed();  // the schema shows the route
        rerouted(from, nowMs);
    }
    if (stateChanged) {
        StateHandler fn;
        {
            MutexGuard guard(sharedMutex());
            fn = sharedStateHandler();
        }
        if (fn) fn(*this);
        if (Api* api = api_.load()) api->remoteHooks().stateChanged(*this);
    }

    // Offline routes: does the device answer? Any reply puts the route back.
    std::vector<Route> probes;
    {
        MutexGuard guard(routesMutex_);
        if (probeMs_ && nowMs - lastProbeMs_ >= (probeWaitMs_ ? probeWaitMs_ : probeMs_)) {
            for (const RouteSlot& r : routes_) {
                if (!r.route.online) probes.push_back(r.route);
            }
            if (!probes.empty()) {
                lastProbeMs_ = nowMs;
                uint32_t base = probeWaitMs_ ? probeWaitMs_ : probeMs_;
                probeWaitMs_ = base >= kMaxProbeMs / 2 ? kMaxProbeMs : base * 2;
            }
        }
    }
    for (const Route& r : probes) {
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

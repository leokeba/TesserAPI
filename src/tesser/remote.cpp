#include "tesser/remote.h"

#include <stdio.h>

#include <algorithm>

#include "tesser/api.h"
#include "tesser/budget.h"
#include "tesser/call.h"
#include "tesser/detail/lazy_patch.h"
#include "tesser/envelope.h"
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

// A remote node's reply, written to a local reply: error paths are mapped
// back under the local path of the remote node.
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


}  // namespace

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

constexpr uint8_t kStreamAttempts = 3;  // a streamed set that timed out is sent twice more

// A path below a remote node: "" for the node, else "/a/b".
std::string normalPath(std::string_view p) {
    std::string s(p);
    if (!s.empty() && s.front() != '/') s.insert(0, "/");
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

// One path is the other or below it.
bool overlaps(const std::string& a, const std::string& b) {
    const std::string& shorter = a.size() < b.size() ? a : b;
    const std::string& longer = a.size() < b.size() ? b : a;
    return longer.compare(0, shorter.size(), shorter) == 0 &&
           (longer.size() == shorter.size() || longer[shorter.size()] == '/');
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
    std::vector<RouteKind> order = hasOrder_ ? order_ : defaultRouteOrder();
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
        order = hasOrder_ ? order_ : defaultRouteOrder();
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
    if (!api_) api_ = Api::owner(*this, localPath_);
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
        if (!forMirror && upstreamCount() == 0) return false;
        std::string local;
        if (!localPath(local)) return true;
        local.append(rest.data(), rest.size());
        JsonVariantConst body = envelope["body"];
        // It came at `level`, once for every subscription of this node's at
        // that level it concerns: for the local subscribers at that level
        // below this node, and, from the mirror, for those of the copy.
        api_->emitEvent(
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
    if (!api.pendingBegin()) return writeError(reply, Status::Busy, Reason::PendingLimit, basePath, "too many pending calls");
    Reply* detached = reply.detach();
    if (!detached) {
        api.pendingDone();
        return writeError(reply, Status::NotAllowed, Reason::CannotWait, basePath, "this transport can't wait for remote nodes");
    }
    std::string remoteBase = remotePath_ == "/" ? std::string() : remotePath_;
    Api* a = &api;
    auto done = [detached, a, localBase, remoteBase](Status s, JsonVariantConst body) {
        writeMapped(*detached, s, body, localBase, remoteBase);
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
    std::string p = normalPath(path);
    {
        MutexGuard guard(streamMutex_);
        // On top of the last set queued for the path, unless a set to a path
        // above or below it came after (they must stay in order).
        bool merged = false;
        for (size_t i = queue_.size(); i-- > 0;) {
            if (queue_[i].path == p) {
                merge(queue_[i].body.as<JsonVariant>(), body.as<JsonVariantConst>());
                merged = true;
                break;
            }
            if (overlaps(queue_[i].path, p)) break;
        }
        if (merged) {
            streamStats_.queued++;
            streamStats_.merged++;
        } else {
            if (queue_.size() >= streamLimit_) return Status::Busy;
            queue_.emplace_back();
            queue_.back().path = std::move(p);
            queue_.back().body = std::move(body);
            streamStats_.queued++;
        }
    }
    pump();
    return Status::Ok;
}

void RemoteNode::onStreamError(StreamErrorHandler fn) {
    MutexGuard guard(streamMutex_);
    onStreamError_ = std::move(fn);
}

RemoteNode::StreamStats RemoteNode::streamStats() const {
    MutexGuard guard(streamMutex_);
    return streamStats_;
}

size_t RemoteNode::streaming() const {
    MutexGuard guard(streamMutex_);
    return queue_.size() + (inFlight_ ? 1 : 0);
}

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

// Puts a set that must go again at the front, with the later sets to its
// path merged on top (up to one to a path above or below it).
void RemoteNode::requeue(std::vector<Pending>& queue, Pending p) {
    for (size_t i = 0; i < queue.size();) {
        if (queue[i].path == p.path) {
            merge(p.body.as<JsonVariant>(), queue[i].body.as<JsonVariantConst>());
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (overlaps(queue[i].path, p.path)) break;
        i++;
    }
    queue.insert(queue.begin(), std::move(p));
}

// Sends the next streamed set if none is in flight; drops them all if the
// node is offline.
void RemoteNode::pump() {
    Route r;
    bool reachable = route(r) && online();
    std::vector<Pending> dropped;
    std::string path, body;
    StreamErrorHandler onError;
    {
        MutexGuard guard(streamMutex_);
        if (inFlight_ || queue_.empty()) return;
        if (!reachable) {
            dropped.swap(queue_);
            streamStats_.failed += static_cast<uint32_t>(dropped.size());
            onError = onStreamError_;
        } else {
            flying_ = std::move(queue_.front());
            queue_.erase(queue_.begin());
            inFlight_ = true;
            streamStats_.sent++;
            path = flying_.path;
            serializeJson(flying_.body, body);
        }
    }
    if (!reachable) {
        if (!onError) return;
        JsonDocument error;
        error["error"] = "timeout";
        error["reason"] = "peer_offline";
        error["message"] = "peer offline";
        for (const Pending& p : dropped) {
            error["path"] = p.path.empty() ? std::string("/") : p.path;
            onError(p.path, Status::Timeout, error.as<JsonVariantConst>());
        }
        return;
    }
    bool sent = r.endpoint->request(
        r.peer, Op::Set, remotePathOf(path), std::string_view(body),
        [this](Status s, JsonVariantConst reply) { streamDone(s, reply); }, Query(), streamTimeoutMs(), this);
    if (!sent) {
        // No call slot or no room on the link: tick() tries again.
        MutexGuard guard(streamMutex_);
        inFlight_ = false;
        streamStats_.sent--;
        requeue(queue_, std::move(flying_));
    }
}

void RemoteNode::streamDone(Status s, JsonVariantConst body) {
    bool retry = s == Status::Timeout || s == Status::Busy;
    std::string failed;
    StreamErrorHandler onError;
    {
        MutexGuard guard(streamMutex_);
        if (!inFlight_) return;
        inFlight_ = false;
        Pending p = std::move(flying_);
        if (retry && ++p.attempts < kStreamAttempts) {
            // Applied or not, it goes again, under anything newer: on the
            // next route if this one was lost.
            requeue(queue_, std::move(p));
        } else if (s != Status::Ok) {
            failed = p.path;
            streamStats_.failed++;
            onError = onStreamError_;
            retry = false;
        }
    }
    if (onError) onError(failed, s, body);
    // A busy peer gets a moment: tick() sends it again.
    if (s != Status::Busy) pump();
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
    merge(u->copy.as<JsonVariant>(), patch);
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

// ---- relayed file transfers --------------------------------------------------

namespace {

constexpr int kAttempts = 3;            // a call whose reply timed out is sent twice more
constexpr size_t kFallbackChunk = 192;  // a peer that gave no chunk size

// An envelope's op for a client at `level`: "upload", "upload@user"
// (docs/DESIGN.md section 13.1).
std::string opFor(const char* op, Access level) {
    std::string s(op);
    if (level < Access::Admin) {
        s += '@';
        s += toString(level);
    }
    return s;
}

// Strict (docs/DESIGN.md section 10.1.2): a node that doesn't know an
// option refuses the envelope rather than ignore it.
std::string fieldsOf(const std::function<void(JsonWriter&)>& write) {
    std::string text;
    StringSink sink(text);
    JsonWriter w(sink);
    w.beginObject();
    write(w);
    w.key("v");
    w.integer(kProtocolVersion);
    w.endObject();
    return text.substr(1, text.size() - 2);  // the members, without the braces
}

// An envelope's builder (DatagramEndpoint::requestBuilt()) from its members.
std::shared_ptr<DatagramEndpoint::Builder> builderOf(std::string fields) {
    return std::make_shared<DatagramEndpoint::Builder>(
        [fields = std::move(fields)](uint32_t id, DatagramEndpoint::EnvelopeOut& out) {
            char head[20];
            int n = snprintf(head, sizeof(head), "{\"id\":%lu,", static_cast<unsigned long>(id));
            if (!out.reserve(static_cast<size_t>(n) + fields.size() + 1)) return false;
            out.append(head, static_cast<size_t>(n));
            out.append(fields);
            out.append("}", 1);
            return true;
        });
}

class RemoteFile : public FileRelay, public TransferHolder, public std::enable_shared_from_this<RemoteFile> {
public:
    RemoteFile(Api& api, RemoteNode& node, std::string remotePath, std::string localBase, std::string remoteBase,
               std::string localPath, Access level)
        : api_(api),
          node_(node),
          remotePath_(std::move(remotePath)),
          localBase_(std::move(localBase)),
          remoteBase_(std::move(remoteBase)),
          localPath_(std::move(localPath)),
          get_(opFor("get", level)),
          upload_(opFor("upload", level)),
          download_(opFor("download", level)) {
        node_.relayHeld(true);  // not evicted meanwhile
        // The route the transfer goes by, start to end: the peer keeps a
        // transfer per client, and the same gateway on another link is
        // another client (docs/DESIGN.md section 14).
        RemoteNode::Route route;
        if (node.route(route)) {
            endpoint_ = route.endpoint;
            peer_ = route.peer;
        }
        // The claim is on the peer, under a key no path can have.
        char hex[24];
        snprintf(hex, sizeof(hex), "@%lx:", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(endpoint_)));
        key_ = hex;
        for (uint8_t i = 0; i < peer_.length; i++) {
            snprintf(hex, sizeof(hex), "%02x", peer_.bytes[i]);
            key_ += hex;
        }
    }
    // Owners close() first; this only covers a relay dropped while claimed.
    ~RemoteFile() override {
        if (claimed_) api_.releaseTransfer(key_);
        node_.relayHeld(false);
    }

    bool claim(Reply& reply) override {
        MutexGuard guard(mutex_);
        if (!claimed_ && !api_.claimTransfer(key_, this)) {
            writeError(reply, Status::Busy, Reason::TransferBusy, localPath_, "another transfer to this node is in progress");
            return false;
        }
        claimed_ = true;
        activeMs_ = millis32();
        return true;
    }

    void probe(Done done) override {
        std::string path = remotePath_;
        call(builderOf(fieldsOf([&](JsonWriter& w) {
                 w.key("op");
                 w.string(get_);
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
        call(builderOf(fieldsOf([&](JsonWriter& w) {
                 w.key("op");
                 w.string(upload_);
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
        bool room = true;
        {
            MutexGuard guard(mutex_);
            // Grown to its exact need, within the byte budget (docs/DESIGN.md
            // section 15); else the chunk is refused, for the client to send
            // again.
            size_t need = pending_.size() + data.size();
            if (pending_.empty()) {
                pending_ = std::move(data);  // taken over, not copied
                data.clear();
            } else if (need > pending_.capacity()) {
                Reservation growth = memoryBudget().reserve(need, 0, true);
                room = static_cast<bool>(growth);
                if (room) pending_.reserve(need);
            }
            if (room) pending_.insert(pending_.end(), data.begin(), data.end());
        }
        if (!room) return lowMemory(done);
        pump(std::move(done));
    }

    void download(uint64_t offset, size_t length, Chunk done) override {
        // A part is at most this node's upload chunk, as a relayed upload's:
        // the remote's reply is held here whole, and in the remote's own
        // reply buffer (docs/DESIGN.md section 14).
        size_t cap = FileSession::uploadChunk(api_.config());
        {
            MutexGuard guard(mutex_);
            if (limit_ && cap > limit_) cap = limit_;
        }
        if (!length || length > cap) length = cap;
        // And what this node's heap holds now, the reply and its way on.
        length = FileSession::chunkInRoom(length, FileSession::kRelayedDownloadThirds);
        std::string path = remotePath_;
        auto self = shared_from_this();
        // The reply's fields apart from its data, which stays in the text.
        auto took = [self, done, offset, length](Status s, JsonVariantConst body, std::string_view data) {
            if (self->smaller(s, body, length)) {
                self->download(offset, length, done);  // asked again, for less
                return;
            }
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
            done(s, body, data);
        };
        auto raw = std::make_shared<DatagramEndpoint::RawHandler>([took](std::string_view text) {
            JsonDocument filter;
            for (const char* k : {"size", "filename", "contentType", "chunk", "offset", "end"}) filter[k] = true;
            JsonDocument body;
            std::string_view data;
            bool bad = deserializeJson(body, text.data(), text.size(), DeserializationOption::Filter(filter)) ||
                       !body.is<JsonObjectConst>();
            // A JSON string, without escapes (base64).
            if (!bad && jsonMember(text, "data", data)) {
                bad = data.size() < 2 || data.front() != '"' || data.back() != '"';
                if (!bad) data = data.substr(1, data.size() - 2);
            }
            if (bad) {
                JsonDocument error;
                error["error"] = "internal";
                error["message"] = "bad download reply";
                took(Status::Internal, error.as<JsonVariantConst>(), std::string_view());
                return;
            }
            took(Status::Ok, body.as<JsonVariantConst>(), data);
        });
        call(builderOf(fieldsOf([&](JsonWriter& w) {
                 w.key("op");
                 w.string(download_);
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
             [took](Status s, JsonVariantConst body) {
                 JsonString data = body["data"].as<JsonString>();
                 took(s, body, std::string_view(data.c_str() ? data.c_str() : "", data.size()));
             },
             0, raw);
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
        if (!t) t = node_.routeUp(endpoint_, peer_) ? endpoint_->timeoutMs() : 3000;
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
    // Sends one envelope by the transfer's route, again on a timeout while
    // the relay is open and the route online. Nothing is sent once the
    // route is offline or gone, even if the node has another: there, the
    // peer has no transfer from this gateway (`timeout`, which ends the
    // relay; the peer drops its side when that link loses this node, or
    // after Config::transferIdleMs).
    // With `raw`, an "ok" reply's body goes to it unparsed instead.
    void call(std::shared_ptr<DatagramEndpoint::Builder> build, Done done, int attempt = 0,
              std::shared_ptr<DatagramEndpoint::RawHandler> raw = nullptr) {
        if (!node_.routeUp(endpoint_, peer_)) {
            JsonDocument error;
            error["error"] = "timeout";
            error["reason"] = "peer_offline";
            error["path"] = remoteBase_.empty() ? std::string("/") : remoteBase_;
            error["message"] = node_.online() ? "the route the transfer started on is offline" : "peer offline";
            done(Status::Timeout, error.as<JsonVariantConst>());
            return;
        }
        {
            MutexGuard guard(mutex_);
            inFlight_ = true;
            activeMs_ = millis32();
        }
        DatagramEndpoint* ep = endpoint_;
        auto self = shared_from_this();
        noRoom_ = false;
        DatagramEndpoint::RawHandler rawOk;
        if (raw) {
            rawOk = [self, raw](std::string_view body) {
                self->landed();
                (*raw)(body);
            };
        }
        bool sent = ep && ep->requestBuilt(
                              peer_, *build,
                              [self, build, done, attempt, raw](Status s, JsonVariantConst body) {
                                  if (s == Status::Timeout && attempt + 1 < kAttempts && self->open() &&
                                      self->node_.routeUp(self->endpoint_, self->peer_)) {
                                      self->call(build, done, attempt + 1, raw);
                                      return;
                                  }
                                  self->landed();
                                  done(s, body);
                              },
                              node_.timeoutMs(), &node_, std::move(rawOk));
        if (!sent) {
            landed();
            if (noRoom_) return lowMemory(done, false);  // the envelope had no room: its caller decides
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
        std::shared_ptr<DatagramEndpoint::Builder> build;
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
                if (limit_ && part > limit_) part = limit_;
                if (part > pending_.size()) part = pending_.size();
                part = FileSession::chunkInRoom(part, FileSession::kRelayedPartThirds);
                std::string head = fieldsOf([&](JsonWriter& w) {
                    w.key("op");
                    w.string(upload_);
                    w.key("path");
                    w.string(remotePath_);
                    w.key("offset");
                    w.uinteger(offset);
                });
                // The whole envelope, built once at its exact size, the data
                // encoded in place from the bytes waiting; built again rather
                // than kept, should it go again (docs/DESIGN.md section 15).
                std::weak_ptr<RemoteFile> weak = shared_from_this();
                build = std::make_shared<DatagramEndpoint::Builder>(
                    [weak, head, part](uint32_t id, DatagramEndpoint::EnvelopeOut& out) {
                        auto self = weak.lock();
                        if (!self) return false;
                        MutexGuard lock(self->mutex_);
                        if (self->pending_.size() < part) return false;  // dropped meanwhile
                        char idHead[20];
                        int n = snprintf(idHead, sizeof(idHead), "{\"id\":%lu,", static_cast<unsigned long>(id));
                        static constexpr char kData[] = ",\"data\":\"";
                        size_t size =
                            static_cast<size_t>(n) + head.size() + sizeof(kData) - 1 + (part + 2) / 3 * 4 + 2;
                        Reservation room = memoryBudget().reserve(size + 32, 0, true);
                        self->noRoom_ = !room || !out.reserve(size);
                        if (self->noRoom_) return false;
                        out.append(idHead, static_cast<size_t>(n));
                        out.append(head);
                        out.append(kData, sizeof(kData) - 1);
                        // Encoded 48 bytes at a time into the envelope.
                        static const char kAlphabet[] =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        const uint8_t* d = self->pending_.data();
                        char buf[64];
                        size_t k = 0;
                        for (size_t i = 0; i < part; i += 3) {
                            uint32_t v = uint32_t(d[i]) << 16;
                            if (i + 1 < part) v |= uint32_t(d[i + 1]) << 8;
                            if (i + 2 < part) v |= d[i + 2];
                            buf[k++] = kAlphabet[v >> 18 & 63];
                            buf[k++] = kAlphabet[v >> 12 & 63];
                            buf[k++] = i + 1 < part ? kAlphabet[v >> 6 & 63] : '=';
                            buf[k++] = i + 2 < part ? kAlphabet[v & 63] : '=';
                            if (k == sizeof(buf)) {
                                out.append(buf, k);
                                k = 0;
                            }
                        }
                        out.append(buf, k);
                        out.append("\"}", 2);
                        return true;
                    });
            }
        }
        if (closed || idle) {
            done(closed ? Status::Busy : Status::Ok, JsonVariantConst());
            return;
        }
        auto self = shared_from_this();
        call(build, [self, done, offset, part](Status s, JsonVariantConst body) {
            if (self->smaller(s, body, part)) {
                self->pump(done);  // the same bytes, in smaller parts
                return;
            }
            if (s == Status::Busy) {
                // The peer (or this node) had no room or no queue: the
                // transfer stays open, and the client sends the rest again.
                MutexGuard guard(self->mutex_);
                self->pending_.clear();
            } else if (s != Status::Ok) {
                self->failed(s);
            }
            if (s != Status::Ok) {
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
    // correct, and busy, which it can try again.
    void failed(Status s) {
        if (s != Status::BadRequest && s != Status::Busy) close();
    }

    // No room to relay what the client sent: dropped, for it to send again
    // from received(); the transfer stays open.
    // A part refused for memory (busy, low_memory: by the peer, or here)
    // goes again at half the size, down to kMinPart; the parts stay that
    // small for the rest of the transfer (docs/DESIGN.md section 14).
    bool smaller(Status s, JsonVariantConst body, size_t part) {
        if (s != Status::Busy || body["reason"] != "low_memory" || part <= kMinPart || !open()) return false;
        MutexGuard guard(mutex_);
        size_t half = part / 2 / 3 * 3;  // whole base64 groups
        limit_ = half > kMinPart ? half : kMinPart;
        return true;
    }

    // `drop`: the bytes waiting go too, for the client to send again.
    void lowMemory(const Done& done, bool drop = true) {
        if (drop) {
            MutexGuard guard(mutex_);
            pending_.clear();
        }
        JsonDocument error;
        error["error"] = "busy";
        error["reason"] = "low_memory";
        error["path"] = remoteBase_.empty() ? std::string("/") : remoteBase_;
        error["message"] = "not enough memory to relay the chunk";
        done(Status::Busy, error.as<JsonVariantConst>());
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
    const std::string get_, upload_, download_;  // ops at the client's level
    std::string key_;
    DatagramEndpoint* endpoint_ = nullptr;  // the transfer's route (constructor)
    PeerAddress peer_;
    mutable Mutex mutex_;
    bool claimed_ = false;
    bool closed_ = false;
    bool finished_ = false;
    bool inFlight_ = false;
    bool expired_ = false;
    uint32_t activeMs_ = 0;
    size_t chunk_ = 0;
    size_t limit_ = 0;  // the largest part since one was refused for memory (smaller()), or 0
    uint64_t size_ = 0;
    uint64_t received_ = 0;
    std::vector<uint8_t> pending_;  // upload bytes from received_ on
    std::atomic<bool> noRoom_{false};  // the last envelope had no room (pump()'s builder)
};

}  // namespace

std::shared_ptr<FileRelay> RemoteNode::relayFile(Api& api, std::string_view rest, std::string_view base,
                                                 Access level) {
    if (!endpoint()) return nullptr;
    std::string remoteBase = remotePath_ == "/" ? std::string() : remotePath_;
    std::string localBase(base.substr(0, base.size() - rest.size()));
    return std::make_shared<RemoteFile>(api, *this, remotePathOf(rest), std::move(localBase), std::move(remoteBase),
                                        std::string(base), level);
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

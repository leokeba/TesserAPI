#include "tesser/detail/remote_routes.h"

#include "tesser/api.h"
#include "tesser/remote.h"

namespace tesser {

namespace {

// The order of tables without their own, whose Api has none.
std::vector<RouteKind>& sharedOrder() {
    static std::vector<RouteKind> order;
    return order;
}

}  // namespace

TransportKind RemoteRoute::kind() const { return endpoint ? endpoint->kind() : TransportKind::Local; }

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

namespace detail {

Mutex& remoteSharedMutex() {
    static Mutex m;
    return m;
}

namespace {
std::atomic<uint32_t> g_orderRevision{1};
}  // namespace

void routeOrderChanged() { g_orderRevision.fetch_add(1); }

// ---- the routes -----------------------------------------------------------------

bool RouteTable::add(DatagramEndpoint& endpoint, const PeerAddress& peer, NetInterface iface, bool& registered) {
    registered = false;
    MutexGuard guard(mutex_);
    for (const Slot& r : routes_) {
        if (r.route.endpoint != &endpoint) continue;
        if (r.route.peer == peer) return false;
        registered = true;
    }
    Slot slot;
    slot.route.endpoint = &endpoint;
    slot.route.peer = peer;
    slot.route.iface = iface;
    slot.added = nextRoute_++;
    routes_.push_back(slot);
    if (!lastEndpoint_) {
        lastEndpoint_ = &endpoint;
        lastPeer_ = peer;
    }
    return true;
}

bool RouteTable::replaceOffline(DatagramEndpoint& endpoint, const PeerAddress& peer, NetInterface iface) {
    MutexGuard guard(mutex_);
    Slot* oldest = nullptr;
    for (Slot& r : routes_) {
        if (r.route.endpoint != &endpoint || r.route.online) continue;
        if (!oldest || static_cast<int32_t>(r.route.heardMs - oldest->route.heardMs) < 0) oldest = &r;
    }
    if (!oldest) return false;
    if (lastEndpoint_ == &endpoint && lastPeer_ == oldest->route.peer) lastPeer_ = peer;
    oldest->route.peer = peer;
    oldest->route.heardMs = 0;
    oldest->route.rttMs = 0;
    oldest->route.iface = iface;
    return true;
}

bool RouteTable::has(const DatagramEndpoint* endpoint, const PeerAddress& peer) const {
    MutexGuard guard(mutex_);
    for (const Slot& r : routes_) {
        if (r.route.endpoint == endpoint && r.route.peer == peer) return true;
    }
    return false;
}

bool RouteTable::up(const DatagramEndpoint* endpoint, const PeerAddress& peer) const {
    MutexGuard guard(mutex_);
    for (const Slot& r : routes_) {
        if (r.route.endpoint == endpoint && r.route.peer == peer) return r.route.online;
    }
    return false;
}

bool RouteTable::down(const DatagramEndpoint* endpoint, const PeerAddress& peer) const {
    MutexGuard guard(mutex_);
    bool back = false;
    for (const Slot& r : routes_) back = back || (r.route.endpoint == endpoint && r.route.peer == peer && !r.route.online);
    return back;
}

void RouteTable::ensureOrder(Api* api) const {
    uint32_t rev = g_orderRevision.load();
    {
        MutexGuard guard(mutex_);
        if (effectiveRev_ == rev && effectiveApi_ == api) return;
        if (hasOrder_) {
            effective_ = order_;
            effectiveRev_ = rev;
            effectiveApi_ = api;
            return;
        }
    }
    // Inherited: read under the hooks' and the shared locks, not under ours.
    std::vector<RouteKind> order;
    if (api) order = api->remoteHooks().routeOrder();
    if (order.empty()) order = defaultOrder();
    MutexGuard guard(mutex_);
    if (hasOrder_) return;  // given its own meanwhile: the next call takes it
    effective_ = std::move(order);
    effectiveRev_ = rev;  // read first: a change since then is seen next time
    effectiveApi_ = api;
}

// The index of the route in use: the best ranked online one, or the best
// ranked one when none is online.
int RouteTable::activeLocked() const {
    const std::vector<RouteKind>& order = effective_;
    auto rank = [&](const Slot& r) {
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

std::vector<RouteTable::Route> RouteTable::list(Api* api) const {
    ensureOrder(api);
    std::vector<Slot> slots;
    std::vector<RouteKind> order;
    {
        MutexGuard guard(mutex_);
        slots = routes_;
        order = effective_;
    }
    auto rank = [&](const Slot& r) {
        size_t i = 0;
        while (i < order.size() && !order[i].covers(r.route.kind(), r.route.iface)) i++;
        return (uint64_t(i) << 32) | r.added;
    };
    // A handful of routes: insertion sort.
    for (size_t i = 1; i < slots.size(); i++) {
        for (size_t j = i; j > 0 && rank(slots[j]) < rank(slots[j - 1]); j--) std::swap(slots[j], slots[j - 1]);
    }
    std::vector<Route> out;
    for (const Slot& r : slots) out.push_back(r.route);
    return out;
}

bool RouteTable::active(Route& out, Api* api) const {
    ensureOrder(api);
    MutexGuard guard(mutex_);
    int i = activeLocked();
    if (i < 0) return false;
    out = routes_[static_cast<size_t>(i)].route;
    return true;
}

std::vector<DatagramEndpoint*> RouteTable::endpoints() const {
    std::vector<DatagramEndpoint*> endpoints;
    MutexGuard guard(mutex_);
    for (const Slot& r : routes_) {
        bool known = false;
        for (DatagramEndpoint* e : endpoints) known = known || e == r.route.endpoint;
        if (!known) endpoints.push_back(r.route.endpoint);
    }
    return endpoints;
}

uint32_t RouteTable::lastHeardMs() const {
    MutexGuard guard(mutex_);
    uint32_t last = 0;
    for (const Slot& r : routes_) {
        if (r.route.heardMs && (!last || static_cast<int32_t>(r.route.heardMs - last) > 0)) last = r.route.heardMs;
    }
    return last;
}

bool RouteTable::onlyThrough(const DatagramEndpoint* endpoint) const {
    MutexGuard guard(mutex_);
    for (const Slot& r : routes_) {
        if (r.route.endpoint != endpoint) return false;
    }
    return true;
}

size_t RouteTable::count(const DatagramEndpoint* endpoint) const {
    MutexGuard guard(mutex_);
    size_t n = 0;
    for (const Slot& r : routes_) n += r.route.endpoint == endpoint;
    return n;
}

// Under mutex_: online if any route is. True if that changed.
bool RouteTable::updateOnlineLocked() {
    bool any = false;
    for (const Slot& r : routes_) any = any || r.route.online;
    return online_.exchange(any) != any;
}

bool RouteTable::detach(const DatagramEndpoint* endpoint) {
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < routes_.size();) {
        if (routes_[i].route.endpoint == endpoint) {
            routes_.erase(routes_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
    if (lastEndpoint_ == endpoint) lastEndpoint_ = nullptr;
    if (routes_.empty()) {
        online_ = false;  // without a route, not a change anyone hears about
        return false;
    }
    return updateOnlineLocked();
}

bool RouteTable::setAll(bool online) {
    MutexGuard guard(mutex_);
    for (Slot& r : routes_) r.route.online = online;
    return updateOnlineLocked();
}

bool RouteTable::setOnline(const DatagramEndpoint* endpoint, const PeerAddress& peer, bool online, uint32_t nowMs,
                           bool back, NetInterface iface) {
    MutexGuard guard(mutex_);
    for (Slot& r : routes_) {
        if (r.route.endpoint != endpoint || r.route.peer != peer) continue;
        if (back) r.route.iface = iface;
        if (r.route.online && !online) {
            lastProbeMs_ = nowMs;  // the first probe waits probeMs
            probeWaitMs_ = probeMs_;
        }
        r.route.online = online;
        if (online && nowMs) r.route.heardMs = nowMs;
    }
    return updateOnlineLocked();
}

void RouteTable::roundTrip(const DatagramEndpoint* endpoint, const PeerAddress& peer, uint32_t rttMs) {
    if (!rttMs) rttMs = 1;
    MutexGuard guard(mutex_);
    for (Slot& r : routes_) {
        if (r.route.endpoint != endpoint || r.route.peer != peer) continue;
        // Smoothed like TCP's: an eighth of each new sample.
        r.route.rttMs = r.route.rttMs ? (r.route.rttMs * 7 + rttMs + 4) / 8 : rttMs;
    }
}

// ---- order ------------------------------------------------------------------------

void RouteTable::setOrder(std::vector<RouteKind> order) {
    {
        MutexGuard guard(mutex_);
        hasOrder_ = !order.empty();
        order_ = std::move(order);
    }
    routeOrderChanged();
}

std::vector<RouteKind> RouteTable::order() const {
    MutexGuard guard(mutex_);
    return order_;
}

void RouteTable::setDefaultOrder(std::vector<RouteKind> order) {
    {
        MutexGuard guard(remoteSharedMutex());
        sharedOrder() = std::move(order);
    }
    routeOrderChanged();
}

std::vector<RouteKind> RouteTable::defaultOrder() {
    MutexGuard guard(remoteSharedMutex());
    return sharedOrder();
}

// ---- what every route shares -----------------------------------------------------

PeerAddress RouteTable::device() const {
    MutexGuard guard(mutex_);
    return device_;
}

bool RouteTable::setDevice(const PeerAddress& device) {
    MutexGuard guard(mutex_);
    if (device_ == device) return false;
    device_ = device;
    return true;
}

std::string RouteTable::token() const {
    MutexGuard guard(mutex_);
    return token_;
}

std::vector<RouteTable::Route> RouteTable::setToken(std::string_view token) {
    std::vector<Route> routes;
    MutexGuard guard(mutex_);
    token_.assign(token.data(), token.size());
    for (const Slot& r : routes_) routes.push_back(r.route);
    return routes;
}

// ---- the node's tick ----------------------------------------------------------------

RouteTable::Observed RouteTable::observe(const Route& current, bool online) {
    Observed o;
    MutexGuard guard(mutex_);
    if (!lastEndpoint_) {
        lastEndpoint_ = current.endpoint;
        lastPeer_ = current.peer;
        lastOnline_ = online;
    }
    if (lastEndpoint_ != current.endpoint || lastPeer_ != current.peer) {
        o.switched = true;
        o.from.endpoint = lastEndpoint_;
        o.from.peer = lastPeer_;
        for (const Slot& r : routes_) {
            if (r.route.endpoint == o.from.endpoint && r.route.peer == o.from.peer) o.from.online = r.route.online;
        }
        lastEndpoint_ = current.endpoint;
        lastPeer_ = current.peer;
    }
    o.stateChanged = o.switched || lastOnline_ != online || announce_.exchange(false);
    lastOnline_ = online;
    return o;
}

bool RouteTable::unobserved(const Route& current, bool online) const {
    MutexGuard guard(mutex_);
    return lastEndpoint_ != current.endpoint || lastPeer_ != current.peer || lastOnline_ != online || announce_.load();
}

std::vector<RouteTable::Route> RouteTable::dueProbes(uint32_t nowMs) {
    std::vector<Route> probes;
    MutexGuard guard(mutex_);
    if (probeMs_ && nowMs - lastProbeMs_ >= (probeWaitMs_ ? probeWaitMs_ : probeMs_)) {
        for (const Slot& r : routes_) {
            if (!r.route.online) probes.push_back(r.route);
        }
        if (!probes.empty()) {
            lastProbeMs_ = nowMs;
            uint32_t base = probeWaitMs_ ? probeWaitMs_ : probeMs_;
            probeWaitMs_ = base >= kMaxProbeMs / 2 ? kMaxProbeMs : base * 2;
        }
    }
    return probes;
}

void RouteTable::nextProbe(Deadline& d) const {
    MutexGuard guard(mutex_);
    if (!probeMs_) return;
    for (const Slot& r : routes_) {
        if (r.route.online) continue;
        d.at(lastProbeMs_ + (probeWaitMs_ ? probeWaitMs_ : probeMs_));
        return;
    }
}

}  // namespace detail
}  // namespace tesser

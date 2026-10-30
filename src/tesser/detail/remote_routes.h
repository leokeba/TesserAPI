#pragma once

// A remote node's routes (docs/DESIGN.md section 14.3): the ways to reach
// its device, the order they are tried in, which one is in use, when to
// probe the offline ones, and the device and token every route shares. It
// calls nothing outside itself; the node talks to the endpoints with what
// it returns.

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "tesser/deadline.h"
#include "tesser/platform.h"
#include "tesser/route.h"

namespace tesser {

class Api;

namespace detail {

// Guards what remote nodes share process-wide: the default route order and
// RemoteNode's process-wide handlers (one mutex: each costs RAM on ESP32).
Mutex& remoteSharedMutex();
// Bumped whenever a route order changes (a node's, an Api's, the default),
// so tables recompute the order in effect only then.
void routeOrderChanged();

class RouteTable {
public:
    using Route = RemoteRoute;

    // `mutex`: the node's leaf lock, which the table shares with its other
    // parts; nothing is called while it is held.
    explicit RouteTable(Mutex& mutex) : mutex_(mutex) {}

    // ---- the routes ---------------------------------------------------------
    // Adds `peer` through `endpoint`, leaving by `iface`: false if it has it.
    // `registered`: a route through `endpoint` was there already.
    bool add(DatagramEndpoint& endpoint, const PeerAddress& peer, NetInterface iface, bool& registered);
    // Points the offline route through `endpoint` heard longest ago at
    // `peer`: false if every route there is online.
    bool replaceOffline(DatagramEndpoint& endpoint, const PeerAddress& peer, NetInterface iface);
    bool has(const DatagramEndpoint* endpoint, const PeerAddress& peer) const;
    // It has the route, and it is online (up()) or offline (down()).
    bool up(const DatagramEndpoint* endpoint, const PeerAddress& peer) const;
    bool down(const DatagramEndpoint* endpoint, const PeerAddress& peer) const;
    // Every route, in the order they are tried; `api` gives the order of a
    // table without its own.
    std::vector<Route> list(Api* api) const;
    // The route in use: the first online one in the order, or the first one
    // when none is online. False without any.
    bool active(Route& out, Api* api) const;
    // Each endpoint a route goes through, once.
    std::vector<DatagramEndpoint*> endpoints() const;
    uint32_t lastHeardMs() const;
    bool onlyThrough(const DatagramEndpoint* endpoint) const;
    size_t count(const DatagramEndpoint* endpoint) const;
    // Drops the routes through `endpoint`.
    // These return true when online() changed.
    bool detach(const DatagramEndpoint* endpoint);
    bool setAll(bool online);
    // `back`: the route was offline (down()) and comes back, leaving by `iface`.
    bool setOnline(const DatagramEndpoint* endpoint, const PeerAddress& peer, bool online, uint32_t nowMs, bool back,
                   NetInterface iface);
    void roundTrip(const DatagramEndpoint* endpoint, const PeerAddress& peer, uint32_t rttMs);
    // Any route online (true until the first one is lost).
    bool online() const { return online_.load(); }

    // ---- order ----------------------------------------------------------------
    // Empty: the inherited one (the Api's, else the default).
    void setOrder(std::vector<RouteKind> order);
    std::vector<RouteKind> order() const;
    static void setDefaultOrder(std::vector<RouteKind> order);
    static std::vector<RouteKind> defaultOrder();

    // ---- what every route shares ---------------------------------------------
    PeerAddress device() const;
    // False if it was the device already.
    bool setDevice(const PeerAddress& device);
    std::string token() const;
    // Sets it, and gives the routes to present it on.
    std::vector<Route> setToken(std::string_view token);

    // ---- the node's tick -------------------------------------------------------
    struct Observed {
        bool switched = false;      // the route in use changed: `from` was it
        bool stateChanged = false;  // switched, online changed, or announce()d
        Route from;
    };
    // Compares the route in use and the node's online state with the last
    // tick's.
    Observed observe(const Route& current, bool online);
    // Whether observe() would have something to say.
    bool unobserved(const Route& current, bool online) const;
    // The routes to probe now (offline ones, when their wait is over).
    std::vector<Route> dueProbes(uint32_t nowMs);
    // When dueProbes() next gives any.
    void nextProbe(Deadline& d) const;
    void setProbe(uint32_t ms) {
        probeMs_ = ms;
        probeWaitMs_ = ms;
    }
    // A route was added: the next tick reports a state change.
    void announce() { announce_ = true; }

private:
    struct Slot {
        Route route;
        uint32_t added;  // tie-break: routes of the same rank keep this order
    };
    // Makes effective_ the order in effect for a node in `api`, if it isn't:
    // the inherited order is read before taking mutex_.
    void ensureOrder(Api* api) const;
    int activeLocked() const;
    bool updateOnlineLocked();

    Mutex& mutex_;
    std::vector<Slot> routes_;
    std::vector<RouteKind> order_;
    bool hasOrder_ = false;
    // The order routes are ranked by, as of routeOrderChanged()'s count
    // effectiveRev_, for a node in effectiveApi_.
    mutable std::vector<RouteKind> effective_;
    mutable uint32_t effectiveRev_ = 0;
    mutable Api* effectiveApi_ = nullptr;
    uint32_t nextRoute_ = 0;
    PeerAddress device_;
    std::string token_;
    // The route observe() last saw in use, to notice a switch.
    DatagramEndpoint* lastEndpoint_ = nullptr;
    PeerAddress lastPeer_;
    bool lastOnline_ = true;
    std::atomic<bool> announce_{false};
    std::atomic<bool> online_{true};
    uint32_t probeMs_ = 5000;
    uint32_t probeWaitMs_ = 5000;  // until the next probe; doubles up to kMaxProbeMs
    uint32_t lastProbeMs_ = 0;
    static constexpr uint32_t kMaxProbeMs = 60000;
};

}  // namespace detail
}  // namespace tesser

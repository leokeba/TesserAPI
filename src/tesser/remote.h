#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/datagram.h"
#include "tesser/node.h"
#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class FileRelay;
struct Subscription;

// An entry of a route order (docs/DESIGN.md section 14.3): a transport, and
// optionally the interface it leaves by. A bare transport (TransportKind::Udp
// converts) matches its routes on any interface.
struct RouteKind {
    TransportKind transport = TransportKind::Local;
    NetInterface iface = NetInterface::Any;

    RouteKind() = default;
    RouteKind(TransportKind t, NetInterface i = NetInterface::Any) : transport(t), iface(i) {}  // NOLINT: implicit
    bool operator==(const RouteKind& o) const { return transport == o.transport && iface == o.iface; }
    bool operator!=(const RouteKind& o) const { return !(*this == o); }
    // This entry covers routes of `kind` leaving by `i`.
    bool covers(TransportKind kind, NetInterface i) const {
        return transport == kind && (iface == NetInterface::Any || iface == i);
    }
};
// "udp", "udp/ethernet".
std::string toString(const RouteKind& kind);
// "udp/ethernet, nowtp" (commas, spaces allowed): false, with `out`
// untouched, if an entry isn't a transport or transport/interface.
bool parseRouteOrder(std::string_view text, std::vector<RouteKind>& out);
// The other way round: "udp/ethernet, nowtp".
std::string formatRouteOrder(const std::vector<RouteKind>& order);

// Another node's tree grafted into this one (docs/DESIGN.md section 14):
// requests below it are forwarded over a DatagramEndpoint (NowTP, ...), and
// the remote's answer comes back as a deferred reply. A gateway uses it to
// serve ESP-NOW nodes over HTTP.
//
// A node can have several routes to its device, one per endpoint that
// reaches it (UDP and NowTP, say): requests go by the first online one in
// the route order (docs/DESIGN.md section 14.3).
//
// Created with Object::remote(); lives as long as the tree. Either may be
// destroyed first: a destroyed endpoint detaches its routes, and a node left
// without one answers "no endpoint".
class RemoteNode : public Annotated<RemoteNode> {
public:
    // A way to reach the device.
    struct Route {
        DatagramEndpoint* endpoint = nullptr;
        PeerAddress peer;
        bool online = true;
        uint32_t heardMs = 0;  // when a message last came this way (endpoint clock; 0: never)
        uint32_t rttMs = 0;    // smoothed round trip of the calls made this way (0: none yet)
        // The local interface it leaves by (UDP; Any when unknown or for
        // NowTP), as the endpoint saw it when the route came online.
        NetInterface iface = NetInterface::Any;
        TransportKind kind() const;
        // "nowtp", "udp/ethernet" (or "udp" when the interface is unknown).
        std::string name() const { return toString(RouteKind(kind(), iface)); }
    };
    // See onStateChange().
    using StateHandler = std::function<void(RemoteNode& node)>;
    // A streamed set that failed: `path` below the node, the status, and the
    // remote's error body (null when it didn't answer).
    using StreamErrorHandler = std::function<void(const std::string& path, Status status, JsonVariantConst body)>;

    struct StreamStats {
        uint32_t queued = 0;  // sets accepted
        uint32_t merged = 0;  // sets merged into one still queued
        uint32_t sent = 0;    // requests sent, resends included
        uint32_t failed = 0;  // sets dropped: error reply, peer offline, out of attempts
    };

    RemoteNode(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer, std::string remotePath);
    // Same, owning a copy of its name (nodes mounted at run time).
    RemoteNode(std::string name, DatagramEndpoint& endpoint, const PeerAddress& peer, std::string remotePath);
    ~RemoteNode() override;

    // Timeout for forwarded requests (default: the endpoint's).
    RemoteNode& timeout(uint32_t ms) {
        timeoutMs_ = ms;
        return *this;
    }
    // Keeps a local copy of the remote tree through a subscription, so reads
    // of a parent include it and local subscribers get its changes and
    // events. `intervalMs` is the subscription's interval; the subscription
    // is renewed every `refreshMs` in case the remote node restarted. The
    // copy is what a client at `level` reads (docs/DESIGN.md section 14),
    // and only clients at `level` or above get it; admin: whatever the
    // remote gives this node.
    RemoteNode& mirror(uint32_t intervalMs = 500, uint32_t refreshMs = 30000, Access level = Access::Public);

    // Asks offline routes whether the device answers again, with a get at
    // depth 0: a device that doesn't advertise (a declared address on
    // another subnet) comes back that way. The first probe goes `ms` after
    // the route is lost (default 5000; 0: never), and the wait doubles after
    // each one, up to a minute, so a device that is gone costs little.
    RemoteNode& probe(uint32_t ms) {
        probeMs_ = ms;
        probeWaitMs_ = ms;
        return *this;
    }

    // ---- routes ---------------------------------------------------------
    // Another way to reach the device (no effect if the node has it).
    // mountPeers() adds the routes of a device announced on several
    // endpoints.
    void addRoute(DatagramEndpoint& endpoint, const PeerAddress& peer);
    bool hasRoute(const DatagramEndpoint* endpoint, const PeerAddress& peer) const;
    // Every route, in the order they are tried.
    std::vector<Route> routes() const;
    // The route requests use: the first online one in the order, or the
    // first one when none is online. False without any.
    bool route(Route& out) const;
    // The order routes are tried in, by transport; routes of a transport the
    // list doesn't name come after, in the order they were added. Empty:
    // the default order. Takes effect at once.
    RemoteNode& routeOrder(std::vector<RouteKind> order);
    std::vector<RouteKind> routeOrder() const;
    // The order of nodes without their own (initially empty: routes in the
    // order they were added). Any task.
    static void setDefaultRouteOrder(std::vector<RouteKind> order);
    static std::vector<RouteKind> defaultRouteOrder();
    // Called, from an endpoint's process() and without the API lock, when a
    // node is created or gets a route, and when its online state or route in
    // use changes. One for every node.
    static void onStateChange(StateHandler fn);
    // The remote node under `parent` mounted for `device`, or null. Under
    // the API lock.
    static RemoteNode* byDevice(const Object& parent, const PeerAddress& device);

    // The active route's endpoint and address (null and empty without one).
    DatagramEndpoint* endpoint() const;
    PeerAddress peer() const;
    const std::string& remotePath() const { return remotePath_; }
    uint32_t timeoutMs() const { return timeoutMs_; }
    bool mirrored() const { return mirror_; }
    // The level the mirrored copy is read at (see mirror()).
    Access mirrorLevel() const { return mirrorLevel_; }
    bool hasCopy() const;

    // Whether the device is reachable, as far as the endpoints know: false
    // once every route is lost (DatagramEndpoint::forgetPeer()), true again
    // when one is heard from. Shown in the schema; a change marks the node
    // changed, so subscribers of its parent hear about it.
    bool online() const { return online_.load(); }
    // Sets every route's state (tests, applications that know better).
    void setOnline(bool online);
    // One route's state; called by its endpoint.
    void setRouteOnline(const DatagramEndpoint* endpoint, const PeerAddress& peer, bool online, uint32_t nowMs);
    // A call on a route got its reply after `rttMs`; called by its endpoint.
    void routeRoundTrip(const DatagramEndpoint* endpoint, const PeerAddress& peer, uint32_t rttMs);
    // The schema hash the peer advertises (0: unknown), shown in the schema
    // so clients can cache the remote's schema under it.
    uint32_t advertisedSchema() const { return advertisedSchema_.load(); }
    void setAdvertisedSchema(uint32_t hash);
    // The protocol version the peer advertises (docs/DESIGN.md section
    // 10.1.2; 1: one that predates versions), 0 while unknown. Shown in the
    // schema.
    uint8_t protocol() const { return protocol_.load(); }
    void setProtocol(uint8_t version);
    // The device a mounted peer is (its MAC; empty when unknown), so that a
    // parent mounts each device once, whatever the transport
    // (docs/DESIGN.md section 14.1). Set by DatagramEndpoint::mountPeers().
    PeerAddress device() const;
    void setDevice(const PeerAddress& device);

    // ---- credentials --------------------------------------------------------
    // A token the device's token check accepts (docs/DESIGN.md section
    // 13.3), presented on every route before requests, and again whenever
    // the device may have forgotten it. Empty: none. Any task.
    void setToken(std::string_view token);
    std::string token() const;
    // The level the device granted the token on the route in use (public
    // until it answers).
    Access access() const;
    // Called, in an endpoint's process() and without the API lock, with
    // every answer to a node's token: Status::Ok and the level granted
    // (public: refused, time to get a new one), or the error. One for every
    // node.
    using AccessHandler = std::function<void(RemoteNode& node, Status status, Access level)>;
    static void onAccess(AccessHandler fn);
    // Called by an endpoint.
    void tokenAnswered(Status status, Access level);

    // ---- streamed sets ----------------------------------------------------
    // Writes `bodyJson` to `path` below this node without waiting, for state
    // that changes fast (docs/DESIGN.md section 14.3). At most one streamed
    // set per node is in flight, so they are applied in the order they were
    // made, whatever the route; the others wait in a queue, where a set to
    // a path already queued is merged into it (objects merge, anything else
    // replaces). Streams converge on the latest state: they don't replay
    // every set. Ok: queued. Timeout: the node is offline. Busy: the queue
    // is full. BadRequest: `bodyJson` isn't JSON. Any task, under the API
    // lock or not.
    Status stream(std::string_view path, std::string_view bodyJson);
    // Sets that may wait (default 8).
    RemoteNode& streamLimit(size_t entries) {
        streamLimit_ = entries;
        return *this;
    }
    // Called, in an endpoint's process() and without the API lock, for each
    // streamed set that is dropped.
    void onStreamError(StreamErrorHandler fn);
    StreamStats streamStats() const;
    // Sets queued or in flight.
    size_t streaming() const;

    // The mirrored copy (objects below `depth` levels as {}), or null.
    // Virtual like forward(): keeps the remote code out of applications
    // without remote nodes.
    virtual void writeCopy(JsonWriter& w, int depth = 255) const;

    // Forwards a request whose path continues below this node (`rest`,
    // "" for the node itself); `base` is the full local path. Below admin,
    // the client's level goes with it (docs/DESIGN.md section 13.1). Called
    // by the request handler, under the API lock.
    virtual void forward(Api& api, const Request& request, std::string_view rest, std::string_view base,
                         Reply& reply);

    // Subscribes a local client below this node (`rest`, "" for the node
    // itself when it isn't mirrored for the client). Clients of the same
    // path and level share one upstream subscription; see docs/DESIGN.md
    // section 14. Called by the request handler, under the API lock.
    virtual void subscribe(Api& api, const Request& request, std::string_view rest, std::string_view base,
                           Reply& reply);
    // A file transfer relayed to the peer (docs/DESIGN.md section 14), not
    // yet claimed: `rest` is the path below this node, `base` the full local
    // path, `level` the client's. Null without an endpoint. Virtual like
    // forward().
    virtual std::shared_ptr<FileRelay> relayFile(Api& api, std::string_view rest, std::string_view base,
                                                 Access level = Access::Admin);

    // The last local subscription to `localPath` at `level` is gone. Called
    // by the API, under its lock.
    virtual void released(std::string_view localPath, Access level);
    // One of several local subscriptions to `localPath` at `level` is gone:
    // the upstream one may narrow. Called by the API, under its lock.
    virtual void narrowed(std::string_view localPath, Access level);
    // What the upstream subscription to `localPath` for clients at `level`
    // asks the remote for (keys, exclude, depth; tests, diagnostics): false
    // without one.
    bool upstreamSpec(std::string_view localPath, std::string& keys, std::string& exclude, int& depth,
                      Access level = Access::Admin) const;
    // Upstream subscriptions currently held (tests, diagnostics).
    size_t upstreamCount() const;

    // Called by an endpoint, from its process(), and when it is destroyed.
    void tick(const DatagramEndpoint& by, uint32_t nowMs);
    void detach(const DatagramEndpoint* endpoint);
    bool handleNotification(const DatagramEndpoint& by, const PeerAddress& from, JsonObjectConst envelope);

    // What an upstream subscription asks for: the union of what its local
    // subscribers need (docs/DESIGN.md section 14).
    struct Spec {
        std::string keys;     // sorted, comma-separated; empty: every child
        std::string exclude;  // likewise, with no keys
        int depth = -1;       // -1: unlimited
        bool operator==(const Spec& o) const { return keys == o.keys && exclude == o.exclude && depth == o.depth; }
        bool operator!=(const Spec& o) const { return !(*this == o); }
    };

private:
    // One subscription to the remote node, shared by local subscribers at
    // one level.
    struct Upstream {
        std::string rest;  // below this node; "" for the node itself
        Access level = Access::Admin;  // its subscribers': the ceiling it asks for
        JsonDocument copy;  // the remote's state, kept with its notifications
        bool ready = false;
        bool subscribing = false;
        bool stale = false;  // a renewal failed: retry soon
        uint32_t interval = 0;
        uint32_t lastSubscribeMs = 0;
        Spec spec;      // what the subscribers need now
        Spec sent;      // what the request in flight (or the last) asked for
        Spec copySpec;  // what `copy` holds
    };
    // The union of the needs of `local`'s subscribers at `level`, plus `extra`'s.
    Spec wanted(const std::string& local, Access level, const Subscription* extra);
    struct RouteSlot {
        Route route;
        uint32_t added;  // tie-break: routes of the same rank keep this order
    };
    struct Pending {
        std::string path;  // below the node
        JsonDocument body;
        uint8_t attempts = 0;
    };

    bool localPath(std::string& out);
    Upstream* findUpstream(std::string_view rest, Access level);
    std::string remotePathOf(std::string_view rest) const;
    bool sendUpstream(const std::string& rest, Access level, uint32_t interval, bool renew);
    void upstreamReply(const std::string& rest, Access level, Status status, JsonVariantConst body);
    bool relayChange(std::string_view rest, Access level, JsonVariantConst patch);
    // Under routesMutex_.
    int activeLocked() const;
    void updateOnlineLocked();
    void rerouted(const Route& from, uint32_t nowMs);
    void pump();
    static void requeue(std::vector<Pending>& queue, Pending p);
    void streamDone(Status s, JsonVariantConst body);
    uint32_t streamTimeoutMs() const;

    std::string remotePath_;
    uint32_t timeoutMs_ = 0;

    mutable Mutex routesMutex_;  // a leaf: nothing is called while it is held
    std::vector<RouteSlot> routes_;
    std::vector<RouteKind> order_;
    bool hasOrder_ = false;
    uint32_t nextRoute_ = 0;
    PeerAddress device_;
    std::string token_;
    // The route tick() last saw in use, to notice a switch.
    DatagramEndpoint* lastEndpoint_ = nullptr;
    PeerAddress lastPeer_;
    bool lastOnline_ = true;
    std::atomic<bool> announce_{false};
    uint32_t probeMs_ = 5000;
    uint32_t probeWaitMs_ = 5000;  // until the next probe; doubles up to kMaxProbeMs
    uint32_t lastProbeMs_ = 0;
    static constexpr uint32_t kMaxProbeMs = 60000;  // mounted, or a route added: tick() tells onStateChange()

    bool mirror_ = false;
    Access mirrorLevel_ = Access::Public;
    uint32_t mirrorIntervalMs_ = 500;
    uint32_t refreshMs_ = 30000;
    uint32_t lastSubscribeMs_ = 0;
    std::atomic<bool> subscribing_{false};
    bool subscribedOnce_ = false;
    mutable Mutex mutex_;
    JsonDocument copy_;
    bool hasCopy_ = false;
    Api* api_ = nullptr;
    std::string localPath_;
    std::vector<std::unique_ptr<Upstream>> upstreams_;  // guarded by mutex_
    std::atomic<bool> online_{true};
    std::atomic<uint32_t> advertisedSchema_{0};
    std::atomic<uint8_t> protocol_{0};
    std::string ownedName_;

    // Streamed sets, guarded by streamMutex_ (a leaf like routesMutex_).
    mutable Mutex streamMutex_;
    std::vector<Pending> queue_;
    Pending flying_;
    bool inFlight_ = false;
    size_t streamLimit_ = 8;
    StreamStats streamStats_;
    StreamErrorHandler onStreamError_;
};

}  // namespace tesser

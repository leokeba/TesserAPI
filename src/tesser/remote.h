#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/datagram.h"
#include "tesser/detail/remote_routes.h"
#include "tesser/detail/remote_stream.h"
#include "tesser/detail/remote_upstream.h"
#include "tesser/node.h"
#include "tesser/platform.h"
#include "tesser/request.h"
#include "tesser/route.h"

namespace tesser {

class FileRelay;
struct Subscription;

// Another node's tree grafted into this one (docs/DESIGN.md section 14):
// requests below it are forwarded over a DatagramEndpoint (NowTP, ...), and
// the remote's answer comes back as a deferred reply. A gateway uses it to
// serve ESP-NOW nodes over HTTP.
//
// A node can have several routes to its device, one per endpoint that
// reaches it (UDP and NowTP, say): requests go by the first online one in
// the route order (docs/DESIGN.md section 14.3).
//
// Created with Object::remote(); lives as long as the tree (one mounted for
// an announced peer may be evicted: DatagramEndpoint::Options::maxMounted).
// Either may be destroyed first: a destroyed endpoint detaches its routes, and a node left
// without one answers "no endpoint". Destroy a node outside the API lock, and not from a
// call into it: its destructor waits for another task's call into it to end, and that
// task may be waiting for the lock (docs/DESIGN.md section 9). Destroy an endpoint, or a
// node, only while no relayed file transfer is open through it (relays()).
// What an application hears from the remote nodes of one Api, and the
// order their routes are tried in (docs/DESIGN.md section 14.3): each Api
// has its own (Api::remoteHooks()), so two applications in one program
// don't replace each other's, and each holds several handlers. A node uses
// its Api's once it knows it (from its first tick), the process-wide ones
// (RemoteNode's statics) as well. Thread-safe.
class RemoteHooks {
public:
    using StateHandler = std::function<void(RemoteNode& node)>;
    using AccessHandler = std::function<void(RemoteNode& node, Status status, Access level)>;
    using Id = uint32_t;
    // As RemoteNode::onStateChange() and onAccess(), for this Api's nodes;
    // several of each. The id removes it (remove()).
    Id onStateChange(StateHandler fn);
    Id onAccess(AccessHandler fn);
    void remove(Id id);
    // The order of this Api's nodes without their own (empty, the default:
    // RemoteNode::setDefaultRouteOrder()'s).
    void setRouteOrder(std::vector<RouteKind> order);
    std::vector<RouteKind> routeOrder() const;

    // For RemoteNode: calls the handlers, without the hooks' lock.
    void stateChanged(RemoteNode& node) const;
    void accessAnswered(RemoteNode& node, Status status, Access level) const;

private:
    mutable Mutex mutex_;  // a leaf: handlers are copied out, then called
    Id next_ = 1;
    std::vector<std::pair<Id, StateHandler>> state_;
    std::vector<std::pair<Id, AccessHandler>> access_;
    std::vector<RouteKind> order_;
};

class RemoteNode : public Annotated<RemoteNode> {
public:
    // A way to reach the device: its endpoint and address, whether it is
    // online, when it was last heard, its round trip and interface.
    using Route = RemoteRoute;
    // See onStateChange().
    using StateHandler = std::function<void(RemoteNode& node)>;
    // A streamed set that failed: `path` below the node, the status, and the
    // remote's error body (null when it didn't answer).
    using StreamErrorHandler = detail::SetStream::ErrorHandler;
    // queued: sets accepted; merged: sets merged into one still queued;
    // sent: requests sent, resends included; failed: sets dropped (error
    // reply, peer offline, out of attempts).
    using StreamStats = detail::SetStream::Stats;

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
        routes_.setProbe(ms);
        return *this;
    }

    // ---- routes ---------------------------------------------------------
    // Another way to reach the device (no effect if the node has it).
    // mountPeers() adds the routes of a device announced on several
    // endpoints.
    void addRoute(DatagramEndpoint& endpoint, const PeerAddress& peer);
    bool hasRoute(const DatagramEndpoint* endpoint, const PeerAddress& peer) const;
    // The node has that route, and it is online.
    bool routeUp(const DatagramEndpoint* endpoint, const PeerAddress& peer) const;
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
    // The order of nodes without their own and whose Api has none
    // (RemoteHooks::setRouteOrder(); initially empty: routes in the order
    // they were added). Process-wide: prefer the Api's. Any task.
    static void setDefaultRouteOrder(std::vector<RouteKind> order);
    static std::vector<RouteKind> defaultRouteOrder();
    // Called, from an endpoint's process() and without the API lock, when a
    // node is created or gets a route, and when its online state or route in
    // use changes. Process-wide, one for every node of every Api, beside the
    // Api's own (Api::remoteHooks()), which an application should use: a
    // second caller replaces this one.
    static void onStateChange(StateHandler fn);
    // The Api it is mounted in, once known (null before its first tick or
    // request).
    Api* api() const;
    // The remote node under `parent` mounted for `device`, or null. Under
    // the API lock.
    static RemoteNode* byDevice(const Object& parent, const PeerAddress& device);

    // ---- discovered peers (DatagramEndpoint::mountPeers()) ----------------
    // Mounted by an endpoint for a peer that announced itself, rather than
    // declared by the application: such a node may be evicted when the
    // endpoint has mounted as many as it takes (docs/DESIGN.md section 14.1).
    bool autoMounted() const { return autoMounted_; }
    void setAutoMounted() { autoMounted_ = true; }
    // Relayed file transfers that hold the node (RemoteNode::relayFile()):
    // it isn't evicted while one does. Any task.
    void relayHeld(bool held) { held ? relays_.fetch_add(1) : relays_.fetch_sub(1); }
    int relays() const { return relays_.load(); }
    // When the device was last heard on any route (0: never), and whether
    // every route goes through `endpoint`.
    uint32_t lastHeardMs() const;
    bool onlyThrough(const DatagramEndpoint* endpoint) const;
    // Routes through `endpoint`.
    size_t routeCount(const DatagramEndpoint* endpoint) const;
    // Points the offline route through `endpoint` heard longest ago at
    // `peer` instead: a node's routes stay bounded however many addresses
    // announce its device. False if every route there is online.
    bool replaceOfflineRoute(DatagramEndpoint& endpoint, const PeerAddress& peer);

    // The active route's endpoint and address (null and empty without one).
    DatagramEndpoint* endpoint() const;
    PeerAddress peer() const;
    const std::string& remotePath() const { return remotePath_; }
    uint32_t timeoutMs() const { return timeoutMs_; }
    bool mirrored() const { return upstreamIf() && upstreamIf()->mirrored(); }
    // The level the mirrored copy is read at (see mirror()).
    Access mirrorLevel() const { return upstreamIf() ? upstreamIf()->mirrorLevel() : Access::Public; }
    bool hasCopy() const;

    // Whether the device is reachable, as far as the endpoints know: false
    // once every route is lost (DatagramEndpoint::forgetPeer()), true again
    // when one is heard from. Shown in the schema; a change marks the node
    // changed, so subscribers of its parent hear about it.
    bool online() const { return routes_.online(); }
    // Sets every route's state (tests, applications that know better).
    void setOnline(bool online);
    // One route's state; called by its endpoint.
    void setRouteOnline(const DatagramEndpoint* endpoint, const PeerAddress& peer, bool online, uint32_t nowMs);
    // A call on a route got its reply after `rttMs`; called by its endpoint.
    void routeRoundTrip(const DatagramEndpoint* endpoint, const PeerAddress& peer, uint32_t rttMs);
    // The schema hash the peer advertises (0: unknown), shown in the schema
    // so clients can cache the remote's schema under it.
    uint32_t advertisedSchema() const { return advertisedSchema_.load(); }
    // Whether the peer's node at `remotePath` is a file node, as its schema
    // said while it advertised its current hash; false when not known (no
    // hash, or not asked since). learnedFile() records an answer. A few
    // paths are kept, the oldest going first (docs/DESIGN.md section 14).
    bool knownFile(std::string_view remotePath, bool& isFile) const;
    void learnedFile(std::string_view remotePath, bool isFile);
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
    // (public: refused, time to get a new one), or the error. Process-wide,
    // as onStateChange(): prefer the Api's (RemoteHooks::onAccess()).
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
        sets().setLimit(entries);
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
    using Spec = UpstreamSpec;

private:
    friend class detail::UpstreamCache;  // sends through the node, notifies its Api

    // Finds its Api, and its path there, if not known yet: false while it
    // isn't in a tree.
    bool localPath(std::string& out);
    std::string remotePathOf(std::string_view rest) const;
    // Sends the next streamed set if none is in flight.
    void pump();
    uint32_t streamTimeoutMs() const;

    std::string remotePath_;
    uint32_t timeoutMs_ = 0;

    // One leaf lock for the routes, the known paths and the streamed sets,
    // which never call anything while holding it (a FreeRTOS mutex costs
    // about 90 bytes, so not one each).
    mutable Mutex leaf_{kRankLeaf, "remote node"};
    detail::RouteTable routes_{leaf_};  // routes, order, device and token
    // knownFile(): paths the peer's schema described, under the hash it
    // advertised then.
    struct KnownPath {
        std::string path;
        uint32_t schemaHash;
        bool file;
    };
    static constexpr size_t kKnownPaths = 8;
    std::vector<KnownPath> knownPaths_;  // under leaf_
    bool autoMounted_ = false;
    std::atomic<int> relays_{0};

    // The mirror and shared subscriptions, made by the first mirror() or
    // subscription and kept until the node goes; null until then.
    std::atomic<detail::UpstreamCache*> upstream_{nullptr};
    detail::UpstreamCache& upstream();
    detail::UpstreamCache* upstreamIf() const { return upstream_.load(std::memory_order_acquire); }
    std::atomic<Api*> api_{nullptr};  // the Api it is mounted in, once known (localPath())
    std::atomic<uint32_t> apiLookupMs_{0};  // tick(): when api_ was last looked for
    std::string localPath_;
    std::atomic<uint32_t> advertisedSchema_{0};
    std::atomic<uint8_t> protocol_{0};
    std::string ownedName_;

    // Streamed sets, made by the first stream() or onStreamError(); null
    // until then.
    std::atomic<detail::SetStream*> stream_{nullptr};
    detail::SetStream& sets();
    detail::SetStream* setsIf() const { return stream_.load(std::memory_order_acquire); }
};

}  // namespace tesser

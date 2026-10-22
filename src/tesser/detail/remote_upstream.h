#pragma once

// What a remote node keeps of its peer's state (docs/DESIGN.md section 14):
// the mirrored copy, and the upstream subscriptions its local subscribers
// share, one per path and level, each with its copy. Its methods take the
// node they belong to, which they send through and whose Api they notify:
// the unit holds no pointer back, so a node is no larger for it.

#include <stdint.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/budget.h"
#include "tesser/platform.h"
#include "tesser/request.h"
#include "tesser/route.h"

namespace tesser {

class Api;
class JsonWriter;
class RemoteNode;
struct Subscription;

// What an upstream subscription asks for: the union of what its local
// subscribers need (RemoteNode::Spec).
struct UpstreamSpec {
    std::string keys;     // sorted, comma-separated; empty: every child
    std::string exclude;  // likewise, with no keys
    int depth = -1;       // -1: unlimited
    bool operator==(const UpstreamSpec& o) const {
        return keys == o.keys && exclude == o.exclude && depth == o.depth;
    }
    bool operator!=(const UpstreamSpec& o) const { return !(*this == o); }
};

namespace detail {

// What a subscription needs (keys and exclude as lists, sorted).
UpstreamSpec specOf(const Subscription& sub);
// Everything any of `specs` needs: the deepest depth; the keys of all when
// all have keys, else every child but those all of them leave out.
UpstreamSpec unionOf(const std::vector<UpstreamSpec>& specs);
// `have` holds everything `need` asks for.
bool covers(const UpstreamSpec& have, const UpstreamSpec& need);

class UpstreamCache {
public:
    // ---- the mirror (RemoteNode::mirror()) -----------------------------------
    void mirror(uint32_t intervalMs, uint32_t refreshMs, Access level);
    bool mirrored() const { return mirror_; }
    Access mirrorLevel() const { return mirrorLevel_; }
    bool hasCopy() const;
    // The copy, objects below `depth` levels as {}; null without one.
    void writeCopy(JsonWriter& w, int depth) const;

    // ---- shared subscriptions ---------------------------------------------------
    // `rest` is the path below the node in each: see RemoteNode's methods of
    // the same names.
    void subscribe(RemoteNode& node, Api& api, const Request& req, std::string_view rest, std::string_view base,
                   Reply& reply);
    void narrowed(RemoteNode& node, const std::string& rest, std::string_view localPath, Access level);
    void released(RemoteNode& node, const std::string& rest, Access level);
    // What the subscription below `rest` at `level` asked the remote for.
    bool sent(std::string_view rest, Access level, UpstreamSpec& out) const;
    size_t count() const;

    // ---- from the node's tick and its peer's notifications ----------------------
    // Renews the upstream subscriptions due.
    void renew(RemoteNode& node, uint32_t nowMs);
    // Subscribes the mirror when due, by `current`, the route in use.
    void refreshMirror(RemoteNode& node, const RemoteRoute& current, uint32_t nowMs);
    // The route in use changed (`from` was it): subscriptions move to the new
    // one.
    void rerouted(RemoteNode& node, const RemoteRoute& from, uint32_t nowMs);
    // A "change" or "event" (`op`) from the peer for `rest` below the node,
    // from a subscription at `level`: false if it isn't for the node.
    bool notified(RemoteNode& node, std::string_view rest, Access level, std::string_view op,
                  JsonObjectConst envelope);

private:
    // One subscription to the remote node, shared by local subscribers at
    // one level.
    struct Upstream {
        std::string rest;  // below the node; "" for the node itself
        Access level = Access::Admin;  // its subscribers': the ceiling it asks for
        JsonDocument copy{jsonAllocator(Pool::Large)};  // the remote's state, kept with its notifications
        bool ready = false;
        bool subscribing = false;
        bool stale = false;  // a renewal failed: retry soon
        uint32_t interval = 0;
        uint32_t lastSubscribeMs = 0;
        UpstreamSpec spec;      // what the subscribers need now
        UpstreamSpec sent;      // what the request in flight (or the last) asked for
        UpstreamSpec copySpec;  // what `copy` holds
    };
    // Under mutex_.
    Upstream* find(std::string_view rest, Access level);
    // The union of the needs of `local`'s subscribers at `level`, plus `extra`'s.
    UpstreamSpec wanted(RemoteNode& node, const std::string& local, Access level, const Subscription* extra);
    bool send(RemoteNode& node, const std::string& rest, Access level, uint32_t interval, bool renew);
    void answered(RemoteNode& node, const std::string& rest, Access level, Status status, JsonVariantConst body);
    bool relayChange(RemoteNode& node, std::string_view rest, Access level, JsonVariantConst patch);

    bool mirror_ = false;
    Access mirrorLevel_ = Access::Public;
    uint32_t mirrorIntervalMs_ = 500;
    uint32_t refreshMs_ = 30000;
    uint32_t lastSubscribeMs_ = 0;
    std::atomic<bool> subscribing_{false};
    bool subscribedOnce_ = false;
    mutable Mutex mutex_{kRankRemote, "remote"};  // after the API lock
    JsonDocument copy_{jsonAllocator(Pool::Large)};  // the mirror's: PSRAM when the chip has it
    bool hasCopy_ = false;
    std::vector<std::unique_ptr<Upstream>> upstreams_;  // guarded by mutex_
};

}  // namespace detail
}  // namespace tesser

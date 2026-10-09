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

struct Subscription;

// Another node's tree grafted into this one (docs/DESIGN.md section 14):
// requests below it are forwarded over a DatagramEndpoint (NowTP, ...), and
// the remote's answer comes back as a deferred reply. A gateway uses it to
// serve ESP-NOW nodes over HTTP.
//
// Created with Object::remote(); lives as long as the tree. Either may be
// destroyed first: a destroyed endpoint detaches its remote nodes, which then
// answer "no endpoint".
class RemoteNode : public Annotated<RemoteNode> {
public:
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
    // is renewed every `refreshMs` in case the remote node restarted.
    RemoteNode& mirror(uint32_t intervalMs = 500, uint32_t refreshMs = 30000);

    DatagramEndpoint* endpoint() const { return endpoint_; }
    const PeerAddress& peer() const { return peer_; }
    const std::string& remotePath() const { return remotePath_; }
    uint32_t timeoutMs() const { return timeoutMs_; }
    bool mirrored() const { return mirror_; }
    bool hasCopy() const;

    // Whether the peer is reachable, as far as the endpoint knows: false
    // once it is lost (DatagramEndpoint::forgetPeer()), true again when it
    // is heard from. Shown in the schema; a change marks the node changed,
    // so subscribers of its parent hear about it.
    bool online() const { return online_.load(); }
    void setOnline(bool online);
    // The schema hash the peer advertises (0: unknown), shown in the schema
    // so clients can cache the remote's schema under it.
    uint32_t advertisedSchema() const { return advertisedSchema_.load(); }
    void setAdvertisedSchema(uint32_t hash);

    // The mirrored copy (objects below `depth` levels as {}), or null.
    // Virtual like forward(): keeps the remote code out of applications
    // without remote nodes.
    virtual void writeCopy(JsonWriter& w, int depth = 255) const;

    // Forwards a request whose path continues below this node (`rest`,
    // "" for the node itself); `base` is the full local path. Called by the
    // request handler, under the API lock.
    virtual void forward(Api& api, const Request& request, std::string_view rest, std::string_view base,
                         Reply& reply);

    // Subscribes a local client below this node (`rest`, "" for the node
    // itself when it isn't mirrored). Clients of the same path share one
    // upstream subscription; see docs/DESIGN.md section 14. Called by the
    // request handler, under the API lock.
    virtual void subscribe(Api& api, const Request& request, std::string_view rest, std::string_view base,
                           Reply& reply);
    // The last local subscription to `localPath` is gone. Called by the API,
    // under its lock.
    virtual void released(std::string_view localPath);
    // Upstream subscriptions currently held (tests, diagnostics).
    size_t upstreamCount() const;

    // Called by the endpoint, from process(), and when it is destroyed.
    void tick(uint32_t nowMs);
    void detach() { endpoint_ = nullptr; }
    bool handleNotification(const PeerAddress& from, JsonObjectConst envelope);

private:
    // One subscription to the remote node, shared by local subscribers.
    struct Upstream {
        std::string rest;  // below this node; "" for the node itself
        JsonDocument copy;  // the remote's state, kept with its notifications
        bool ready = false;
        bool subscribing = false;
        bool stale = false;  // a renewal failed: retry soon
        uint32_t interval = 0;
        uint32_t lastSubscribeMs = 0;
    };

    bool localPath(std::string& out);
    Upstream* findUpstream(std::string_view rest);
    std::string remotePathOf(std::string_view rest) const;
    bool sendUpstream(const std::string& rest, uint32_t interval, bool renew);
    void upstreamReply(const std::string& rest, Status status, JsonVariantConst body);
    bool relayChange(std::string_view rest, JsonVariantConst patch);

    DatagramEndpoint* endpoint_;
    PeerAddress peer_;
    std::string remotePath_;
    uint32_t timeoutMs_ = 0;

    bool mirror_ = false;
    uint32_t mirrorIntervalMs_ = 500;
    uint32_t refreshMs_ = 30000;
    uint32_t lastSubscribeMs_ = 0;
    bool subscribing_ = false;
    bool subscribedOnce_ = false;
    mutable Mutex mutex_;
    JsonDocument copy_;
    bool hasCopy_ = false;
    Api* api_ = nullptr;
    std::string localPath_;
    std::vector<std::unique_ptr<Upstream>> upstreams_;  // guarded by mutex_
    std::atomic<bool> online_{true};
    std::atomic<uint32_t> advertisedSchema_{0};
    std::string ownedName_;
};

}  // namespace tesser

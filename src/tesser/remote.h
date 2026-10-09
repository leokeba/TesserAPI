#pragma once

#include <string>

#include <ArduinoJson.h>

#include "tesser/datagram.h"
#include "tesser/node.h"
#include "tesser/platform.h"

namespace tesser {

// Another node's tree grafted into this one (docs/DESIGN.md section 14):
// requests below it are forwarded over a DatagramEndpoint (NowTP, ...), and
// the remote's answer comes back as a deferred reply. A gateway uses it to
// serve ESP-NOW nodes over HTTP.
//
// Created with Object::remote(); lives as long as the tree. Either may be
// destroyed first: a destroyed endpoint detaches its remote nodes, which then
// answer "no endpoint".
class RemoteNode : public Node {
public:
    RemoteNode(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer, std::string remotePath);
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
    RemoteNode& doc(const char* text) {
        setDoc(text);
        return *this;
    }

    DatagramEndpoint* endpoint() const { return endpoint_; }
    const PeerAddress& peer() const { return peer_; }
    const std::string& remotePath() const { return remotePath_; }
    uint32_t timeoutMs() const { return timeoutMs_; }
    bool mirrored() const { return mirror_; }
    bool hasCopy() const;

    // The mirrored copy (objects below `depth` levels as {}), or null.
    void writeCopy(JsonWriter& w, int depth = 255) const;

    // Called by the endpoint, from process(), and when it is destroyed.
    void tick(uint32_t nowMs);
    void detach() { endpoint_ = nullptr; }
    bool handleNotification(const PeerAddress& from, JsonObjectConst envelope);

private:
    bool localPath(std::string& out);

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
};

}  // namespace tesser

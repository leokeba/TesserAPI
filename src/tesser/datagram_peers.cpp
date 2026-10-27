#include "tesser/datagram.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/api.h"
#include "tesser/remote.h"

// The peers a datagram endpoint knows of: heard, lost and forgotten, their
// advertisements, the remote nodes routed through it and those it mounts
// for peers that announce themselves (docs/DESIGN.md sections 10.3 and
// 14.1).

namespace tesser {

namespace {
constexpr size_t kMaxKnown = 32;  // peers known_ (and advertised_) keeps
}  // namespace

std::string buildAdvertisement(uint8_t port, uint32_t schemaHash, size_t maxMessage) {
    char meta[80];
    int n = snprintf(meta, sizeof(meta), "{\"tesser\":%u,\"schema\":\"%08lx\",\"v\":%d", unsigned(port),
                     static_cast<unsigned long>(schemaHash), kProtocolVersion);
    if (n > 0 && maxMessage) {
        n += snprintf(meta + n, sizeof(meta) - static_cast<size_t>(n), ",\"max\":%lu",
                      static_cast<unsigned long>(maxMessage));
    }
    if (n > 0 && static_cast<size_t>(n) + 1 < sizeof(meta)) meta[n++] = '}';
    return std::string(meta, n > 0 ? static_cast<size_t>(n) : 0);
}

bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash) {
    uint8_t protocol;
    return parseAdvertisement(metadata, port, schemaHash, protocol);
}

bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash, uint8_t& protocol) {
    size_t maxMessage;
    return parseAdvertisement(metadata, port, schemaHash, protocol, maxMessage);
}

bool parseAdvertisement(std::string_view metadata, uint8_t& port, uint32_t& schemaHash, uint8_t& protocol,
                        size_t& maxMessage) {
    JsonDocument doc;
    if (deserializeJson(doc, metadata.data(), metadata.size())) return false;
    if (!doc["tesser"].is<uint8_t>() || !doc["schema"].is<const char*>()) return false;
    port = doc["tesser"].as<uint8_t>();
    schemaHash = static_cast<uint32_t>(strtoul(doc["schema"].as<const char*>(), nullptr, 16));
    protocol = doc["v"].is<uint8_t>() && doc["v"].as<uint8_t>() ? doc["v"].as<uint8_t>() : uint8_t(1);
    maxMessage = doc["max"].is<uint32_t>() ? doc["max"].as<uint32_t>() : 0;
    return true;
}

const char* toString(PeerLoss reason) {
    switch (reason) {
        case PeerLoss::Lost: return "lost";
        case PeerLoss::Failed: return "failed";
        case PeerLoss::Goodbye: return "goodbye";
        case PeerLoss::Restarted: return "restarted";
        case PeerLoss::Silent: return "silent";
        case PeerLoss::Stopped: return "stopped";
        case PeerLoss::AuthFailed: return "auth_failed";
    }
    return "lost";
}

// A message to `peer` wasn't delivered (NowTP's SendFailed or Timeout): its
// calls fail at once and its routes go offline, so a remote node takes
// another; but what it holds here as a client (subscriptions, a file
// transfer, its token) stays, as one lost frame on a busy link isn't the
// peer gone (docs/DESIGN.md section 10.3).
void DatagramEndpoint::unreachable(const PeerAddress& peer) {
    markOnline(peer, false);
    expire(0, true, &peer, Reason::SendFailed);
    bool wasHeard = false;
    PeerAddress device;
    {
        MutexGuard guard(mutex_);
        if (Known* k = knownLocked(peer)) {
            wasHeard = k->online;
            k->online = false;
            device = k->device;
        }
    }
    if (wasHeard && onPeerLost_) onPeerLost_(peer, device, PeerLoss::Failed);
}

void DatagramEndpoint::dropPeer(const PeerAddress& peer, PeerLoss reason) {
    // Offline first: a remote node whose call fails below goes by another
    // route at once.
    markOnline(peer, false);
    expire(0, true, &peer,
           reason == PeerLoss::Failed       ? Reason::SendFailed
           : reason == PeerLoss::AuthFailed ? Reason::AuthFailed
                                            : Reason::None);
    dropFiles(&peer, 0);
    bool wasHeard = false;
    PeerAddress device;
    {
        MutexGuard guard(mutex_);
        for (size_t i = 0; i < tokens_.size(); i++) {
            if (tokens_[i].first == peer) {
                tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
        }
        if (PeerAuth* a = authLocked(peer)) {
            // It no longer knows our token.
            a->state = PeerAuth::Idle;
            a->level = Access::Public;
            a->renew = false;
        }
        Known* k = knownLocked(peer);
        if (!k && reason == PeerLoss::AuthFailed) {
            // Never heard (its frames don't authenticate either): known now,
            // so the failure is reported once.
            k = &learnLocked(peer, clock_ ? clock_() : 0);
            k->online = false;
        }
        if (k) {
            wasHeard = k->online;
            k->online = false;
            k->lossPending = false;
            device = k->device;
            // A wrong key is reported once, heard or not, until the peer is
            // heard again.
            if (reason == PeerLoss::AuthFailed) {
                wasHeard = !k->authFailed;
                k->authFailed = true;
            }
        }
    }
    {
        MutexGuard guard(subscribersMutex_);
        for (size_t i = 0; i < subscribers_.size(); i++) {
            if (subscribers_[i]->peer != peer) continue;
            if (api_) api_->dropSubscriber(subscribers_[i]);
            delete subscribers_[i];
            subscribers_.erase(subscribers_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (wasHeard && onPeerLost_) onPeerLost_(peer, device, reason);
}

DatagramEndpoint::Known* DatagramEndpoint::knownLocked(const PeerAddress& peer) {
    for (Known& k : known_) {
        if (k.peer == peer) return &k;
    }
    return nullptr;
}

// Remembers a peer heard from; past 32, the one silent longest is forgotten.
DatagramEndpoint::Known& DatagramEndpoint::learnLocked(const PeerAddress& peer, uint32_t now) {
    if (Known* k = knownLocked(peer)) {
        k->online = true;
        k->heardMs = now;
        k->authFailed = false;
        k->lossPending = false;
        return *k;
    }
    if (known_.size() >= kMaxKnown) {
        size_t oldest = 0;
        for (size_t i = 1; i < known_.size(); i++) {
            bool older = known_[i].online == known_[oldest].online
                             ? static_cast<int32_t>(known_[i].heardMs - known_[oldest].heardMs) < 0
                             : !known_[i].online;
            if (older) oldest = i;
        }
        known_.erase(known_.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    // A NowTP address is the device's MAC.
    known_.push_back(Known{peer, kind_ == TransportKind::NowTP ? peer : PeerAddress(), true, now, false});
    return known_.back();
}

bool DatagramEndpoint::deviceOnline(const PeerAddress& device) const {
    if (!device.length) return false;
    MutexGuard guard(mutex_);
    for (const Known& k : known_) {
        if (k.online && k.device == device) return true;
    }
    return false;
}

PeerAddress DatagramEndpoint::deviceOf(const PeerAddress& peer) const {
    MutexGuard guard(mutex_);
    for (const Known& k : known_) {
        if (k.peer == peer) return k.device;
    }
    return kind_ == TransportKind::NowTP ? peer : PeerAddress();
}

void watchDevices(std::vector<DatagramEndpoint*> endpoints, DatagramEndpoint::PeerLossHandler lost) {
    auto shared = std::make_shared<std::vector<DatagramEndpoint*>>(std::move(endpoints));
    for (DatagramEndpoint* ep : *shared) {
        ep->onPeerLost(DatagramEndpoint::PeerLossHandler(
            [shared, lost](const PeerAddress& peer, const PeerAddress& device, PeerLoss reason) {
                // A restart lost the device's state even though it is back.
                if (device.length && reason != PeerLoss::Restarted) {
                    for (DatagramEndpoint* other : *shared) {
                        if (other->deviceOnline(device)) return;
                    }
                }
                if (lost) lost(peer, device, reason);
            }));
    }
}

void watchDevices(std::vector<DatagramEndpoint*> endpoints, DatagramEndpoint::PeerLostHandler lost) {
    watchDevices(std::move(endpoints),
                 DatagramEndpoint::PeerLossHandler([lost](const PeerAddress& peer, const PeerAddress& device, PeerLoss) {
                     if (lost) lost(peer, device);
                 }));
}

void DatagramEndpoint::autoAdvertise(std::function<void(uint32_t)> publish, uint32_t current, uint32_t debounceMs) {
    publish_ = std::move(publish);
    publishedHash_ = current;
    seenRevision_ = schemaRevision();
    advertiseDebounceMs_ = debounceMs;
    revisionPending_ = false;
}

void DatagramEndpoint::checkAdvertisement(uint32_t now) {
    if (!publish_ || !api_) return;
    uint32_t rev = schemaRevision();
    if (rev != seenRevision_) {
        // Changes come in bursts (several peers mounted at once): wait for quiet.
        seenRevision_ = rev;
        revisionAtMs_ = now;
        revisionPending_ = true;
        return;
    }
    if (!revisionPending_ || now - revisionAtMs_ < advertiseDebounceMs_) return;
    revisionPending_ = false;
    uint32_t hash = api_->schemaHash();
    if (hash == publishedHash_) return;
    publishedHash_ = hash;
    publish_(hash);
}

void DatagramEndpoint::addRemote(RemoteNode* remote) {
    MutexGuard guard(mutex_);
    for (RemoteNode* r : remotes_) {
        if (r == remote) return;
    }
    remotes_.push_back(remote);
}

void DatagramEndpoint::routeAdded(RemoteNode* remote, const PeerAddress& peer) {
    MutexGuard guard(mutex_);
    for (auto& a : advertised_) {
        if (a.peer != peer) continue;
        remote->setAdvertisedSchema(a.schemaHash);
        remote->setProtocol(a.protocol);
    }
    // What this endpoint knows of the peer holds for the new route too.
    if (Known* k = knownLocked(peer)) {
        remote->setRouteOnline(this, peer, k->online, k->online ? k->heardMs : 0);
    }
}

void DatagramEndpoint::removeRemote(RemoteNode* remote) {
    for (;;) {
        {
            MutexGuard guard(mutex_);
            unlistLocked(remote);
            if (!pinnedLocked(remote)) return;
        }
        // Another task is calling the node (it is out of remotes_ now, so
        // no new call starts): its destruction waits for the call to end.
        unpinned_.take(10);
    }
}

void DatagramEndpoint::unlistLocked(const RemoteNode* remote) {
    for (size_t i = 0; i < remotes_.size(); i++) {
        if (remotes_[i] == remote) {
            remotes_.erase(remotes_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

bool DatagramEndpoint::pinnedLocked(const RemoteNode* node) const {
    for (const RemoteNode* p : pinned_) {
        if (p == node) return true;
    }
    return false;
}

bool DatagramEndpoint::pinned(const RemoteNode* node) const {
    MutexGuard guard(mutex_);
    return pinnedLocked(node);
}

void DatagramEndpoint::Pinned::all() {
    for (RemoteNode* r : endpoint_.remotes_) add(r);
}

void DatagramEndpoint::Pinned::add(RemoteNode* node) {
    nodes.push_back(node);
    endpoint_.pinned_.push_back(node);
}

DatagramEndpoint::Pinned::~Pinned() {
    if (nodes.empty()) return;
    {
        MutexGuard guard(endpoint_.mutex_);
        std::vector<RemoteNode*>& pinned = endpoint_.pinned_;
        for (RemoteNode* n : nodes) {
            for (size_t i = 0; i < pinned.size(); i++) {
                if (pinned[i] == n) {
                    pinned.erase(pinned.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
    }
    endpoint_.unpinned_.give();
}

void DatagramEndpoint::forgetPeer(const PeerAddress& peer, PeerLoss reason) {
    {
        MutexGuard guard(mutex_);
        lostPeers_.emplace_back(peer, reason);
    }
    if (onQueued_) onQueued_();
}

void DatagramEndpoint::linkLost(const PeerAddress& peer) {
    {
        MutexGuard guard(mutex_);
        unheardPeers_.push_back(peer);
    }
    if (onQueued_) onQueued_();
}

bool DatagramEndpoint::lossPending(const PeerAddress& peer) const {
    MutexGuard guard(mutex_);
    for (const Known& k : known_) {
        if (k.peer == peer) return k.lossPending;
    }
    return false;
}

// A link's loss for a peer heard within kLossGraceMs: offline only, until it
// is heard again or has been silent that long (dropDeferred()). NowTP loses a
// peer whose announcements it missed, though its other messages come: a
// gateway relaying a firmware image lost its transfer on the remote mid-upload
// ("no upload in progress", DEVKIT-V3 to DEVKIT-R1).
bool DatagramEndpoint::deferLoss(const PeerAddress& peer) {
    uint32_t now = clock_ ? clock_() : 0;
    {
        MutexGuard guard(mutex_);
        Known* k = knownLocked(peer);
        if (!k || !k->online || now - k->heardMs >= kLossGraceMs) return false;
        k->lossPending = true;
    }
    markOnline(peer, false);
    return true;
}

void DatagramEndpoint::dropDeferred(uint32_t now) {
    std::vector<PeerAddress> gone;
    {
        MutexGuard guard(mutex_);
        for (Known& k : known_) {
            if (k.lossPending && now - k.heardMs >= kLossGraceMs) gone.push_back(k.peer);
        }
    }
    for (const PeerAddress& p : gone) dropPeer(p, PeerLoss::Lost);
}

void DatagramEndpoint::mountPeers(Object& parent, uint32_t mirrorIntervalMs) {
    MutexGuard guard(mutex_);
    for (auto& m : mounts_) {
        if (m.first == &parent) {
            m.second = mirrorIntervalMs;
            return;
        }
    }
    mounts_.emplace_back(&parent, mirrorIntervalMs);
}

void DatagramEndpoint::peerSeen(const PeerAddress& peer, std::string_view name, uint32_t schemaHash,
                                const PeerAddress& device, uint8_t protocol, size_t maxMessage) {
    {
        MutexGuard guard(mutex_);
        for (Seen& s : seenPeers_) {
            if (s.peer == peer) {  // coalesce repeated announcements
                s.name.assign(name.data(), name.size());
                s.schemaHash = schemaHash;
                s.device = device;
                s.protocol = protocol;
                s.maxMessage = maxMessage;
                return;
            }
        }
        seenPeers_.push_back(Seen{peer, std::string(name), schemaHash, device, protocol, maxMessage});
    }
    if (onQueued_) onQueued_();
}

void DatagramEndpoint::markOnline(const PeerAddress& peer, bool online) {
    uint32_t now = clock_ ? clock_() : 0;
    MutexGuard guard(mutex_);
    if (online) learnLocked(peer, now);
    for (RemoteNode* r : remotes_) r->setRouteOnline(this, peer, online, now);
}

namespace {

// A node name from an advertised name: characters names can't hold become
// '-', and leading or trailing dashes go.
std::string nodeName(std::string_view advertised) {
    std::string n;
    for (char c : advertised) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-' || c == '.';
        n += ok ? c : '-';
    }
    while (!n.empty() && n.front() == '-') n.erase(n.begin());
    while (!n.empty() && n.back() == '-') n.pop_back();
    return n;
}

}  // namespace

void DatagramEndpoint::applySeen(const Seen& seen) {
    std::vector<std::pair<Object*, uint32_t>> mounts;
    // The device: a NowTP address is its MAC; other links advertise it.
    const PeerAddress& device = seen.device.length || kind_ != TransportKind::NowTP ? seen.device : seen.peer;
    Pinned routed(*this);  // nodes with a route through the peer
    {
        MutexGuard guard(mutex_);
        uint32_t now = clock_ ? clock_() : 0;
        Known& k = learnLocked(seen.peer, now);
        if (device.length) k.device = device;
        k.maxMessage = seen.maxMessage;
        for (RemoteNode* r : remotes_) {
            if (!r->hasRoute(this, seen.peer)) continue;
            r->setRouteOnline(this, seen.peer, true, now);
            r->setAdvertisedSchema(seen.schemaHash);
            r->setProtocol(seen.protocol);
            routed.add(r);
        }
        bool found = false;
        for (auto& a : advertised_) {
            if (a.peer == seen.peer) {
                a.schemaHash = seen.schemaHash;
                a.protocol = seen.protocol;
                found = true;
            }
        }
        if (!found) {
            // As many as known_ holds: one forgotten announces again.
            if (advertised_.size() >= kMaxKnown) advertised_.erase(advertised_.begin());
            advertised_.push_back(Advertised{seen.peer, seen.schemaHash, seen.protocol});
        }
        mounts = mounts_;
    }
    if (!api_) return;
    MutexGuard apiGuard(api_->mutex());
    // A node declared with this address learns the device it reaches.
    if (device.length) {
        for (RemoteNode* r : routed.nodes) {
            if (!r->device().length) r->setDevice(device);
        }
    }
    for (auto& m : mounts) mount(*m.first, m.second, seen, device);
}

// Under the API lock.
void DatagramEndpoint::mount(Object& parentRef, uint32_t mirrorMs, const Seen& seen, const PeerAddress& device) {
    Object* parent = &parentRef;
    // A parent mounts each device once: the first transport to announce it
    // mounts it, and the others add their routes to that node.
    for (const Node* c = parent->first(); c; c = c->next()) {
        if (c->type() != NodeType::Remote) continue;
        auto* r = const_cast<RemoteNode*>(static_cast<const RemoteNode*>(c));
        if (r->hasRoute(this, seen.peer)) return;
        if (device.length && r->device() == device) {
            // Bounded however many addresses announce the device (over UDP
            // the device is what the peer says): an offline route makes
            // room, else the address isn't added.
            if (!options_.maxRoutesPerNode || r->routeCount(this) < options_.maxRoutesPerNode) {
                r->addRoute(*this, seen.peer);
            } else if (!r->replaceOfflineRoute(*this, seen.peer)) {
                return;
            }
            r->setAdvertisedSchema(seen.schemaHash);
            r->setProtocol(seen.protocol);
            return;
        }
    }
    // Named after the peer; when that is taken or empty, the last three bytes
    // of its device's address (or its own) are added.
    std::string name = nodeName(seen.name);
    if (name.empty() || parent->child(name)) {
        char suffix[8] = "";
        const uint8_t* b = device.length ? device.bytes : seen.peer.bytes;
        uint8_t n = device.length ? device.length : seen.peer.length;
        if (n >= 3) snprintf(suffix, sizeof(suffix), "%02x%02x%02x", b[n - 3], b[n - 2], b[n - 1]);
        name = name.empty() ? std::string(suffix) : name + "-" + suffix;
    }
    if (name.empty() || parent->child(name)) return;
    if (!roomToMount()) return;
    RemoteNode& node = parent->add(new RemoteNode(std::move(name), *this, seen.peer, "/"));
    node.setAutoMounted();
    node.setDevice(device);
    if (mirrorMs) node.mirror(mirrorMs);
    node.changed();  // subscribers of the parent see the new key
}

// Under the API lock: room for one more node mounted for a peer
// (Options::maxMounted). Past it, the node that was heard longest ago among
// those nothing uses is evicted: mounted here, offline, reached through this
// endpoint only (no other endpoint's task holds it), and without a
// subscription, a call in flight, a relayed transfer or a task calling it
// (pinned) (docs/DESIGN.md section 14.1).
bool DatagramEndpoint::roomToMount() {
    if (!options_.maxMounted) return true;
    std::vector<std::pair<Object*, uint32_t>> mounts;
    {
        MutexGuard guard(mutex_);
        mounts = mounts_;
    }
    size_t count = 0;
    Object* evictParent = nullptr;
    RemoteNode* evict = nullptr;
    uint32_t evictHeard = 0;
    for (auto& m : mounts) {
        for (Node* c = m.first->first(); c; c = c->next()) {
            if (c->type() != NodeType::Remote) continue;
            auto* r = static_cast<RemoteNode*>(c);
            if (!r->autoMounted() || !r->routeCount(this)) continue;
            count++;
            if (r->online() || !r->onlyThrough(this) || r->relays() || hasCalls(r) || api_->subscribedLocked(*r) ||
                pinned(r)) {
                continue;
            }
            uint32_t heard = r->lastHeardMs();
            if (!evict || static_cast<int32_t>(heard - evictHeard) < 0) {
                evict = r;
                evictParent = m.first;
                evictHeard = heard;
            }
        }
    }
    if (count < options_.maxMounted) return true;
    if (!evict) {
        MutexGuard guard(mutex_);
        stats_.unmounted++;
        return false;
    }
    {
        // Out of remotes_ first, so no call into it starts any more: unless
        // one started since (another task's process()), it goes now.
        MutexGuard guard(mutex_);
        if (pinnedLocked(evict)) {
            stats_.unmounted++;
            return false;
        }
        unlistLocked(evict);
    }
    // A reply that let go of the API lock may still stand on it: it leaves
    // this endpoint now, and is destroyed once no reply has (Api::retire()).
    evictParent->unlink(evict);
    cancelCalls(evict);
    evict->detach(this);
    api_->retire(evict);
    MutexGuard guard(mutex_);
    stats_.evicted++;
    return true;
}

}  // namespace tesser

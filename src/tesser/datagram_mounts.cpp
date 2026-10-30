#include "tesser/datagram.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/api.h"
#include "tesser/remote.h"

// The remote nodes routed through a datagram endpoint, and the nodes it
// mounts for peers that announce themselves (detail::Mounter, docs/DESIGN.md
// section 14.1).

namespace tesser {
namespace detail {

void Mounter::addRemote(RemoteNode* remote) {
    MutexGuard guard(ep_.mutex_);
    for (RemoteNode* r : remotes_) {
        if (r == remote) return;
    }
    remotes_.push_back(remote);
}

void Mounter::routeAdded(RemoteNode* remote, const PeerAddress& peer) {
    MutexGuard guard(ep_.mutex_);
    const PeerTable::Peer* p = ep_.peers_.find(peer);
    if (p && p->advertised) {
        remote->setAdvertisedSchema(p->schemaHash);
        remote->setProtocol(p->protocol);
    }
    // What this endpoint knows of the peer holds for the new route too.
    if (p && p->known) remote->setRouteOnline(&ep_, peer, p->online, p->online ? p->heardMs : 0);
}

void Mounter::removeRemote(RemoteNode* remote) {
    for (;;) {
        {
            MutexGuard guard(ep_.mutex_);
            unlistLocked(remote);
            if (!pinnedLocked(remote)) return;
        }
        // Another task is calling the node (it is out of remotes_ now, so
        // no new call starts): its destruction waits for the call to end.
        unpinned_.take(10);
    }
}

void Mounter::unlistLocked(const RemoteNode* remote) {
    for (size_t i = 0; i < remotes_.size(); i++) {
        if (remotes_[i] == remote) {
            remotes_.erase(remotes_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

bool Mounter::pinnedLocked(const RemoteNode* node) const {
    for (const RemoteNode* p : pinned_) {
        if (p == node) return true;
    }
    return false;
}

bool Mounter::pinned(const RemoteNode* node) const {
    MutexGuard guard(ep_.mutex_);
    return pinnedLocked(node);
}

void Mounter::detachAll() {
    for (RemoteNode* r : remotes_) r->detach(&ep_);
    remotes_.clear();
}

void Mounter::Pinned::all() {
    if (nodes.empty() && !scratch_ && !mounter_.pinScratchBusy_) {
        nodes.swap(mounter_.pinScratch_);
        mounter_.pinScratchBusy_ = scratch_ = true;
    }
    for (RemoteNode* r : mounter_.remotes_) add(r);
}

void Mounter::Pinned::add(RemoteNode* node) {
    nodes.push_back(node);
    mounter_.pinned_.push_back(node);
}

Mounter::Pinned::~Pinned() {
    if (nodes.empty() && !scratch_) return;
    bool any = !nodes.empty();
    {
        MutexGuard guard(mounter_.ep_.mutex_);
        std::vector<RemoteNode*>& pinned = mounter_.pinned_;
        for (RemoteNode* n : nodes) {
            for (size_t i = 0; i < pinned.size(); i++) {
                if (pinned[i] == n) {
                    pinned.erase(pinned.begin() + static_cast<std::ptrdiff_t>(i));
                    break;
                }
            }
        }
        if (scratch_) {
            nodes.clear();
            nodes.swap(mounter_.pinScratch_);
            mounter_.pinScratchBusy_ = false;
        }
    }
    if (any) mounter_.unpinned_.give();
}

void Mounter::mountPeers(Object& parent, uint32_t mirrorIntervalMs) {
    MutexGuard guard(ep_.mutex_);
    for (auto& m : mounts_) {
        if (m.first == &parent) {
            m.second = mirrorIntervalMs;
            return;
        }
    }
    mounts_.emplace_back(&parent, mirrorIntervalMs);
}

void Mounter::peerSeen(const PeerAddress& peer, std::string_view name, uint32_t schemaHash,
                                const PeerAddress& device, uint8_t protocol, size_t maxMessage) {
    {
        MutexGuard guard(ep_.mutex_);
        for (SeenPeer& s : seen_) {
            if (s.peer == peer) {  // coalesce repeated announcements
                s.name.assign(name.data(), name.size());
                s.schemaHash = schemaHash;
                s.device = device;
                s.protocol = protocol;
                s.maxMessage = maxMessage;
                return;
            }
        }
        seen_.push_back(SeenPeer{peer, std::string(name), schemaHash, device, protocol, maxMessage});
    }
    ep_.wake();
}

void Mounter::markOnline(const PeerAddress& peer, bool online) {
    uint32_t now = ep_.now();
    MutexGuard guard(ep_.mutex_);
    if (online) ep_.peers_.learn(peer, now);
    for (RemoteNode* r : remotes_) r->setRouteOnline(&ep_, peer, online, now);
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

bool Mounter::applySeen() {
    std::vector<SeenPeer> seen;
    {
        MutexGuard guard(ep_.mutex_);
        seen.swap(seen_);
    }
    for (const SeenPeer& s : seen) apply(s);
    return !seen.empty();
}

void Mounter::apply(const SeenPeer& seen) {
    std::vector<std::pair<Object*, uint32_t>> mounts;
    // The device: a NowTP address is its MAC; other links advertise it.
    const PeerAddress& device = seen.device.length || ep_.kind_ != TransportKind::NowTP ? seen.device : seen.peer;
    Pinned routed(*this);  // nodes with a route through the peer
    {
        MutexGuard guard(ep_.mutex_);
        uint32_t now = ep_.now();
        PeerTable::Peer& k = ep_.peers_.learn(seen.peer, now);
        if (device.length) k.device = device;
        k.maxMessage = seen.maxMessage;
        for (RemoteNode* r : remotes_) {
            if (!r->hasRoute(&ep_, seen.peer)) continue;
            r->setRouteOnline(&ep_, seen.peer, true, now);
            r->setAdvertisedSchema(seen.schemaHash);
            r->setProtocol(seen.protocol);
            routed.add(r);
        }
        ep_.peers_.setAdvertised(seen.peer, seen.schemaHash, seen.protocol);
        mounts = mounts_;
    }
    if (!ep_.api_) return;
    MutexGuard apiGuard(ep_.api_->mutex());
    // A node declared with this address learns the device it reaches.
    if (device.length) {
        for (RemoteNode* r : routed.nodes) {
            if (!r->device().length) r->setDevice(device);
        }
    }
    for (auto& m : mounts) mount(*m.first, m.second, seen, device);
}

// Under the API lock.
void Mounter::mount(Object& parentRef, uint32_t mirrorMs, const SeenPeer& seen, const PeerAddress& device) {
    Object* parent = &parentRef;
    // A parent mounts each device once: the first transport to announce it
    // mounts it, and the others add their routes to that node.
    for (const Node* c = parent->first(); c; c = c->next()) {
        if (c->type() != NodeType::Remote) continue;
        auto* r = const_cast<RemoteNode*>(static_cast<const RemoteNode*>(c));
        if (r->hasRoute(&ep_, seen.peer)) return;
        if (device.length && r->device() == device) {
            // Bounded however many addresses announce the device (over UDP
            // the device is what the peer says): an offline route makes
            // room, else the address isn't added.
            if (!ep_.options_.maxRoutesPerNode || r->routeCount(&ep_) < ep_.options_.maxRoutesPerNode) {
                r->addRoute(ep_, seen.peer);
            } else if (!r->replaceOfflineRoute(ep_, seen.peer)) {
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
    RemoteNode& node = parent->add(new RemoteNode(std::move(name), ep_, seen.peer, "/"));
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
bool Mounter::roomToMount() {
    if (!ep_.options_.maxMounted) return true;
    std::vector<std::pair<Object*, uint32_t>> mounts;
    {
        MutexGuard guard(ep_.mutex_);
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
            if (!r->autoMounted() || !r->routeCount(&ep_)) continue;
            count++;
            if (r->online() || !r->onlyThrough(&ep_) || r->relays() || ep_.calls_.has(r) || ep_.api_->subscribedLocked(*r) ||
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
    if (count < ep_.options_.maxMounted) return true;
    if (!evict) {
        MutexGuard guard(ep_.mutex_);
        ep_.stats_.unmounted++;
        return false;
    }
    {
        // Out of remotes_ first, so no call into it starts any more: unless
        // one started since (another task's process()), it goes now.
        MutexGuard guard(ep_.mutex_);
        if (pinnedLocked(evict)) {
            ep_.stats_.unmounted++;
            return false;
        }
        unlistLocked(evict);
    }
    // A reply that let go of the API lock may still stand on it: it leaves
    // this endpoint now, and is destroyed once no reply has (Api::retire()).
    evictParent->unlink(evict);
    ep_.calls_.cancel(evict);
    evict->detach(&ep_);
    ep_.api_->retire(evict);
    MutexGuard guard(ep_.mutex_);
    ep_.stats_.evicted++;
    return true;
}

}  // namespace detail
}  // namespace tesser

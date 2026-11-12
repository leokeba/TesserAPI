#include "tesser/api.h"
#include "tesser/datagram_types.h"

#include <string.h>

// Who writes a tracked node, and releasing what a lost writer set
// (docs/DESIGN.md section 14.4).

namespace tesser {

namespace {

// `n`, or a node below it, changed in (since, now].
bool changedWithin(const Node& n, uint32_t since, uint32_t now) {
    if (n.type() != NodeType::Object) return changedSince(n.generation(), since, now);
    for (const Node* c = static_cast<const Object&>(n).first(); c; c = c->next()) {
        if (changedWithin(*c, since, now)) return true;
    }
    return false;
}

// `inner` is `outer` or below it.
bool contains(const Node& outer, const Node& inner) {
    if (&outer == &inner) return true;
    if (outer.type() != NodeType::Object) return false;
    for (const Node* c = static_cast<const Object&>(outer).first(); c; c = c->next()) {
        if (contains(*c, inner)) return true;
    }
    return false;
}

void setWriter(uint8_t (&device)[6], uint8_t& length, const Client* client) {
    length = client && client->deviceLength ? client->deviceLength : 0;
    if (length > sizeof(device)) length = sizeof(device);
    if (length) memcpy(device, client->device, length);
}

}  // namespace

void Api::trackWriter(Node& node, WriterLost release) {
    MutexGuard guard(mutex_);
    writersHook_ = [](Api& api, const Client& client, uint32_t before) { api.writersSet(client, before); };
    for (Tracked& t : tracked_) {
        if (t.node == &node) {
            t.release = std::move(release);
            return;
        }
    }
    tracked_.push_back(Tracked{&node, std::move(release), {}, 0});
}

void Api::untrackWriter(Node& node) {
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < tracked_.size(); i++) {
        if (tracked_[i].node == &node) {
            tracked_.erase(tracked_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (tracked_.empty()) writersHook_ = nullptr;
}

void Api::writersSet(const Client& client, uint32_t before) {
    uint32_t now = currentGeneration();
    for (Tracked& t : tracked_) {
        if (changedWithin(*t.node, before, now)) setWriter(t.device, t.deviceLength, &client);
    }
}

void Api::wrote(Node& node) {
    MutexGuard guard(mutex_);
    for (Tracked& t : tracked_) {
        if (contains(*t.node, node) || contains(node, *t.node)) setWriter(t.device, t.deviceLength, current_);
    }
}

bool Api::writerOf(const Node& node, PeerAddress& device) const {
    MutexGuard guard(mutex_);
    for (const Tracked& t : tracked_) {
        if (t.node != &node) continue;
        if (!t.deviceLength) return false;
        device = PeerAddress::fromBytes(t.device, t.deviceLength);
        return true;
    }
    return false;
}

void Api::writerLost(const PeerAddress& device, PeerLoss reason) {
    if (!device.length) return;
    MutexGuard guard(mutex_);
    // Forgotten first, and called from a copy: a release may write, track
    // or untrack.
    std::vector<std::pair<Node*, WriterLost>> released;
    for (Tracked& t : tracked_) {
        if (!t.deviceLength || !(PeerAddress::fromBytes(t.device, t.deviceLength) == device)) continue;
        t.deviceLength = 0;
        if (t.release) released.emplace_back(t.node, t.release);
    }
    for (auto& r : released) r.second(*r.first, reason);
}

}  // namespace tesser

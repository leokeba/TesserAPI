#include "tesser/datagram.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/api.h"
#include "tesser/remote.h"

// The peers a datagram endpoint knows of (detail::PeerTable): heard, lost and
// forgotten, what they advertised and the tokens they hold (docs/DESIGN.md
// sections 10.3 and 14.1).

namespace tesser {

namespace {
constexpr size_t kMaxKnown = 32;  // peers a table knows, and whose advertisements it keeps
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
    detail::Advert a;
    if (!detail::parseAdvert(metadata, a) || a.port < 0 || a.port > 255 || !a.hasSchema) return false;
    port = static_cast<uint8_t>(a.port);
    schemaHash = a.schemaHash;
    protocol = a.protocol;
    maxMessage = a.maxMessage;
    return true;
}

namespace {
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

bool detail::parseAdvert(std::string_view text, Advert& out) {
    JsonDocument doc;
    if (parseJson(doc, text) || !doc.is<JsonObjectConst>()) return false;
    if (doc["tesser"].is<uint8_t>()) out.port = doc["tesser"].as<uint8_t>();
    if (doc["name"].is<const char*>()) {
        out.hasName = true;
        out.name = doc["name"].as<const char*>();
    }
    if (doc["schema"].is<const char*>()) {
        out.hasSchema = true;
        out.schemaHash = static_cast<uint32_t>(strtoul(doc["schema"].as<const char*>(), nullptr, 16));
    }
    if (doc["v"].is<uint8_t>() && doc["v"].as<uint8_t>()) out.protocol = doc["v"].as<uint8_t>();
    if (doc["max"].is<uint32_t>()) out.maxMessage = doc["max"].as<uint32_t>();
    const char* id = doc["id"] | "";
    uint8_t mac[6];
    bool good = strlen(id) == 12;
    for (int i = 0; good && i < 6; i++) {
        int hi = hexValue(id[2 * i]), lo = hexValue(id[2 * i + 1]);
        good = hi >= 0 && lo >= 0;
        if (good) mac[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    if (good) out.device = PeerAddress::fromBytes(mac, 6);
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

namespace detail {

PeerTable::~PeerTable() {
    for (Peer& p : peers_) delete p.tokens;
}

PeerTable::Peer* PeerTable::find(const PeerAddress& peer) {
    for (Peer& p : peers_) {
        if (p.peer == peer) return &p;
    }
    return nullptr;
}

const PeerTable::Peer* PeerTable::find(const PeerAddress& peer) const {
    return const_cast<PeerTable*>(this)->find(peer);
}

PeerTable::Peer* PeerTable::known(const PeerAddress& peer) {
    Peer* p = find(peer);
    return p && p->known ? p : nullptr;
}

const PeerTable::Peer* PeerTable::known(const PeerAddress& peer) const {
    return const_cast<PeerTable*>(this)->known(peer);
}

PeerTable::Peer& PeerTable::record(const PeerAddress& peer) {
    if (Peer* p = find(peer)) return *p;
    peers_.emplace_back();
    peers_.back().peer = peer;
    return peers_.back();
}

void PeerTable::trim(Peer& p) {
    if (p.known || p.advertised || p.tokens) return;
    peers_.erase(peers_.begin() + (&p - peers_.data()));
}

void PeerTable::trimTokens(Peer& p) {
    if (p.tokens && !p.tokens->holds && !p.tokens->presents) {
        delete p.tokens;
        p.tokens = nullptr;
    }
    trim(p);
}

PeerTable::Peer& PeerTable::learn(const PeerAddress& peer, uint32_t now) {
    if (Peer* p = known(peer)) {
        p->online = true;
        p->heardMs = now;
        p->authFailed = false;
        p->lossPending = false;
        return *p;
    }
    size_t count = 0;
    Peer* oldest = nullptr;
    for (Peer& p : peers_) {
        if (!p.known) continue;
        count++;
        bool older = !oldest || (p.online == oldest->online ? static_cast<int32_t>(p.heardMs - oldest->heardMs) < 0
                                                            : !p.online);
        if (older) oldest = &p;
    }
    if (count >= kMaxKnown) {
        oldest->known = oldest->online = oldest->authFailed = oldest->lossPending = false;
        oldest->device = PeerAddress();
        oldest->maxMessage = 0;
        trim(*oldest);
    }
    Peer& p = record(peer);
    p.known = p.online = true;
    p.heardMs = now;
    if (kind_ == TransportKind::NowTP) p.device = peer;  // the device's MAC
    return p;
}

void PeerTable::setAdvertised(const PeerAddress& peer, uint32_t schemaHash, uint8_t protocol) {
    Peer* p = find(peer);
    if (!p || !p->advertised) {
        // As many as it knows: one forgotten announces again.
        size_t count = 0;
        Peer* oldest = nullptr;
        for (Peer& q : peers_) {
            if (!q.advertised) continue;
            count++;
            if (!oldest || static_cast<int32_t>(q.advertisedAt - oldest->advertisedAt) < 0) oldest = &q;
        }
        if (count >= kMaxKnown) {
            oldest->advertised = false;
            trim(*oldest);
        }
        p = &record(peer);
        p->advertised = true;
        p->advertisedAt = ++count_;
    }
    p->schemaHash = schemaHash;
    p->protocol = protocol;
}

PeerTable::Tokens& PeerTable::tokensOf(const PeerAddress& peer) {
    Peer& p = record(peer);
    if (!p.tokens) p.tokens = new Tokens;
    return *p.tokens;
}

bool PeerTable::heldToken(const PeerAddress& peer, std::string& out) const {
    const Peer* p = find(peer);
    if (!p || !p->tokens || !p->tokens->holds) return false;
    out = p->tokens->held;
    return true;
}

void PeerTable::dropHeldToken(const PeerAddress& peer) {
    Peer* p = find(peer);
    if (!p || !p->tokens) return;
    p->tokens->holds = false;
    std::string().swap(p->tokens->held);
    trimTokens(*p);
}

void PeerTable::holdToken(const PeerAddress& peer, std::string_view token, size_t most) {
    dropHeldToken(peer);
    if (token.empty()) return;
    size_t count = 0;
    Peer* oldest = nullptr;
    for (Peer& p : peers_) {
        if (!p.tokens || !p.tokens->holds) continue;
        count++;
        if (!oldest || static_cast<int32_t>(p.tokens->heldAt - oldest->tokens->heldAt) < 0) oldest = &p;
    }
    if (count >= most && oldest) dropHeldToken(oldest->peer);
    if (!most) return;
    Tokens& t = tokensOf(peer);
    t.holds = true;
    t.held.assign(token.data(), token.size());
    t.heldAt = ++count_;
}

PeerTable::Presented* PeerTable::presented(const PeerAddress& peer) {
    Peer* p = find(peer);
    return p && p->tokens && p->tokens->presents ? &p->tokens->presented : nullptr;
}

const PeerTable::Presented* PeerTable::presented(const PeerAddress& peer) const {
    return const_cast<PeerTable*>(this)->presented(peer);
}

PeerTable::Presented& PeerTable::present(const PeerAddress& peer) {
    Tokens& t = tokensOf(peer);
    t.presents = true;
    t.presented = Presented();
    return t.presented;
}

void PeerTable::stopPresenting(const PeerAddress& peer) {
    Peer* p = find(peer);
    if (!p || !p->tokens) return;
    p->tokens->presents = false;
    p->tokens->presented = Presented();
    trimTokens(*p);
}

}  // namespace detail

// A message to `peer` wasn't delivered (NowTP's SendFailed or Timeout): its
// calls fail at once and its routes go offline, so a remote node takes
// another; but what it holds here as a client (subscriptions, a file
// transfer, its token) stays, as one lost frame on a busy link isn't the
// peer gone (docs/DESIGN.md section 10.3).
void DatagramEndpoint::unreachable(const PeerAddress& peer) {
    mounts_.markOnline(peer, false);
    calls_.expire(0, true, &peer, Reason::SendFailed);
    bool wasHeard = false;
    PeerAddress device;
    {
        MutexGuard guard(mutex_);
        if (detail::PeerTable::Peer* k = peers_.known(peer)) {
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
    mounts_.markOnline(peer, false);
    calls_.expire(0, true, &peer,
                  reason == PeerLoss::Failed       ? Reason::SendFailed
                  : reason == PeerLoss::AuthFailed ? Reason::AuthFailed
                                                   : Reason::None);
    server_.dropFiles(&peer, 0);
    bool wasHeard = false;
    PeerAddress device;
    {
        MutexGuard guard(mutex_);
        peers_.dropHeldToken(peer);
        if (detail::PeerTable::Presented* a = peers_.presented(peer)) {
            // It no longer knows our token.
            a->state = detail::PeerTable::Presented::Idle;
            a->level = Access::Public;
            a->renew = false;
        }
        detail::PeerTable::Peer* k = peers_.known(peer);
        if (!k && reason == PeerLoss::AuthFailed) {
            // Never heard (its frames don't authenticate either): known now,
            // so the failure is reported once.
            k = &peers_.learn(peer, now());
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
    server_.dropSubscriber(peer);
    if (wasHeard && onPeerLost_) onPeerLost_(peer, device, reason);
}

bool DatagramEndpoint::deviceOnline(const PeerAddress& device) const {
    if (!device.length) return false;
    MutexGuard guard(mutex_);
    for (const detail::PeerTable::Peer& k : peers_.peers()) {
        if (k.known && k.online && k.device == device) return true;
    }
    return false;
}

PeerAddress DatagramEndpoint::deviceOf(const PeerAddress& peer) const {
    MutexGuard guard(mutex_);
    if (const detail::PeerTable::Peer* k = peers_.known(peer)) return k->device;
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

void DatagramEndpoint::checkAdvertisement(uint32_t t) {
    if (!publish_ || !api_) return;
    uint32_t rev = schemaRevision();
    if (rev != seenRevision_) {
        // Changes come in bursts (several peers mounted at once): wait for quiet.
        seenRevision_ = rev;
        revisionAtMs_ = t;
        revisionPending_ = true;
        return;
    }
    if (!revisionPending_ || t - revisionAtMs_ < advertiseDebounceMs_) return;
    revisionPending_ = false;
    uint32_t hash = api_->schemaHash();
    if (hash == publishedHash_) return;
    publishedHash_ = hash;
    publish_(hash);
}

void DatagramEndpoint::forgetPeer(const PeerAddress& peer, PeerLoss reason) {
    {
        MutexGuard guard(mutex_);
        lostPeers_.emplace_back(peer, reason);
    }
    if (onQueued_) onQueued_();
}

void DatagramEndpoint::forgetAll(PeerLoss reason) {
    std::vector<PeerAddress> peers;
    auto add = [&peers](const PeerAddress& p) {
        if (!p.length) return;
        for (const PeerAddress& q : peers) {
            if (q == p) return;
        }
        peers.push_back(p);
    };
    {
        MutexGuard guard(mutex_);
        for (const detail::PeerTable::Peer& k : peers_.peers()) {
            if (k.known) add(k.peer);
        }
        for (const detail::CallTable::Slot& c : calls_.calls()) add(c.to);
    }
    server_.eachSubscriber(add);
    {
        MutexGuard guard(mutex_);
        for (const PeerAddress& p : peers) lostPeers_.emplace_back(p, reason);
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
    const detail::PeerTable::Peer* k = peers_.known(peer);
    return k && k->lossPending;
}

// A link's loss for a peer heard within kLossGraceMs: offline only, until it
// is heard again or has been silent that long (dropDeferred()). NowTP loses a
// peer whose announcements it missed, though its other messages come: a
// gateway relaying a firmware image lost its transfer on the remote mid-upload
// ("no upload in progress", DEVKIT-V3 to DEVKIT-R1).
bool DatagramEndpoint::deferLoss(const PeerAddress& peer) {
    uint32_t t = now();
    {
        MutexGuard guard(mutex_);
        detail::PeerTable::Peer* k = peers_.known(peer);
        if (!k || !k->online || t - k->heardMs >= kLossGraceMs) return false;
        k->lossPending = true;
    }
    mounts_.markOnline(peer, false);
    return true;
}

void DatagramEndpoint::dropDeferred(uint32_t t) {
    std::vector<PeerAddress> gone;
    {
        MutexGuard guard(mutex_);
        for (const detail::PeerTable::Peer& k : peers_.peers()) {
            if (k.lossPending && t - k.heardMs >= kLossGraceMs) gone.push_back(k.peer);
        }
    }
    for (const PeerAddress& p : gone) dropPeer(p, PeerLoss::Lost);
}

}  // namespace tesser

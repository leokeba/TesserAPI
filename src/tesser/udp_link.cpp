#include "tesser/udp_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tesser/api.h"
#include "tesser/json_writer.h"

namespace tesser {

// Wire format (docs/DESIGN.md section 10.7): every datagram starts with a
// 20-byte header, multi-byte fields little-endian:
//   0 magic 'T'   1 version << 4 | type   2 flags   3 index (data) or status (ack)
//   4 sender's nonce   8 message id   12 fragment count   13 reserved
//   14 fragment size   16 message length   20 payload
namespace {

constexpr uint8_t kMagic = 0x54;
constexpr uint8_t kVersion = 1;
constexpr size_t kHeader = 20;
constexpr unsigned kMaxFragments = 64;

constexpr uint8_t kData = 0;
constexpr uint8_t kAck = 1;
constexpr uint8_t kAdvert = 2;

// data flags
constexpr uint8_t kReliable = 0x01;
constexpr uint8_t kAckRequest = 0x02;
constexpr uint8_t kBroadcast = 0x04;
// advertisement flags
constexpr uint8_t kQuery = 0x01;
constexpr uint8_t kBye = 0x02;
// acknowledgement status
constexpr uint8_t kComplete = 0;
constexpr uint8_t kPartial = 1;
constexpr uint8_t kRejected = 2;

struct Header {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint8_t index = 0;
    uint8_t count = 0;
    uint32_t nonce = 0;
    uint32_t id = 0;
    uint16_t chunk = 0;
    uint32_t length = 0;
};

void put16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t get32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}
uint64_t get64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

void writeHeader(uint8_t* p, const Header& h) {
    p[0] = kMagic;
    p[1] = static_cast<uint8_t>(kVersion << 4 | h.type);
    p[2] = h.flags;
    p[3] = h.index;
    put32(p + 4, h.nonce);
    put32(p + 8, h.id);
    p[12] = h.count;
    p[13] = 0;
    put16(p + 14, h.chunk);
    put32(p + 16, h.length);
}

bool readHeader(const uint8_t* p, size_t len, Header& h) {
    if (len < kHeader || p[0] != kMagic || (p[1] >> 4) != kVersion) return false;
    h.type = p[1] & 0x0f;
    h.flags = p[2];
    h.index = p[3];
    h.nonce = get32(p + 4);
    h.id = get32(p + 8);
    h.count = p[12];
    h.chunk = get16(p + 14);
    h.length = get32(p + 16);
    return true;
}

uint64_t flag(unsigned i) { return uint64_t(1) << i; }
uint64_t allOf(unsigned count) { return count >= 64 ? ~uint64_t(0) : flag(count) - 1; }
uint32_t bits(uint64_t v) { return static_cast<uint32_t>(__builtin_popcountll(v)); }

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

PeerAddress udpAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    uint8_t bytes[6] = {a, b, c, d, static_cast<uint8_t>(port >> 8), static_cast<uint8_t>(port)};
    return PeerAddress::fromBytes(bytes, 6);
}

bool parseUdpAddress(std::string_view text, uint16_t defaultPort, PeerAddress& out) {
    uint8_t ip[4];
    size_t i = 0;
    for (int part = 0; part < 4; part++) {
        if (part && (i >= text.size() || text[i++] != '.')) return false;
        unsigned v = 0;
        size_t digits = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 4) {
            v = v * 10 + unsigned(text[i++] - '0');
            digits++;
        }
        if (!digits || v > 255) return false;
        ip[part] = static_cast<uint8_t>(v);
    }
    unsigned port = defaultPort;
    if (i < text.size()) {
        if (text[i++] != ':' || i == text.size()) return false;
        port = 0;
        for (; i < text.size(); i++) {
            if (text[i] < '0' || text[i] > '9') return false;
            port = port * 10 + unsigned(text[i] - '0');
            if (port > 65535) return false;
        }
    }
    out = udpAddress(ip[0], ip[1], ip[2], ip[3], static_cast<uint16_t>(port));
    return true;
}

std::string formatUdpAddress(const PeerAddress& a) {
    if (a.length != 6) return std::string();
    char s[24];
    snprintf(s, sizeof(s), "%u.%u.%u.%u:%u", a.bytes[0], a.bytes[1], a.bytes[2], a.bytes[3],
             unsigned(a.bytes[4] << 8 | a.bytes[5]));
    return s;
}

UdpLink::UdpLink(Api* api, PacketFn send, DatagramEndpoint::ClockFn clock, Options options,
                 DatagramEndpoint::Options endpointOptions)
    : send_(std::move(send)),
      clock_(clock),
      options_(options),
      endpoint_(
          api, TransportKind::Udp,
          [this](const PeerAddress& to, std::string m, Delivery d) { return sendMessage(to, std::move(m), d); },
          std::move(clock), endpointOptions),
      api_(api) {
    if (options_.fragmentSize < 64) options_.fragmentSize = 64;
    size_t most = size_t(options_.fragmentSize) * kMaxFragments;
    if (options_.maxMessage > most) options_.maxMessage = most;
    scratch_.resize(kHeader + options_.fragmentSize);
}

UdpLink::~UdpLink() { running_ = false; }

void UdpLink::start(uint32_t nonce) {
    MutexGuard guard(mutex_);
    nonce_ = nonce;
    nextId_ = 1;
    peers_.clear();
    running_ = true;
    // Advertising nodes answer the query; ours goes with it.
    sendAdvert(PeerAddress(), kQuery);
}

void UdpLink::stop() {
    std::vector<PeerAddress> peers;
    {
        MutexGuard guard(mutex_);
        if (!running_) return;
        if (advertising_) sendAdvert(PeerAddress(), kBye);
        running_ = false;
        for (const Peer& p : peers_) peers.push_back(p.address);
        for (const Outgoing& o : outgoing_) peers.push_back(o.to);
        peers_.clear();
        outgoing_.clear();
        partials_.clear();
    }
    for (const PeerAddress& p : peers) endpoint_.forgetPeer(p, PeerLoss::Stopped);
}

void UdpLink::setName(std::string_view name) {
    MutexGuard guard(mutex_);
    name_.assign(name.data(), name.size());
}

void UdpLink::setDeviceId(const uint8_t mac[6]) {
    MutexGuard guard(mutex_);
    device_ = PeerAddress::fromBytes(mac, 6);
}

bool UdpLink::advertise() {
    if (!api_) return false;
    uint32_t hash = api_->schemaHash();
    {
        MutexGuard guard(mutex_);
        advertising_ = true;
        hash_ = hash;
        if (running_) sendAdvert(PeerAddress(), 0);
    }
    // From now on tick() re-advertises when the tree changes.
    endpoint_.autoAdvertise(
        [this](uint32_t h) {
            MutexGuard guard(mutex_);
            hash_ = h;
            if (running_) sendAdvert(PeerAddress(), 0);
        },
        hash);
    return true;
}

void UdpLink::mountPeers(Object& parent, uint32_t mirrorIntervalMs) {
    endpoint_.mountPeers(parent, mirrorIntervalMs);
    MutexGuard guard(mutex_);
    if (running_) sendAdvert(PeerAddress(), kQuery);
}

bool UdpLink::sendDatagram(const PeerAddress& to, const uint8_t* data, size_t len) {
    bool ok = send_ && send_(to, data, len);
    if (ok) stats_.sent++;  // refused ones (an interface UDP mustn't use, a full buffer) don't count
    return ok;
}

// Sends the fragments in `mask`; a reliable message asks for an
// acknowledgement with the last one.
void UdpLink::transmit(const PeerAddress& to, uint32_t id, const std::string& data, uint8_t count, uint64_t mask,
                       uint8_t flags) {
    unsigned last = 0;
    for (unsigned i = 0; i < count; i++) {
        if (mask & flag(i)) last = i;
    }
    size_t size = options_.fragmentSize;
    for (unsigned i = 0; i < count; i++) {
        if (!(mask & flag(i))) continue;
        Header h;
        h.type = kData;
        h.flags = static_cast<uint8_t>(flags | ((flags & kReliable) && i == last ? kAckRequest : 0));
        h.index = static_cast<uint8_t>(i);
        h.count = count;
        h.nonce = nonce_;
        h.id = id;
        h.chunk = options_.fragmentSize;
        h.length = static_cast<uint32_t>(data.size());
        size_t offset = i * size;
        size_t n = data.size() - offset < size ? data.size() - offset : size;
        writeHeader(scratch_.data(), h);
        if (n) memcpy(scratch_.data() + kHeader, data.data() + offset, n);
        sendDatagram(to, scratch_.data(), kHeader + n);
    }
}

bool UdpLink::sendMessage(const PeerAddress& to, std::string message, Delivery delivery) {
    if (!running_ || message.size() > options_.maxMessage) return false;
    size_t size = options_.fragmentSize;
    uint8_t count = static_cast<uint8_t>(message.empty() ? 1 : (message.size() + size - 1) / size);
    // Broadcasts can't be acknowledged; LatestOnly goes once too.
    bool broadcast = to.length == 0 || delivery == Delivery::BestEffort;
    bool reliable = delivery == Delivery::Reliable && !broadcast;
    MutexGuard guard(mutex_);
    if (!running_) return false;
    if (reliable && outgoing_.size() >= options_.maxOutgoing) return false;
    uint32_t id = nextId_++;
    if (!reliable) {
        // Copies of a broadcast keep its id: receivers apply the first.
        uint8_t copies = broadcast && options_.broadcastCopies ? options_.broadcastCopies : 1;
        for (uint8_t c = 0; c < copies; c++) transmit(to, id, message, count, allOf(count), broadcast ? kBroadcast : 0);
        return true;
    }
    uint32_t t = now();
    // Kept for retransmissions: taken, not copied (a large reply on a
    // fragmented heap had no room for a second block).
    outgoing_.push_back(Outgoing{to, id, std::move(message), count, 0, t + options_.ackTimeoutMs, options_.ackTimeoutMs,
                                 t + options_.sendTimeoutMs});
    const Outgoing& o = outgoing_.back();
    transmit(to, id, o.data, count, allOf(count), kReliable);
    return true;
}

void UdpLink::sendAck(const PeerAddress& to, uint32_t id, uint8_t status, uint64_t got) {
    uint8_t buf[kHeader + 8];
    Header h;
    h.type = kAck;
    h.index = status;
    h.nonce = nonce_;
    h.id = id;
    writeHeader(buf, h);
    size_t len = kHeader;
    if (status == kPartial) {
        put64(buf + kHeader, got);
        len += 8;
    }
    sendDatagram(to, buf, len);
}

// {"name":"...","schema":"<8 hex digits>","id":"<MAC, 12 hex digits>"}
void UdpLink::sendAdvert(const PeerAddress& to, uint8_t flags) {
    std::string payload;
    if (advertising_ && !(flags & kBye)) {
        StringSink sink(payload);
        JsonWriter w(sink);
        w.beginObject();
        w.key("name");
        w.string(name_);
        char hex[16];
        snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(hash_));
        w.key("schema");
        w.string(hex);
        if (device_.length == 6) {
            snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", device_.bytes[0], device_.bytes[1],
                     device_.bytes[2], device_.bytes[3], device_.bytes[4], device_.bytes[5]);
            w.key("id");
            w.string(hex);
        }
        w.endObject();
    }
    std::vector<uint8_t> buf(kHeader + payload.size());
    Header h;
    h.type = kAdvert;
    h.flags = flags;
    h.nonce = nonce_;
    writeHeader(buf.data(), h);
    if (!payload.empty()) memcpy(buf.data() + kHeader, payload.data(), payload.size());
    // Periodic advertisements repeat anyway; queries and goodbyes don't.
    uint8_t copies = to.length == 0 && flags && options_.broadcastCopies ? options_.broadcastCopies : 1;
    for (uint8_t c = 0; c < copies; c++) sendDatagram(to, buf.data(), buf.size());
    if (to.length == 0 && !(flags & kBye)) advertisedMs_ = now();
}

UdpLink::Peer* UdpLink::peerFor(const PeerAddress& from, uint32_t nonce, uint32_t t, bool& restarted) {
    for (Peer& p : peers_) {
        if (p.address != from) continue;
        if (p.nonce != nonce) {
            restarted = true;
            p.nonce = nonce;
            p.any = false;
            p.window = 0;
        }
        p.heardMs = t;
        return &p;
    }
    if (peers_.size() >= options_.maxPeers && !peers_.empty()) {
        size_t oldest = 0;
        for (size_t i = 1; i < peers_.size(); i++) {
            if (static_cast<int32_t>(peers_[i].heardMs - peers_[oldest].heardMs) < 0) oldest = i;
        }
        peers_.erase(peers_.begin() + static_cast<std::ptrdiff_t>(oldest));
    }
    Peer p;
    p.address = from;
    p.nonce = nonce;
    p.heardMs = t;
    peers_.push_back(p);
    return &peers_.back();
}

// Message ids grow by one per message; a 64-message window behind the newest
// one completed remembers which arrived. Older ones count as repeats.
bool UdpLink::seen(const Peer& p, uint32_t id) {
    if (!p.any) return false;
    int32_t d = static_cast<int32_t>(p.newest - id);
    if (d < 0) return false;
    if (d >= 64) return true;
    return (p.window & flag(static_cast<unsigned>(d))) != 0;
}

void UdpLink::markSeen(Peer& p, uint32_t id) {
    if (!p.any) {
        p.any = true;
        p.newest = id;
        p.window = 1;
        return;
    }
    int32_t d = static_cast<int32_t>(p.newest - id);
    if (d < 0) {
        uint32_t shift = id - p.newest;
        p.window = shift >= 64 ? 0 : p.window << shift;
        p.window |= 1;
        p.newest = id;
    } else if (d < 64) {
        p.window |= flag(static_cast<unsigned>(d));
    }
}

void UdpLink::dropPartials(const PeerAddress& from) {
    for (size_t i = 0; i < partials_.size();) {
        if (partials_[i].from == from) {
            partials_.erase(partials_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            i++;
        }
    }
}

// Evicts the oldest partial messages until `bytes` more fit the budget.
bool UdpLink::makeRoom(size_t bytes) {
    if (bytes > options_.reassemblyBytes) return false;
    for (;;) {
        size_t used = 0;
        for (const Partial& p : partials_) used += p.data.size();
        if (used + bytes <= options_.reassemblyBytes) return true;
        size_t oldest = 0;
        for (size_t i = 1; i < partials_.size(); i++) {
            if (static_cast<int32_t>(partials_[i].touchedMs - partials_[oldest].touchedMs) < 0) oldest = i;
        }
        partials_.erase(partials_.begin() + static_cast<std::ptrdiff_t>(oldest));
        stats_.dropped++;
    }
}

void UdpLink::handleData(const PeerAddress& from, Peer& peer, const uint8_t* data, size_t len, uint32_t t,
                         std::vector<Inbound>& out) {
    Header h;
    readHeader(data, len, h);
    const uint8_t* payload = data + kHeader;
    size_t n = len - kHeader;
    // Every fragment but the last holds `chunk` bytes, the last 1 to `chunk`.
    bool ok = h.count >= 1 && h.count <= kMaxFragments && h.index < h.count;
    if (ok && h.count == 1) {
        ok = n == h.length;
    } else if (ok) {
        uint64_t before = uint64_t(h.count - 1) * h.chunk;
        ok = h.chunk > 0 && h.length > before && h.length <= before + h.chunk &&
             n == (h.index + 1u < h.count ? h.chunk : h.length - before);
    }
    if (!ok) {
        stats_.dropped++;
        return;
    }
    bool reliable = h.flags & kReliable;
    if (h.length > options_.maxMessage) {
        stats_.dropped++;
        if (reliable) sendAck(from, h.id, kRejected, 0);
        return;
    }
    if (seen(peer, h.id)) {
        stats_.duplicates++;
        if (reliable && (h.flags & kAckRequest || h.count == 1)) sendAck(from, h.id, kComplete, 0);
        return;
    }
    if (h.count == 1) {
        markSeen(peer, h.id);
        if (reliable) sendAck(from, h.id, kComplete, 0);
        out.push_back(Inbound{from, std::string(reinterpret_cast<const char*>(payload), n), reliable,
                              (h.flags & kBroadcast) != 0});
        return;
    }
    Partial* p = nullptr;
    for (Partial& q : partials_) {
        if (q.from == from && q.nonce == h.nonce && q.id == h.id) p = &q;
    }
    if (p && (p->count != h.count || p->chunk != h.chunk || p->data.size() != h.length)) {
        stats_.dropped++;
        return;
    }
    if (!p) {
        // A heap too fragmented for the message: dropped, and the sender
        // tries again, rather than an abort.
        if (!makeRoom(h.length) || largestFreeBlock() < size_t(h.length) + 1024 || !heapAbove(h.length)) {
            stats_.dropped++;
            return;
        }
        partials_.push_back(Partial{from, h.nonce, h.id, std::string(h.length, '\0'), 0, h.chunk, h.count,
                                    static_cast<uint8_t>(h.flags & (kReliable | kBroadcast)), t});
        p = &partials_.back();
    }
    memcpy(&p->data[size_t(h.index) * h.chunk], payload, n);
    p->got |= flag(h.index);
    p->touchedMs = t;
    if (p->got == allOf(p->count)) {
        markSeen(peer, h.id);
        bool rel = p->flags & kReliable;
        if (rel) sendAck(from, h.id, kComplete, 0);
        out.push_back(Inbound{from, std::move(p->data), rel, (p->flags & kBroadcast) != 0});
        partials_.erase(partials_.begin() + (p - partials_.data()));
    } else if (reliable && (h.flags & kAckRequest)) {
        sendAck(from, h.id, kPartial, p->got);
    }
}

void UdpLink::handleAck(const PeerAddress& from, const uint8_t* data, size_t len, uint32_t t) {
    Header h;
    readHeader(data, len, h);
    for (size_t i = 0; i < outgoing_.size(); i++) {
        Outgoing& o = outgoing_[i];
        if (o.id != h.id || o.to != from) continue;
        if (h.index == kPartial && len >= kHeader + 8) {
            o.acked |= get64(data + kHeader) & allOf(o.count);
            uint64_t missing = allOf(o.count) & ~o.acked;
            if (missing) {
                // Only what is missing goes again, at once.
                transmit(o.to, o.id, o.data, o.count, missing, kReliable);
                stats_.retransmits += bits(missing);
                o.probeMs = t + o.backoffMs;
                return;
            }
        } else if (h.index == kRejected) {
            stats_.failed++;
        } else if (h.index != kComplete) {
            return;
        }
        outgoing_.erase(outgoing_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
    }
}

void UdpLink::packet(const PeerAddress& from, const uint8_t* data, size_t len) {
    Header h;
    if (!running_) return;
    if (!readHeader(data, len, h)) {
        MutexGuard guard(mutex_);
        stats_.dropped++;
        return;
    }
    std::vector<Inbound> inbound;
    bool restarted = false, announced = false, bye = false;
    std::string name;
    uint32_t hash = 0;
    PeerAddress device;
    {
        MutexGuard guard(mutex_);
        if (!running_ || h.nonce == nonce_) return;  // our own broadcast, looped back
        stats_.received++;
        uint32_t t = now();
        Peer* peer = peerFor(from, h.nonce, t, restarted);
        if (restarted) {
            // What we were sending to its previous run is moot.
            dropPartials(from);
            for (size_t i = 0; i < outgoing_.size();) {
                if (outgoing_[i].to == from) {
                    outgoing_.erase(outgoing_.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    i++;
                }
            }
        }
        if (h.type == kData) {
            handleData(from, *peer, data, len, t, inbound);
        } else if (h.type == kAck) {
            handleAck(from, data, len, t);
        } else if (h.type == kAdvert) {
            if (h.flags & kBye) {
                peer->advertised = false;
                bye = true;
            } else if (len > kHeader) {
                JsonDocument doc;
                if (!deserializeJson(doc, reinterpret_cast<const char*>(data + kHeader), len - kHeader) &&
                    doc["name"].is<const char*>() && doc["schema"].is<const char*>()) {
                    name = doc["name"].as<const char*>();
                    hash = static_cast<uint32_t>(strtoul(doc["schema"].as<const char*>(), nullptr, 16));
                    const char* id = doc["id"] | "";
                    uint8_t mac[6];
                    bool good = strlen(id) == 12;
                    for (int i = 0; good && i < 6; i++) {
                        int hi = hexDigit(id[2 * i]), lo = hexDigit(id[2 * i + 1]);
                        good = hi >= 0 && lo >= 0;
                        if (good) mac[i] = static_cast<uint8_t>(hi << 4 | lo);
                    }
                    if (good) device = PeerAddress::fromBytes(mac, 6);
                    peer->advertised = true;
                    announced = true;
                } else {
                    stats_.dropped++;
                }
            }
            if ((h.flags & kQuery) && advertising_) sendAdvert(from, 0);
        } else {
            stats_.dropped++;
        }
    }
    // A peer that restarted lost its calls and subscriptions: forget them.
    if (restarted) endpoint_.forgetPeer(from, PeerLoss::Restarted);
    for (Inbound& in : inbound) endpoint_.receive(in.from, std::move(in.data), in.reliable, in.broadcast);
    if (announced) endpoint_.peerSeen(from, name, hash, device);
    if (bye) endpoint_.forgetPeer(from, PeerLoss::Goodbye);
}

bool UdpLink::tick() {
    std::vector<std::pair<PeerAddress, PeerLoss>> lost;
    {
        MutexGuard guard(mutex_);
        uint32_t t = now();
        if (running_) {
            for (size_t i = 0; i < outgoing_.size();) {
                Outgoing& o = outgoing_[i];
                if (static_cast<int32_t>(t - o.deadlineMs) >= 0) {
                    // Unreachable: what else waits for it fails with it.
                    stats_.failed++;
                    PeerAddress to = o.to;
                    lost.emplace_back(to, PeerLoss::Failed);
                    for (size_t j = 0; j < outgoing_.size();) {
                        if (outgoing_[j].to == to) {
                            outgoing_.erase(outgoing_.begin() + static_cast<std::ptrdiff_t>(j));
                        } else {
                            j++;
                        }
                    }
                    i = 0;
                    continue;
                }
                if (static_cast<int32_t>(t - o.probeMs) >= 0) {
                    uint64_t missing = allOf(o.count) & ~o.acked;
                    transmit(o.to, o.id, o.data, o.count, missing, kReliable);
                    stats_.retransmits += bits(missing);
                    o.backoffMs = o.backoffMs * 2 > 1000 ? 1000 : o.backoffMs * 2;
                    o.probeMs = t + o.backoffMs;
                }
                i++;
            }
            for (size_t i = 0; i < partials_.size();) {
                if (t - partials_[i].touchedMs >= options_.sendTimeoutMs) {
                    partials_.erase(partials_.begin() + static_cast<std::ptrdiff_t>(i));
                    stats_.dropped++;
                } else {
                    i++;
                }
            }
            for (Peer& p : peers_) {
                if (p.advertised && t - p.heardMs >= options_.peerTimeoutMs) {
                    p.advertised = false;
                    lost.emplace_back(p.address, PeerLoss::Silent);
                }
            }
            if (advertising_ && t - advertisedMs_ >= options_.advertiseMs) sendAdvert(PeerAddress(), 0);
        }
    }
    for (const auto& p : lost) endpoint_.forgetPeer(p.first, p.second);
    return endpoint_.process() || !lost.empty();
}

UdpLink::Stats UdpLink::stats() const {
    MutexGuard guard(mutex_);
    return stats_;
}

}  // namespace tesser

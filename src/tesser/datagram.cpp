#include "tesser/datagram.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/api.h"
#include "tesser/call.h"
#include "tesser/client.h"
#include "tesser/envelope.h"
#include "tesser/file_session.h"
#include "tesser/remote.h"

// A datagram endpoint's core: taking messages from the link, process(), and
// sorting them out for its parts: the calls to peers (datagram_calls.cpp), the
// peers it knows of (datagram_peers.cpp), the nodes mounted for them
// (datagram_mounts.cpp), and serving them (datagram_server.cpp), see
// docs/DESIGN.md section 16.

namespace tesser {

DatagramEndpoint::DatagramEndpoint(Api* api, TransportKind kind, SendFn send, ClockFn clock, Options options)
    : api_(api),
      kind_(kind),
      incoming_("incoming", checked(options).queueBytes, checked(options).queueBytesPerPeer),
      send_(std::move(send)),
      clock_(std::move(clock)),
      options_(checked(options)),
      peers_(kind),
      calls_(*this),
      mounts_(*this),
      server_(*this) {}

DatagramEndpoint::Options DatagramEndpoint::checked(Options o) {
    if (o.maxMessage && o.queueBytesPerPeer && o.queueBytesPerPeer < o.maxMessage) o.queueBytesPerPeer = o.maxMessage;
    if (o.queueBytes && o.queueBytesPerPeer && o.queueBytes < o.queueBytesPerPeer) o.queueBytes = o.queueBytesPerPeer;
    return o;
}

size_t DatagramEndpoint::replyLimit() const {
    size_t limit = api_ ? api_->config().maxResponse : 0;
    size_t link = options_.maxMessage;
    if (!limit || (link && link < limit)) limit = link;
    return limit;
}

size_t DatagramEndpoint::peerLimit(const PeerAddress& peer) const {
    size_t limit = options_.maxMessage;
    auto lower = [&limit](size_t l) {
        if (l && (!limit || l < limit)) limit = l;
    };
    {
        MutexGuard guard(mutex_);
        if (const detail::PeerTable::Peer* k = peers_.known(peer)) lower(k->maxMessage);
    }
    if (peerLimitFn_) lower(peerLimitFn_(peer));  // the link's lock: not under ours
    return limit;
}

size_t DatagramEndpoint::replyLimitFor(const PeerAddress& peer) const {
    size_t limit = replyLimit();
    size_t theirs = peerLimit(peer);
    if (theirs && (!limit || theirs < limit)) limit = theirs;
    return limit;
}

// Pending handlers are dropped without being called: whatever they capture
// may already be gone. (A captured Pending still answers its own client.)
DatagramEndpoint::~DatagramEndpoint() {
    std::vector<detail::CallTable::Slot> dropped;  // destroyed after the lock, as by cancelCalls()
    {
        MutexGuard guard(mutex_);
        dropped = calls_.takeAll();
        mounts_.detachAll();
    }
    server_.dropAll();
}

uintptr_t budgetKey(const PeerAddress& p) {
    uint32_t h = 2166136261u;
    for (uint8_t i = 0; i < p.length; i++) h = (h ^ p.bytes[i]) * 16777619u;
    return h ? h : 1;
}

Reservation DatagramEndpoint::hold(const PeerAddress& from, std::string_view text, bool reliable, bool broadcast,
                                   bool heap) {
    Reservation r = incoming_.reserve(text.size(), budgetKey(from), true, heap, mallocPool(text.size()));
    if (!r) {
        {
            MutexGuard guard(mutex_);
            stats_.dropped++;
        }
        if (refusable(reliable, broadcast)) return r;  // the link refuses it: sent again
        // Answered, or its call failed, so nobody waits for a timeout: the
        // queue's own limit is queue_full, the heap low_memory (a relay then
        // asks for less, docs/DESIGN.md section 14).
        bool queueRoom = static_cast<bool>(incoming_.reserve(text.size(), budgetKey(from), true, false));
        server_.refuse(from, text, broadcast, heap && queueRoom ? Reason::LowMemory : Reason::QueueFull);
    }
    return r;
}

bool DatagramEndpoint::receive(const PeerAddress& from, const char* data, size_t len, bool reliable,
                               bool broadcast) {
    // The link's buffer (NowTP's) is only lent: copied if the budget, the
    // heap floor and the largest block allow it (docs/DESIGN.md section 15).
    // If not, the link refuses it (linkRefuses()), or a request is answered
    // busy and a reply fails its call.
    Reservation held = hold(from, std::string_view(data, len), reliable, broadcast, true);
    if (!held) return !refusable(reliable, broadcast);
    Incoming msg;
    msg.from = from;
    msg.text.assign(data, len);
    held.settle();
    msg.held = std::move(held);
    msg.reliable = reliable;
    msg.broadcast = broadcast;
    return queue(std::move(msg));
}

bool DatagramEndpoint::receive(const PeerAddress& from, std::string&& text, bool reliable, bool broadcast) {
    // Allocated already: counted, not checked against the heap.
    Reservation held = hold(from, text, reliable, broadcast, false);
    if (!held) return !refusable(reliable, broadcast);
    Incoming msg;
    msg.from = from;
    msg.text = std::move(text);
    msg.held = std::move(held);
    msg.reliable = reliable;
    msg.broadcast = broadcast;
    return queue(std::move(msg));
}

bool DatagramEndpoint::receiveOwned(const PeerAddress& from, void* buffer, size_t len, bool reliable,
                                    bool broadcast) {
    Incoming msg;
    msg.owned.reset(static_cast<char*>(buffer));  // freed even when refused
    Reservation held = hold(from, std::string_view(msg.owned.get(), len), reliable, broadcast, false);
    if (!held) return !refusable(reliable, broadcast);
    msg.from = from;
    msg.ownedLen = len;
    msg.held = std::move(held);
    msg.reliable = reliable;
    msg.broadcast = broadcast;
    return queue(std::move(msg));
}

bool DatagramEndpoint::queue(Incoming&& msg) {
    bool queued = false;
    {
        MutexGuard guard(mutex_);
        if (queue_.size() < options_.queueLimit) {
            queue_.push_back(std::move(msg));
            queued = true;
        } else {
            stats_.dropped++;
        }
    }
    if (queued) {
        if (onQueued_) onQueued_();
        return true;
    }
    if (refusable(msg.reliable, msg.broadcast)) return false;  // the link refuses it: sent again
    // Full: answer requests with "busy" so clients needn't wait for a timeout.
    server_.reject(msg.from, msg.view(), msg.broadcast, Status::Busy, Reason::QueueFull, "request queue full");
    return true;
}

bool DatagramEndpoint::process() {
    bool worked = false;
    std::vector<std::pair<PeerAddress, PeerLoss>> lost;
    std::vector<PeerAddress> unheard;
    std::vector<Failure> failures;
    {
        MutexGuard guard(mutex_);
        lost.swap(lostPeers_);
        unheard.swap(unheardPeers_);
        failures.swap(failures_);
    }
    for (const auto& p : lost) {
        dropPeer(p.first, p.second);
        worked = true;
    }
    for (const PeerAddress& p : unheard) {
        if (!deferLoss(p)) dropPeer(p, PeerLoss::Lost);
        worked = true;
    }
    for (const Failure& f : failures) {
        if (f.failure == SendFailure::Rejected) {
            calls_.fail(f.callId, Status::Internal, Reason::Rejected,
                        "the remote node refused the message (no listener on the port, or too large)");
        } else if (f.failure == SendFailure::AuthFailed) {
            dropPeer(f.peer, PeerLoss::AuthFailed);
        } else {
            unreachable(f.peer);
        }
        worked = true;
    }
    if (mounts_.applySeen()) worked = true;
    for (;;) {
        Incoming msg;
        {
            MutexGuard guard(mutex_);
            if (queue_.empty()) break;
            msg = std::move(queue_.front());
            queue_.erase(queue_.begin());
        }
        mounts_.markOnline(msg.from, true);
        handle(msg);
        worked = true;
    }
    size_t before = pendingCalls();
    uint32_t t = now();
    calls_.expire(t, false, nullptr);
    {
        detail::Mounter::Pinned remotes(mounts_);
        {
            MutexGuard guard(mutex_);
            remotes.all();
        }
        for (RemoteNode* r : remotes.nodes) r->tick(*this, t);
    }
    server_.dropFiles(nullptr, t);
    dropDeferred(t);
    checkAdvertisement(t);
    server_.checkIdle();
    return worked || pendingCalls() != before;
}

void DatagramEndpoint::nextTick(Deadline& d) {
    {
        MutexGuard guard(mutex_);
        if (!queue_.empty() || !lostPeers_.empty() || !unheardPeers_.empty() || !failures_.empty() ||
            mounts_.seenWaiting()) {
            d.in(0);
        }
        calls_.nextTick(d);
        for (const detail::PeerTable::Peer& k : peers_.peers()) {
            if (k.lossPending) d.at(k.heardMs + kLossGraceMs);
        }
    }
    server_.nextTick(d);
    if (publish_ && api_) {
        if (schemaRevision() != seenRevision_) {
            d.in(0);
        } else if (revisionPending_) {
            d.at(revisionAtMs_ + advertiseDebounceMs_);
        }
    }
    detail::Mounter::Pinned remotes(mounts_);
    {
        MutexGuard guard(mutex_);
        remotes.all();
    }
    for (RemoteNode* r : remotes.nodes) r->nextTick(*this, d);
}

namespace {

constexpr size_t kNone = std::string_view::npos;

size_t skipSpace(std::string_view s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    return i;
}

// Past the string whose opening quote is at `i`.
size_t skipString(std::string_view s, size_t i) {
    for (i++; i < s.size(); i++) {
        if (s[i] == '\\') {
            i++;
        } else if (s[i] == '"') {
            return i + 1;
        }
    }
    return kNone;
}

// Past the value starting at `i`.
size_t skipValue(std::string_view s, size_t i) {
    if (i >= s.size()) return kNone;
    if (s[i] == '"') return skipString(s, i);
    if (s[i] == '{' || s[i] == '[') {
        int depth = 0;
        while (i < s.size()) {
            char c = s[i];
            if (c == '"') {
                i = skipString(s, i);
                if (i == kNone) return kNone;
                continue;
            }
            if (c == '{' || c == '[') depth++;
            if ((c == '}' || c == ']') && --depth == 0) return i + 1;
            i++;
        }
        return kNone;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\n' &&
           s[i] != '\r' && s[i] != '\t') {
        i++;
    }
    return i;
}

}  // namespace

// Each key of the JSON object `json` (valid JSON) to `each`, in order, until
// it returns false; false if `json` isn't an object.
bool jsonKeys(std::string_view json, const std::function<bool(std::string_view key)>& each) {
    size_t i = skipSpace(json, 0);
    if (i >= json.size() || json[i] != '{') return false;
    i = skipSpace(json, i + 1);
    if (i < json.size() && json[i] == '}') return true;
    for (;;) {
        if (i >= json.size() || json[i] != '"') return false;
        size_t end = skipString(json, i);
        if (end == kNone) return false;
        if (!each(json.substr(i + 1, end - i - 2))) return true;
        i = skipSpace(json, end);
        if (i >= json.size() || json[i] != ':') return false;
        size_t valueEnd = skipValue(json, skipSpace(json, i + 1));
        if (valueEnd == kNone) return false;
        i = skipSpace(json, valueEnd);
        if (i < json.size() && json[i] == '}') return true;
        if (i >= json.size() || json[i] != ',') return false;
        i = skipSpace(json, i + 1);
    }
}

// The text of member `key` of the JSON object `json` (valid JSON).
bool jsonMember(std::string_view json, std::string_view key, std::string_view& out) {
    size_t i = skipSpace(json, 0);
    if (i >= json.size() || json[i] != '{') return false;
    i = skipSpace(json, i + 1);
    if (i < json.size() && json[i] == '}') return false;
    for (;;) {
        if (i >= json.size() || json[i] != '"') return false;
        size_t end = skipString(json, i);
        if (end == kNone) return false;
        std::string_view k = json.substr(i + 1, end - i - 2);
        i = skipSpace(json, end);
        if (i >= json.size() || json[i] != ':') return false;
        i = skipSpace(json, i + 1);
        size_t valueEnd = skipValue(json, i);
        if (valueEnd == kNone) return false;
        if (k == key) {
            out = json.substr(i, valueEnd - i);
            return true;
        }
        i = skipSpace(json, valueEnd);
        if (i >= json.size() || json[i] != ',') return false;
        i = skipSpace(json, i + 1);
    }
}

// Out of handle(), whose frame sits under every request a peer makes: what
// only a notification needs stays off it (recursive walkers and handlers
// run on top, in a task of 6-8 KB).
__attribute__((noinline)) void DatagramEndpoint::handleNotification(JsonObjectConst env, const Incoming& msg,
                                                                    std::string_view text) {
    {
        detail::Mounter::Pinned remotes(mounts_);
        {
            MutexGuard guard(mutex_);
            remotes.all();
        }
        // Remote nodes take what concerns their mirror; the application's
        // handler and clients of the peer still see everything.
        for (RemoteNode* r : remotes.nodes) {
            if (r->handleNotification(*this, msg.from, env)) break;
        }
    }
    std::vector<std::shared_ptr<ClientInbox>> inboxes;
    {
        MutexGuard guard(mutex_);
        for (auto& c : clients_) {
            if (c.first == msg.from) inboxes.push_back(c.second);
        }
    }
    for (auto& inbox : inboxes) inbox->notification(std::string(text));
    if (onNotification_) onNotification_(msg.from, env);
}

void DatagramEndpoint::handle(Incoming& msg) {
    if (calls_.takeRaw(msg.from, msg.view())) return;
    // Parsed, a large message takes about as much again: only above the
    // heap floor (dropped otherwise, like a lost datagram).
    std::string_view text = msg.view();
    Reservation parsing;  // the document, about as large as the text
    if (text.size() > 1024) {
        // An upload's document leaves its data out: small. In PSRAM when
        // the chip has it: a remote's whole tree or schema, say.
        parsing = memoryBudget().reserve(isUploadEnvelope(text) ? 512 : 2 * text.size(), 0, false, true, Pool::Large);
        if (!parsing) {
            {
                MutexGuard guard(mutex_);
                stats_.dropped++;
            }
            server_.refuse(msg.from, msg.view(), msg.broadcast, Reason::LowMemory);
            return;
        }
    }
    // Responses and notifications nest as deep as the peer wrote them;
    // requests are held to the same limit as on other transports below.
    JsonDocument doc(jsonAllocator(parsing ? parsing.pool() : Pool::Internal));
    // An upload's data is read from the text, not copied into the document.
    DeserializationError err =
        isUploadEnvelope(text)
            ? parseJson(doc, text, kWrittenNesting, uploadFilter())
            : parseJson(doc, text, kWrittenNesting);
    if (err || !doc.is<JsonObjectConst>()) {
        {
            MutexGuard guard(mutex_);
            stats_.dropped++;
        }
        // The heap, not the message: answered busy (or its call failed).
        if (err == DeserializationError::NoMemory) server_.refuse(msg.from, text, msg.broadcast, Reason::LowMemory);
        return;
    }
    JsonObjectConst env = doc.as<JsonObjectConst>();

    // Response to one of our calls.
    if (env["status"].is<const char*>()) return calls_.handleResponse(env, msg.from);

    // Notification from a node we subscribed to: never answered.
    std::string_view op = env["op"].is<const char*>() ? std::string_view(env["op"].as<const char*>()) : "";
    if (op == "change" || op == "event") return handleNotification(env, msg, text);

    // Request.
    if (op.empty() || !api_) {
        MutexGuard guard(mutex_);
        stats_.dropped++;
        return;
    }
    server_.serve(env, msg);
}

void DatagramEndpoint::addClient(const PeerAddress& peer, std::shared_ptr<ClientInbox> inbox) {
    MutexGuard guard(mutex_);
    clients_.emplace_back(peer, std::move(inbox));
}

void DatagramEndpoint::removeClient(const ClientInbox* inbox) {
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < clients_.size(); i++) {
        if (clients_[i].second.get() == inbox) {
            clients_.erase(clients_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

DatagramEndpoint::Stats DatagramEndpoint::stats() const {
    MutexGuard guard(mutex_);
    return stats_;
}

}  // namespace tesser

#include "tesser/api.h"

#include <stdio.h>
#include <stdlib.h>

#include <memory>
#include <string>
#include <vector>

#include "tesser/detail/api_handler.h"
#include "tesser/envelope.h"
#include "tesser/file.h"
#include "tesser/remote.h"

namespace tesser {

bool parseOp(std::string_view s, Op& out) {
    if (s == "get") {
        out = Op::Get;
    } else if (s == "set") {
        out = Op::Set;
    } else if (s == "sub") {
        out = Op::Subscribe;
    } else if (s == "unsub") {
        out = Op::Unsubscribe;
    } else {
        return false;
    }
    return true;
}

bool splitOp(std::string_view op, std::string_view& name, Access& ceiling) {
    size_t at = op.find('@');
    name = op.substr(0, at);
    ceiling = Access::Admin;
    if (at == std::string_view::npos) return true;
    if (parseAccess(op.substr(at + 1), ceiling)) return true;
    name = op;
    return false;
}

bool parseView(std::string_view s, View& out) {
    if (s == "value") {
        out = View::Value;
    } else if (s == "schema") {
        out = View::Schema;
    } else if (s == "hash") {
        out = View::Hash;
    } else if (s == "info") {
        out = View::Info;
    } else {
        return false;
    }
    return true;
}

const char* toString(TransportKind kind) {
    switch (kind) {
        case TransportKind::Local: return "local";
        case TransportKind::Serial: return "serial";
        case TransportKind::Http: return "http";
        case TransportKind::NowTP: return "nowtp";
        case TransportKind::WebSocket: return "websocket";
        case TransportKind::Udp: return "udp";
        case TransportKind::Ble: return "ble";
    }
    return "local";
}

void Api::setDeviceId(const uint8_t mac[6]) {
    MutexGuard guard(mutex_);
    memcpy(deviceId_, mac, 6);
    hasDeviceId_ = true;
}

bool Api::deviceId(uint8_t mac[6]) const {
    {
        MutexGuard guard(mutex_);
        if (hasDeviceId_) {
            memcpy(mac, deviceId_, 6);
            return true;
        }
    }
    return stationMac(mac);
}

std::string Api::deviceIdHex() const {
    uint8_t mac[6];
    if (!deviceId(mac)) return std::string();
    char hex[13];
    snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return hex;
}

bool parseTransportKind(std::string_view s, TransportKind& out) {
    static const TransportKind all[] = {TransportKind::Local, TransportKind::Serial,    TransportKind::Http,
                                        TransportKind::NowTP, TransportKind::WebSocket, TransportKind::Udp,
                                        TransportKind::Ble};
    for (TransportKind k : all) {
        if (s == toString(k)) {
            out = k;
            return true;
        }
    }
    return false;
}

const char* toString(NetInterface iface) {
    switch (iface) {
        case NetInterface::Any: return "";
        case NetInterface::Ethernet: return "ethernet";
        case NetInterface::Wifi: return "wifi";
        case NetInterface::Ap: return "ap";
    }
    return "";
}

bool parseNetInterface(std::string_view s, NetInterface& out) {
    static const NetInterface all[] = {NetInterface::Ethernet, NetInterface::Wifi, NetInterface::Ap};
    for (NetInterface i : all) {
        if (s == toString(i)) {
            out = i;
            return true;
        }
    }
    return false;
}

const char* toString(Access a) {
    switch (a) {
        case Access::Public: return "public";
        case Access::User: return "user";
        case Access::Admin: return "admin";
    }
    return "public";
}

bool parseAccess(std::string_view s, Access& out) {
    if (s == "public") {
        out = Access::Public;
    } else if (s == "user") {
        out = Access::User;
    } else if (s == "admin") {
        out = Access::Admin;
    } else {
        return false;
    }
    return true;
}

const char* toString(Op op) {
    switch (op) {
        case Op::Get: return "get";
        case Op::Set: return "set";
        case Op::Subscribe: return "sub";
        case Op::Unsubscribe: return "unsub";
    }
    return "get";
}

bool Api::pendingBegin() {
    int n = pending_.fetch_add(1);
    if (n >= config_.maxPending) {
        pending_.fetch_sub(1);
        return false;
    }
    return true;
}

void Api::authorize(Authorizer fn) {
    MutexGuard guard(mutex_);
    authorizer_ = std::move(fn);
}

void Api::authenticate(TokenCheck fn) {
    MutexGuard guard(mutex_);
    tokenCheck_ = std::move(fn);
}

void Api::authenticate(std::function<Access(std::string_view token)> fn) {
    if (!fn) return authenticate(TokenCheck());
    authenticate([fn = std::move(fn)](std::string_view token, const Client&) { return fn(token); });
}

// Under the API lock: checks typically read keys or users from the tree.
Access Api::tokenAccess(std::string_view token, const Client& client) const {
    MutexGuard guard(mutex_);
    return tokenCheck_ ? tokenCheck_(token, client) : Access::Public;
}

bool ClientToken::recheck(const Api& api, const Client& client, Access& level) const {
    if (fixed) return false;
    level = api.tokenAccess(token, client);
    return true;
}

bool Api::belowRemote(std::string_view path) const {
    MutexGuard guard(mutex_);
    bool remote = false;
    detail::walkObjects(*this, path, [&remote](const Node& n) {
        remote = n.type() == NodeType::Remote;
        return !remote;  // what is below it is the peer's
    });
    return remote;
}

void Api::handle(const Request& request, Reply& reply) {
    if (request.client.level() > request.query.ceiling) {
        // Asked to act at a lower level (docs/DESIGN.md section 13.1).
        Request capped = request;
        capped.client.cap(request.query.ceiling);
        return handle(capped, reply);
    }
    if (config_.queued && enqueue(request, reply)) return;
    handleNow(request, reply);
}

FileNode* Api::fileNode(const Request& request, Reply& reply, bool& notFile,
                        std::vector<std::unique_ptr<Node>>& elements, RemoteTarget* remote) {
    MutexGuard guard(mutex_);
    notFile = false;
    return detail::Handler(*this, request, reply).file(notFile, elements, remote);
}

bool Api::claimTransfer(std::string_view path, TransferHolder* holder) {
    MutexGuard guard(mutex_);
    for (const Transfer& t : transfers_) {
        if (t.path != path) continue;
        // A client that went quiet without going away (a message transport
        // has no connection to lose) loses its transfer to the next one.
        // Expiring releases the claim, so the loop must end here.
        if (!t.holder || !t.holder->expireIfIdle(millis32(), config_.transferIdleMs)) return false;
        break;
    }
    transfers_.push_back(Transfer{std::string(path), holder});
    return true;
}

void Api::releaseTransfer(std::string_view path) {
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < transfers_.size(); i++) {
        if (transfers_[i].path == path) {
            transfers_.erase(transfers_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

void Api::handleNow(const Request& request, Reply& reply) {
    {
        MutexGuard guard(mutex_);
        const Client* outer = current_;
        current_ = &request.client;
        uint32_t before = currentGeneration();
        detail::Handler(*this, request, reply).run();
        if (request.op == Op::Set && !request.query.persist && unsavedHook_) unsavedHook_(*this, before);
        current_ = outer;
    }
    // What a streamed reply still holds goes out without the lock, unless
    // the caller holds it (docs/DESIGN.md section 9).
    if (reply.wantsDrain() && !mutex_.heldByMe()) reply.drain();
}

void detail::handleNested(Api& api, const Request& request, Reply& reply) {
    MutexGuard guard(api.mutex());
    const Client* outer = api.current_;
    api.current_ = &request.client;
    std::unique_ptr<detail::Handler> handler(new detail::Handler(api, request, reply));
    handler->run();
    api.current_ = outer;
}

// ---- queued mode ------------------------------------------------------------

namespace {
// Lets a queued request's action defer: the detached reply changes hands.
class QueuedReply : public Reply {
public:
    explicit QueuedReply(Reply* inner) : inner_(inner) {}
    Sink& begin(Status s) override { return inner_->begin(s); }
    void end() override { inner_->end(); }
    bool rollback() override { return inner_->rollback(); }
    Reply* detach() override {
        if (transferred) return nullptr;
        transferred = true;
        return inner_;
    }
    bool transferred = false;

private:
    Reply* inner_;
};
}  // namespace

bool Api::enqueue(const Request& request, Reply& reply) {
    {
        MutexGuard guard(queueMutex_);
        if (queue_.size() >= config_.maxQueued) {
            writeError(reply, Status::Busy, Reason::QueueFull, request.path, "request queue full");
            return true;
        }
    }
    // The copies kept until poll() runs it, counted (docs/DESIGN.md section 15).
    size_t bytes = request.path.size() + request.query.keys.size() + request.query.exclude.size() +
                   (request.body.isNull() ? 0 : measureJson(request.body)) + sizeof(QueuedRequest);
    Reservation held = memoryBudget().reserve(bytes, 0, false);
    if (!held) {
        writeError(reply, Status::Busy, Reason::LowMemory, request.path, "not enough memory to queue the request");
        return true;
    }
    Reply* detached = reply.detach();
    if (!detached) return false;  // the transport can't wait: handle inline
    auto* q = new QueuedRequest();
    q->request = request;
    q->path.assign(request.path.data(), request.path.size());
    q->keys.assign(request.query.keys.data(), request.query.keys.size());
    q->exclude.assign(request.query.exclude.data(), request.query.exclude.size());
    if (!request.body.isNull()) serializeJson(request.body, q->body);
    q->request.path = q->path;
    q->request.query.keys = q->keys;
    q->request.query.exclude = q->exclude;
    q->request.body = JsonVariantConst();
    q->reply = detached;
    held.settle();
    q->held = std::move(held);
    MutexGuard guard(queueMutex_);
    queue_.push_back(q);
    return true;
}

void Api::runQueued() {
    while (queuedRequests()) {
        // Taken out of the queue under the API lock, and run before letting
        // go of it: dropSubscriber() discards the client's requests still
        // queued, then waits for this lock, so a request already taken out
        // never runs against a client destroyed meanwhile (its subscriber).
        MutexGuard api(mutex_);
        QueuedRequest* q;
        {
            MutexGuard guard(queueMutex_);
            if (queue_.empty()) return;
            q = queue_.front();
            queue_.erase(queue_.begin());
        }
        // The body parsed once already, so only memory can run out here.
        JsonDocument doc;
        bool parsed = q->body.empty() || !parseJson(doc, q->body, kWrittenNesting);
        q->request.body = doc.as<JsonVariantConst>();
        QueuedReply reply(q->reply);
        if (parsed) {
            handleNow(q->request, reply);
        } else {
            writeError(reply, Status::Internal, Reason::LowMemory, q->request.path, "out of memory");
        }
        if (!reply.transferred) q->reply->release();
        delete q;
    }
}

bool secretEquals(std::string_view presented, std::string_view expected) {
    // Every byte of the longer one is read, whatever matched before: no
    // early exit for the compiler to turn the loop into.
    size_t n = presented.size() > expected.size() ? presented.size() : expected.size();
    unsigned diff = presented.size() == expected.size() ? 0u : 1u;
    for (size_t i = 0; i < n; i++) {
        unsigned a = i < presented.size() ? static_cast<unsigned char>(presented[i]) : 0u;
        unsigned b = i < expected.size() ? static_cast<unsigned char>(expected[i]) : 0u;
        diff |= a ^ b;
    }
    return diff == 0;
}

uint32_t Api::schemaHash() {
    StringReply reply;
    Request req;
    req.path = "/";
    req.query.view = View::Hash;
    req.client.authenticated = true;  // internal
    handleNow(req, reply);
    // "1a2b3c4d", quoted
    return reply.body.size() == 10 ? static_cast<uint32_t>(strtoul(reply.body.c_str() + 1, nullptr, 16)) : 0;
}

size_t Api::queuedRequests() const {
    MutexGuard guard(queueMutex_);
    return queue_.size();
}

size_t Api::queuedRequests(const Subscriber* subscriber) const {
    MutexGuard guard(queueMutex_);
    size_t n = 0;
    for (const QueuedRequest* q : queue_) n += q->request.subscriber == subscriber;
    return n;
}

}  // namespace tesser

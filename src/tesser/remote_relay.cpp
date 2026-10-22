#include "tesser/remote.h"

#include <stdio.h>

#include "tesser/api.h"
#include "tesser/budget.h"
#include "tesser/detail/remote_json.h"
#include "tesser/envelope.h"
#include "tesser/file.h"
#include "tesser/file_session.h"

// File transfers relayed through a remote node to its peer (docs/DESIGN.md
// section 14).

namespace tesser {

namespace {

constexpr int kAttempts = 3;            // a call whose reply timed out is sent twice more
constexpr size_t kFallbackChunk = 192;  // a peer that gave no chunk size

// An envelope's op for a client at `level`: "upload", "upload@user"
// (docs/DESIGN.md section 13.1).
std::string opFor(const char* op, Access level) {
    std::string s(op);
    if (level < Access::Admin) {
        s += '@';
        s += toString(level);
    }
    return s;
}

// Strict (docs/DESIGN.md section 10.1.2): a node that doesn't know an
// option refuses the envelope rather than ignore it.
std::string fieldsOf(const std::function<void(JsonWriter&)>& write) {
    std::string text;
    StringSink sink(text);
    JsonWriter w(sink);
    w.beginObject();
    write(w);
    w.key("v");
    w.integer(kProtocolVersion);
    w.endObject();
    return text.substr(1, text.size() - 2);  // the members, without the braces
}

// An envelope's builder (DatagramEndpoint::requestBuilt()) from its members.
std::shared_ptr<DatagramEndpoint::Builder> builderOf(std::string fields) {
    return std::make_shared<DatagramEndpoint::Builder>(
        [fields = std::move(fields)](uint32_t id, DatagramEndpoint::EnvelopeOut& out) {
            char head[20];
            int n = snprintf(head, sizeof(head), "{\"id\":%lu,", static_cast<unsigned long>(id));
            if (!out.reserve(static_cast<size_t>(n) + fields.size() + 1)) return false;
            out.append(head, static_cast<size_t>(n));
            out.append(fields);
            out.append("}", 1);
            return true;
        });
}

class RemoteFile : public FileRelay, public TransferHolder, public std::enable_shared_from_this<RemoteFile> {
public:
    RemoteFile(Api& api, RemoteNode& node, std::string remotePath, std::string localBase, std::string remoteBase,
               std::string localPath, Access level)
        : api_(api),
          node_(node),
          remotePath_(std::move(remotePath)),
          localBase_(std::move(localBase)),
          remoteBase_(std::move(remoteBase)),
          localPath_(std::move(localPath)),
          get_(opFor("get", level)),
          upload_(opFor("upload", level)),
          download_(opFor("download", level)) {
        node_.relayHeld(true);  // not evicted meanwhile
        // The route the transfer goes by, start to end: the peer keeps a
        // transfer per client, and the same gateway on another link is
        // another client (docs/DESIGN.md section 14).
        RemoteNode::Route route;
        if (node.route(route)) {
            endpoint_ = route.endpoint;
            peer_ = route.peer;
        }
        // The claim is on the peer, under a key no path can have.
        char hex[24];
        snprintf(hex, sizeof(hex), "@%lx:", static_cast<unsigned long>(reinterpret_cast<uintptr_t>(endpoint_)));
        key_ = hex;
        for (uint8_t i = 0; i < peer_.length; i++) {
            snprintf(hex, sizeof(hex), "%02x", peer_.bytes[i]);
            key_ += hex;
        }
    }
    // Owners close() first; this only covers a relay dropped while claimed.
    ~RemoteFile() override {
        if (claimed_) api_.releaseTransfer(key_);
        node_.relayHeld(false);
    }

    bool claim(Reply& reply) override {
        MutexGuard guard(mutex_);
        if (!claimed_ && !api_.claimTransfer(key_, this)) {
            writeError(reply, Status::Busy, Reason::TransferBusy, localPath_, "another transfer to this node is in progress");
            return false;
        }
        claimed_ = true;
        activeMs_ = millis32();
        return true;
    }

    bool knownFile(bool& isFile) const override { return node_.knownFile(remotePath_, isFile); }
    void learnedFile(bool isFile) override { node_.learnedFile(remotePath_, isFile); }

    void probe(Done done) override {
        std::string path = remotePath_;
        call(builderOf(fieldsOf([&](JsonWriter& w) {
                 w.key("op");
                 w.string(get_);
                 w.key("path");
                 w.string(path);
                 w.key("view");
                 w.string("schema");
                 w.key("depth");
                 w.integer(0);
             })),
             std::move(done));
    }

    void openUpload(uint64_t size, std::string_view filename, Done done) override {
        {
            MutexGuard guard(mutex_);
            size_ = size;
            received_ = 0;
            pending_.clear();
        }
        std::string path = remotePath_;
        auto self = shared_from_this();
        call(builderOf(fieldsOf([&](JsonWriter& w) {
                 w.key("op");
                 w.string(upload_);
                 w.key("path");
                 w.string(path);
                 w.key("size");
                 w.uinteger(size);
                 if (!filename.empty()) {
                     w.key("filename");
                     w.string(filename);
                 }
             })),
             [self, done](Status s, JsonVariantConst body) {
                 if (s == Status::Ok) {
                     MutexGuard guard(self->mutex_);
                     if (self->size_ == 0) {
                         self->finishLocked();  // nothing to send: that was the upload's reply
                     } else {
                         self->chunk_ = body["chunk"].as<size_t>();
                         self->received_ = body["received"].as<uint64_t>();
                     }
                 } else {
                     self->failed(s);
                 }
                 done(s, body);
             });
    }

    void write(std::vector<uint8_t> data, Done done) override {
        bool room = true;
        {
            MutexGuard guard(mutex_);
            // Grown to its exact need, within the byte budget (docs/DESIGN.md
            // section 15); else the chunk is refused, for the client to send
            // again.
            size_t need = pending_.size() + data.size();
            if (pending_.empty()) {
                pending_ = std::move(data);  // taken over, not copied
                data.clear();
            } else if (need > pending_.capacity()) {
                Reservation growth = memoryBudget().reserve(need, 0, true, true, mallocPool(need));
                room = static_cast<bool>(growth);
                if (room) pending_.reserve(need);
            }
            if (room) pending_.insert(pending_.end(), data.begin(), data.end());
        }
        if (!room) return lowMemory(done);
        pump(std::move(done));
    }

    void download(uint64_t offset, size_t length, Chunk done) override {
        // A part is at most this node's upload chunk, as a relayed upload's:
        // the remote's reply is held here whole, and in the remote's own
        // reply buffer (docs/DESIGN.md section 14).
        size_t cap = FileSession::uploadChunk(api_.config());
        {
            MutexGuard guard(mutex_);
            if (limit_ && cap > limit_) cap = limit_;
        }
        if (!length || length > cap) length = cap;
        // A reply this link takes.
        size_t own = endpoint_ ? endpoint_->maxMessage() : 0;
        if (own && FileSession::chunkFor(own) < length) length = FileSession::chunkFor(own);
        // And what this node's heap holds now, the reply and its way on.
        length = FileSession::chunkInRoom(length, FileSession::kRelayedDownloadThirds);
        std::string path = remotePath_;
        auto self = shared_from_this();
        // The reply's fields apart from its data, which stays in the text.
        auto took = [self, done, offset, length](Status s, JsonVariantConst body, std::string_view data) {
            if (self->smaller(s, body, length)) {
                self->download(offset, length, done);  // asked again, for less
                return;
            }
            if (s == Status::Ok) {
                MutexGuard guard(self->mutex_);
                if (body["chunk"].is<size_t>()) self->chunk_ = body["chunk"].as<size_t>();
                // The end: the peer is free again, though the last chunk
                // can still be asked for again.
                if (body["end"] == true) {
                    self->finished_ = true;
                    self->release();
                }
            } else {
                self->failed(s);
            }
            done(s, body, data);
        };
        auto raw = std::make_shared<DatagramEndpoint::RawHandler>([took](std::string_view text) {
            JsonDocument filter;
            for (const char* k : {"size", "filename", "contentType", "chunk", "offset", "end"}) filter[k] = true;
            JsonDocument body;
            std::string_view data;
            bool bad = deserializeJson(body, text.data(), text.size(), DeserializationOption::Filter(filter)) ||
                       !body.is<JsonObjectConst>();
            // A JSON string, without escapes (base64).
            if (!bad && jsonMember(text, "data", data)) {
                bad = data.size() < 2 || data.front() != '"' || data.back() != '"';
                if (!bad) data = data.substr(1, data.size() - 2);
            }
            if (bad) {
                JsonDocument error;
                error["error"] = "internal";
                error["message"] = "bad download reply";
                took(Status::Internal, error.as<JsonVariantConst>(), std::string_view());
                return;
            }
            took(Status::Ok, body.as<JsonVariantConst>(), data);
        });
        call(builderOf(fieldsOf([&](JsonWriter& w) {
                 w.key("op");
                 w.string(download_);
                 w.key("path");
                 w.string(path);
                 if (offset) {
                     w.key("offset");
                     w.uinteger(offset);
                 }
                 if (length) {
                     w.key("length");
                     w.uinteger(length);
                 }
             })),
             [took](Status s, JsonVariantConst body) {
                 JsonString data = body["data"].as<JsonString>();
                 took(s, body, std::string_view(data.c_str() ? data.c_str() : "", data.size()));
             },
             0, raw);
    }

    void close() override {
        MutexGuard guard(mutex_);
        closed_ = true;
        release();
    }

    void writeReply(Reply& reply, Status status, JsonVariantConst body) const override {
        detail::writeMapped(reply, status, body, localBase_, remoteBase_);
    }

    uint64_t received() const override {
        MutexGuard guard(mutex_);
        return received_;
    }
    size_t chunk() const override {
        MutexGuard guard(mutex_);
        return chunk_;
    }
    bool busy() const override {
        MutexGuard guard(mutex_);
        return inFlight_;
    }
    bool finished() const override {
        MutexGuard guard(mutex_);
        return finished_;
    }
    bool open() const override {
        MutexGuard guard(mutex_);
        return !closed_;
    }
    bool expired() const override {
        MutexGuard guard(mutex_);
        return expired_;
    }
    uint32_t patienceMs() const override {
        uint32_t t = node_.timeoutMs();
        if (!t) t = routeUsable() ? endpoint_->timeoutMs() : 3000;
        return t * kAttempts + 1000;
    }

    bool expireIfIdle(uint32_t nowMs, uint32_t idleMs) override {
        // Never waits: the caller holds the API lock, which claim() and
        // release() take after this mutex.
        if (!idleMs || !mutex_.tryLock()) return false;
        bool idle = claimed_ && !inFlight_ && nowMs - activeMs_ >= idleMs;
        if (idle) {
            closed_ = true;
            expired_ = true;
            release();
        }
        mutex_.unlock();
        return idle;
    }

private:
    // Sends one envelope by the transfer's route, again on a timeout while
    // the relay is open and the route online. Nothing is sent once the
    // route is offline or gone, even if the node has another: there, the
    // peer has no transfer from this gateway (`timeout`, which ends the
    // relay; the peer drops its side when that link loses this node, or
    // after Config::transferIdleMs).
    // With `raw`, an "ok" reply's body goes to it unparsed instead.
    void call(std::shared_ptr<DatagramEndpoint::Builder> build, Done done, int attempt = 0,
              std::shared_ptr<DatagramEndpoint::RawHandler> raw = nullptr) {
        if (!routeUsable()) {
            JsonDocument error;
            error["error"] = "timeout";
            error["reason"] = "peer_offline";
            error["path"] = remoteBase_.empty() ? std::string("/") : remoteBase_;
            error["message"] = node_.online() ? "the route the transfer started on is offline" : "peer offline";
            done(Status::Timeout, error.as<JsonVariantConst>());
            return;
        }
        {
            MutexGuard guard(mutex_);
            inFlight_ = true;
            activeMs_ = millis32();
        }
        DatagramEndpoint* ep = endpoint_;
        auto self = shared_from_this();
        noRoom_ = false;
        DatagramEndpoint::RawHandler rawOk;
        if (raw) {
            rawOk = [self, raw](std::string_view body) {
                self->landed();
                (*raw)(body);
            };
        }
        bool sent = ep && ep->requestBuilt(
                              peer_, *build,
                              [self, build, done, attempt, raw](Status s, JsonVariantConst body) {
                                  if (s == Status::Timeout && attempt + 1 < kAttempts && self->open() &&
                                      self->routeUsable()) {
                                      self->call(build, done, attempt + 1, raw);
                                      return;
                                  }
                                  self->landed();
                                  done(s, body);
                              },
                              node_.timeoutMs(), &node_, std::move(rawOk));
        if (!sent) {
            landed();
            if (noRoom_) return lowMemory(done, false);  // the envelope had no room: its caller decides
            done(Status::Busy, JsonVariantConst());
        }
    }

    // The transfer's route is online, or its peer was heard so lately that
    // the link's loss of it waits (DatagramEndpoint::linkLost()).
    bool routeUsable() const {
        return node_.routeUp(endpoint_, peer_) || (endpoint_ && endpoint_->lossPending(peer_));
    }

    void landed() {
        MutexGuard guard(mutex_);
        inFlight_ = false;
        activeMs_ = millis32();
    }

    // Upload: sends the next part of pending_.
    void pump(Done done) {
        std::shared_ptr<DatagramEndpoint::Builder> build;
        uint64_t offset = 0;
        size_t part = 0;
        bool closed, idle;
        // An envelope the peer takes (its advertised limit), asked before
        // this relay's lock: the link may take its own.
        size_t takes = endpoint_ && routeUsable() ? endpoint_->peerLimit(peer_) : 0;
        {
            MutexGuard guard(mutex_);
            closed = closed_;
            idle = pending_.empty();
            if (!closed && !idle) {
                offset = received_;
                part = chunk_ ? chunk_ : kFallbackChunk;
                if (limit_ && part > limit_) part = limit_;
                if (part > pending_.size()) part = pending_.size();
                if (takes && FileSession::chunkFor(takes) < part) part = FileSession::chunkFor(takes);
                part = FileSession::chunkInRoom(part, FileSession::kRelayedPartThirds);
                std::string head = fieldsOf([&](JsonWriter& w) {
                    w.key("op");
                    w.string(upload_);
                    w.key("path");
                    w.string(remotePath_);
                    w.key("offset");
                    w.uinteger(offset);
                });
                // The whole envelope, built once at its exact size, the data
                // encoded in place from the bytes waiting; built again rather
                // than kept, should it go again (docs/DESIGN.md section 15).
                std::weak_ptr<RemoteFile> weak = shared_from_this();
                build = std::make_shared<DatagramEndpoint::Builder>(
                    [weak, head, part](uint32_t id, DatagramEndpoint::EnvelopeOut& out) {
                        auto self = weak.lock();
                        if (!self) return false;
                        MutexGuard lock(self->mutex_);
                        if (self->pending_.size() < part) return false;  // dropped meanwhile
                        char idHead[20];
                        int n = snprintf(idHead, sizeof(idHead), "{\"id\":%lu,", static_cast<unsigned long>(id));
                        static constexpr char kData[] = ",\"data\":\"";
                        size_t size =
                            static_cast<size_t>(n) + head.size() + sizeof(kData) - 1 + (part + 2) / 3 * 4 + 2;
                        // The envelope, in one block, and room for the link's
                        // copies of it in flight (its frames, then the
                        // radio's: about twice it, measured on DEVKIT-V3), not
                        // held here; without, the part goes again at half the
                        // size (smaller()).
                        Reservation room = memoryBudget().reserve(size + 32, 0, true, true, out.pool(size));
                        Reservation link = room ? memoryBudget().reserve(2 * size, 0, false) : Reservation();
                        self->noRoom_ = !room || !link || !out.reserve(size);
                        if (self->noRoom_) return false;
                        out.append(idHead, static_cast<size_t>(n));
                        out.append(head);
                        out.append(kData, sizeof(kData) - 1);
                        // Encoded 48 bytes at a time into the envelope.
                        static const char kAlphabet[] =
                            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        const uint8_t* d = self->pending_.data();
                        char buf[64];
                        size_t k = 0;
                        for (size_t i = 0; i < part; i += 3) {
                            uint32_t v = uint32_t(d[i]) << 16;
                            if (i + 1 < part) v |= uint32_t(d[i + 1]) << 8;
                            if (i + 2 < part) v |= d[i + 2];
                            buf[k++] = kAlphabet[v >> 18 & 63];
                            buf[k++] = kAlphabet[v >> 12 & 63];
                            buf[k++] = i + 1 < part ? kAlphabet[v >> 6 & 63] : '=';
                            buf[k++] = i + 2 < part ? kAlphabet[v & 63] : '=';
                            if (k == sizeof(buf)) {
                                out.append(buf, k);
                                k = 0;
                            }
                        }
                        out.append(buf, k);
                        out.append("\"}", 2);
                        return true;
                    });
            }
        }
        if (closed || idle) {
            done(closed ? Status::Busy : Status::Ok, JsonVariantConst());
            return;
        }
        auto self = shared_from_this();
        call(build, [self, done, offset, part](Status s, JsonVariantConst body) {
            if (self->smaller(s, body, part)) {
                self->pump(done);  // the same bytes, in smaller parts
                return;
            }
            if (s == Status::Busy) {
                // The peer (or this node) had no room or no queue: the
                // transfer stays open, and the client sends the rest again.
                MutexGuard guard(self->mutex_);
                self->pending_.clear();
            } else if (s != Status::Ok) {
                self->failed(s);
            }
            if (s != Status::Ok) {
                done(s, body);
                return;
            }
            bool more = false;
            bool stuck = false;
            {
                MutexGuard guard(self->mutex_);
                if (offset + part >= self->size_) {
                    // The part that completes the file: that was the upload's reply.
                    self->received_ = self->size_;
                    self->pending_.clear();
                    self->finishLocked();
                } else {
                    uint64_t got = body["received"].as<uint64_t>();
                    if (got <= offset || got > offset + part) {
                        stuck = true;
                    } else {
                        self->pending_.erase(self->pending_.begin(),
                                             self->pending_.begin() + static_cast<std::ptrdiff_t>(got - offset));
                        self->received_ = got;
                        more = !self->pending_.empty();
                    }
                }
            }
            if (stuck) {
                self->close();
                JsonDocument error;
                error["error"] = "internal";
                error["path"] = self->remotePath_;
                error["message"] = "the remote node didn't take the chunk";
                done(Status::Internal, error.as<JsonVariantConst>());
            } else if (more) {
                self->pump(done);
            } else {
                done(Status::Ok, body);
            }
        });
    }

    // An error ends the transfer, except a bad offset, which the client can
    // correct, and busy, which it can try again.
    void failed(Status s) {
        if (s != Status::BadRequest && s != Status::Busy) close();
    }

    // No room to relay what the client sent: dropped, for it to send again
    // from received(); the transfer stays open.
    // A part refused for memory (busy, low_memory: by the peer, or here)
    // goes again at half the size, down to kMinPart; the parts stay that
    // small for the rest of the transfer (docs/DESIGN.md section 14).
    bool smaller(Status s, JsonVariantConst body, size_t part) {
        if (s != Status::Busy || body["reason"] != "low_memory" || part <= kMinPart || !open()) return false;
        MutexGuard guard(mutex_);
        size_t half = part / 2 / 3 * 3;  // whole base64 groups
        limit_ = half > kMinPart ? half : kMinPart;
        return true;
    }

    // `drop`: the bytes waiting go too, for the client to send again.
    void lowMemory(const Done& done, bool drop = true) {
        if (drop) {
            MutexGuard guard(mutex_);
            pending_.clear();
        }
        JsonDocument error;
        error["error"] = "busy";
        error["reason"] = "low_memory";
        error["path"] = remoteBase_.empty() ? std::string("/") : remoteBase_;
        error["message"] = "not enough memory to relay the chunk";
        done(Status::Busy, error.as<JsonVariantConst>());
    }

    void finishLocked() {
        finished_ = true;
        closed_ = true;
        release();
    }

    void release() {
        if (claimed_) api_.releaseTransfer(key_);
        claimed_ = false;
    }

    Api& api_;
    RemoteNode& node_;
    const std::string remotePath_, localBase_, remoteBase_, localPath_;
    const std::string get_, upload_, download_;  // ops at the client's level
    std::string key_;
    DatagramEndpoint* endpoint_ = nullptr;  // the transfer's route (constructor)
    PeerAddress peer_;
    mutable Mutex mutex_;
    bool claimed_ = false;
    bool closed_ = false;
    bool finished_ = false;
    bool inFlight_ = false;
    bool expired_ = false;
    uint32_t activeMs_ = 0;
    size_t chunk_ = 0;
    size_t limit_ = 0;  // the largest part since one was refused for memory (smaller()), or 0
    uint64_t size_ = 0;
    uint64_t received_ = 0;
    std::vector<uint8_t> pending_;  // upload bytes from received_ on
    std::atomic<bool> noRoom_{false};  // the last envelope had no room (pump()'s builder)
};

}  // namespace

std::shared_ptr<FileRelay> RemoteNode::relayFile(Api& api, std::string_view rest, std::string_view base,
                                                 Access level) {
    if (!endpoint()) return nullptr;
    std::string remoteBase = remotePath_ == "/" ? std::string() : remotePath_;
    std::string localBase(base.substr(0, base.size() - rest.size()));
    return std::make_shared<RemoteFile>(api, *this, remotePathOf(rest), std::move(localBase), std::move(remoteBase),
                                        std::string(base), level);
}

}  // namespace tesser

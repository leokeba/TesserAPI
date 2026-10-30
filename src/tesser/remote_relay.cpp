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
        auto step = newStep(Stage::Probe);
        step->fields = fieldsOf(Stage::Probe);
        step->done = std::move(done);
        call(step);
    }

    void openUpload(uint64_t size, std::string_view filename, Done done) override {
        {
            MutexGuard guard(mutex_);
            size_ = size;
            received_ = 0;
            pending_.clear();
        }
        auto step = newStep(Stage::Open);
        step->fields = fieldsOf(Stage::Open, 0, size, filename);
        step->done = std::move(done);
        call(step);
    }

    void downloadName(std::string_view filename) override {
        MutexGuard guard(mutex_);
        downloadName_.assign(filename.data(), filename.size());
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
        if (!room) {
            dropPending();
            JsonDocument error;
            lowMemory(error);
            return done(Status::Busy, error.as<JsonVariantConst>());
        }
        pump(std::move(done));
    }

    void download(uint64_t offset, size_t length, Chunk done) override {
        // A part is at most this node's upload chunk, as a relayed upload's:
        // the remote's reply is held here whole, and in the remote's own
        // reply buffer (docs/DESIGN.md section 14).
        size_t cap = FileSession::uploadChunk(api_.config());
        std::string name;  // the opening chunk's
        {
            MutexGuard guard(mutex_);
            if (limit_ && cap > limit_) cap = limit_;
            if (!offset) name = downloadName_;
        }
        if (!length || length > cap) length = cap;
        // A reply this link takes.
        size_t own = endpoint_ ? endpoint_->maxMessage() : 0;
        if (own && FileSession::chunkFor(own) < length) length = FileSession::chunkFor(own);
        // And what this node's heap holds now, the reply and its way on.
        length = FileSession::chunkInRoom(length, FileSession::kRelayedDownloadThirds);
        auto step = newStep(Stage::Download);
        step->fields = fieldsOf(Stage::Download, offset, 0, name, length);
        step->chunk = std::move(done);
        step->offset = offset;
        step->bytes = length;
        call(step);
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
    // What the transfer is doing with the peer: one call at a time, each a
    // stage of a transfer.
    enum class Stage : uint8_t {
        Probe,     // is the path a file?
        Open,      // the upload's first envelope
        Part,      // a part of the upload
        Download,  // a part of the download
    };
    // One call: what it sends, and what its reply goes on to.
    struct Step {
        std::shared_ptr<RemoteFile> file;
        Stage stage = Stage::Probe;
        uint8_t attempt = 0;
        std::string fields;  // the envelope's members (a part's, before its data)
        Done done;           // not Download
        Chunk chunk;         // Download
        uint64_t offset = 0;  // Part, Download: where it starts
        size_t bytes = 0;     // and how much it asks for
    };
    // Every callback of a call: one type, so one std::function manager
    // however many signatures the endpoint holds it as.
    struct Callback {
        std::shared_ptr<Step> step;
        bool operator()(uint32_t id, DatagramEndpoint::EnvelopeOut& out) const {
            return step->file->build(*step, id, out);
        }
        void operator()(Status s, JsonVariantConst body) const { step->file->replied(step, s, body); }
        void operator()(std::string_view body) const { step->file->repliedRaw(step, body); }
    };

    std::shared_ptr<Step> newStep(Stage stage) {
        auto step = std::make_shared<Step>();
        step->file = shared_from_this();
        step->stage = stage;
        return step;
    }

    // An envelope's members, without the braces. Strict (docs/DESIGN.md
    // section 10.1.2): a node that doesn't know an option refuses the
    // envelope rather than ignore it. `size`: Open's. `filename`: Open's,
    // and an opening Download's. `length`: Download's.
    std::string fieldsOf(Stage stage, uint64_t offset = 0, uint64_t size = 0, std::string_view filename = {},
                         size_t length = 0) const {
        std::string text;
        StringSink sink(text);
        JsonWriter w(sink);
        w.beginObject();
        w.key("op");
        w.string(stage == Stage::Probe ? get_ : stage == Stage::Download ? download_ : upload_);
        w.key("path");
        w.string(remotePath_);
        if (stage == Stage::Probe) {
            w.key("view");
            w.string("schema");
            w.key("depth");
            w.integer(0);
        } else if (stage == Stage::Open) {
            w.key("size");
            w.uinteger(size);
        } else if (stage == Stage::Part || offset) {
            w.key("offset");
            w.uinteger(offset);
        }
        if (stage != Stage::Probe && stage != Stage::Part && !filename.empty()) {
            w.key("filename");
            w.string(filename);
        }
        if (length) {
            w.key("length");
            w.uinteger(length);
        }
        w.key("v");
        w.integer(kProtocolVersion);
        w.endObject();
        return text.substr(1, text.size() - 2);
    }

    // Sends a step's envelope by the transfer's route, again on a timeout
    // while the relay is open and the route online. Nothing is sent once the
    // route is offline or gone, even if the node has another: there, the
    // peer has no transfer from this gateway (`timeout`, which ends the
    // relay; the peer drops its side when that link loses this node, or
    // after Config::transferIdleMs).
    void call(const std::shared_ptr<Step>& step) {
        if (!routeUsable()) {
            JsonDocument error;
            error["error"] = "timeout";
            error["reason"] = "peer_offline";
            error["path"] = remoteBase_.empty() ? std::string("/") : remoteBase_;
            error["message"] = node_.online() ? "the route the transfer started on is offline" : "peer offline";
            advance(step, Status::Timeout, error.as<JsonVariantConst>(), dataOf(error.as<JsonVariantConst>()));
            return;
        }
        {
            MutexGuard guard(mutex_);
            inFlight_ = true;
            activeMs_ = millis32();
        }
        noRoom_ = false;
        Callback callback{step};
        bool sent = endpoint_ && endpoint_->requestBuilt(
                                     peer_, callback, callback, node_.timeoutMs(), &node_,
                                     step->stage == Stage::Download ? DatagramEndpoint::RawHandler(callback) : nullptr);
        if (sent) return;
        landed();
        if (noRoom_) {
            // The envelope had no room: its caller decides.
            JsonDocument error;
            lowMemory(error);
            advance(step, Status::Busy, error.as<JsonVariantConst>(), dataOf(error.as<JsonVariantConst>()));
            return;
        }
        advance(step, Status::Busy, JsonVariantConst(), std::string_view());
    }

    // The envelope of a step, for its call (and should it go again).
    bool build(const Step& step, uint32_t id, DatagramEndpoint::EnvelopeOut& out) {
        char head[20];
        int n = snprintf(head, sizeof(head), "{\"id\":%lu,", static_cast<unsigned long>(id));
        size_t idLen = static_cast<size_t>(n);
        if (step.stage != Stage::Part) {
            if (!out.reserve(idLen + step.fields.size() + 1)) return false;
            out.append(head, idLen);
            out.append(step.fields);
            out.append("}", 1);
            return true;
        }
        // The whole envelope, built once at its exact size, the data encoded
        // in place from the bytes waiting; built again rather than kept,
        // should it go again (docs/DESIGN.md section 15).
        MutexGuard lock(mutex_);
        if (pending_.size() < step.bytes) return false;  // dropped meanwhile
        static constexpr char kData[] = ",\"data\":\"";
        size_t size = idLen + step.fields.size() + sizeof(kData) - 1 + (step.bytes + 2) / 3 * 4 + 2;
        // The envelope, in one block, and room for the link's copies of it
        // in flight (its frames, then the radio's: about twice it, measured
        // on DEVKIT-V3), not held here; without, the part goes again at half
        // the size (smaller()).
        Reservation room = memoryBudget().reserve(size + 32, 0, true, true, out.pool(size));
        Reservation link = room ? memoryBudget().reserve(2 * size, 0, false) : Reservation();
        noRoom_ = !room || !link || !out.reserve(size);
        if (noRoom_) return false;
        out.append(head, idLen);
        out.append(step.fields);
        out.append(kData, sizeof(kData) - 1);
        // Encoded 48 bytes at a time into the envelope.
        detail::base64Encode(
            pending_.data(), step.bytes,
            [](void* o, const char* text, size_t len) {
                static_cast<DatagramEndpoint::EnvelopeOut*>(o)->append(text, len);
            },
            &out);
        out.append("\"}", 2);
        return true;
    }

    // Any reply but a raw one.
    void replied(const std::shared_ptr<Step>& step, Status s, JsonVariantConst body) {
        if (s == Status::Timeout && step->attempt + 1 < kAttempts && open() && routeUsable()) {
            step->attempt++;
            call(step);
            return;
        }
        landed();
        advance(step, s, body, dataOf(body));
    }

    // An "ok" reply to a Download: its body as the peer wrote it, the data
    // staying in the text.
    void repliedRaw(const std::shared_ptr<Step>& step, std::string_view text) {
        landed();
        JsonDocument filter;
        for (const char* k : {"size", "filename", "contentType", "chunk", "offset", "end"}) filter[k] = true;
        JsonDocument body;
        std::string_view data;
        bool bad = parseJson(body, text, ARDUINOJSON_DEFAULT_NESTING_LIMIT, filter) || !body.is<JsonObjectConst>();
        // A JSON string, without escapes (base64).
        if (!bad && jsonMember(text, "data", data)) {
            bad = data.size() < 2 || data.front() != '"' || data.back() != '"';
            if (!bad) data = data.substr(1, data.size() - 2);
        }
        if (bad) {
            JsonDocument error;
            error["error"] = "internal";
            error["message"] = "bad download reply";
            advance(step, Status::Internal, error.as<JsonVariantConst>(), std::string_view());
            return;
        }
        advance(step, Status::Ok, body.as<JsonVariantConst>(), data);
    }

    static std::string_view dataOf(JsonVariantConst body) {
        JsonString data = body["data"].as<JsonString>();
        return std::string_view(data.c_str() ? data.c_str() : "", data.size());
    }

    // A step is over: where the transfer goes from here. `data`: a Download's.
    void advance(const std::shared_ptr<Step>& step, Status s, JsonVariantConst body, std::string_view data) {
        switch (step->stage) {
            case Stage::Probe:
                step->done(s, body);
                return;
            case Stage::Open:
                if (s == Status::Ok) {
                    MutexGuard guard(mutex_);
                    if (size_ == 0) {
                        finishLocked();  // nothing to send: that was the upload's reply
                    } else {
                        chunk_ = body["chunk"].as<size_t>();
                        received_ = body["received"].as<uint64_t>();
                    }
                } else {
                    failed(s);
                }
                step->done(s, body);
                return;
            case Stage::Part:
                return partDone(*step, s, body);
            case Stage::Download:
                if (smaller(s, body, step->bytes)) {
                    download(step->offset, step->bytes, step->chunk);  // asked again, for less
                    return;
                }
                if (s == Status::Ok) {
                    MutexGuard guard(mutex_);
                    if (body["chunk"].is<size_t>()) chunk_ = body["chunk"].as<size_t>();
                    // The end: the peer is free again, though the last chunk
                    // can still be asked for again.
                    if (body["end"] == true) {
                        finished_ = true;
                        release();
                    }
                } else {
                    failed(s);
                }
                step->chunk(s, body, data);
                return;
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
        uint64_t offset = 0;
        size_t part = 0;
        bool closed, idle;
        std::shared_ptr<Step> step;
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
                step = newStep(Stage::Part);
                step->fields = fieldsOf(Stage::Part, offset);
                step->offset = offset;
                step->bytes = part;
            }
        }
        if (closed || idle) {
            done(closed ? Status::Busy : Status::Ok, JsonVariantConst());
            return;
        }
        step->done = std::move(done);
        call(step);
    }

    void partDone(const Step& step, Status s, JsonVariantConst body) {
        uint64_t offset = step.offset;
        size_t part = step.bytes;
        if (smaller(s, body, part)) {
            pump(step.done);  // the same bytes, in smaller parts
            return;
        }
        if (s == Status::Busy) {
            // The peer (or this node) had no room or no queue: the transfer
            // stays open, and the client sends the rest again.
            MutexGuard guard(mutex_);
            pending_.clear();
        } else if (s != Status::Ok) {
            failed(s);
        }
        if (s != Status::Ok) {
            step.done(s, body);
            return;
        }
        bool more = false;
        bool stuck = false;
        {
            MutexGuard guard(mutex_);
            if (offset + part >= size_) {
                // The part that completes the file: that was the upload's reply.
                received_ = size_;
                pending_.clear();
                finishLocked();
            } else {
                uint64_t got = body["received"].as<uint64_t>();
                if (got <= offset || got > offset + part) {
                    stuck = true;
                } else {
                    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(got - offset));
                    received_ = got;
                    more = !pending_.empty();
                }
            }
        }
        if (stuck) {
            close();
            JsonDocument error;
            error["error"] = "internal";
            error["path"] = remotePath_;
            error["message"] = "the remote node didn't take the chunk";
            step.done(Status::Internal, error.as<JsonVariantConst>());
        } else if (more) {
            pump(step.done);
        } else {
            step.done(Status::Ok, body);
        }
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

    void dropPending() {
        MutexGuard guard(mutex_);
        pending_.clear();
    }

    void lowMemory(JsonDocument& error) const {
        error["error"] = "busy";
        error["reason"] = "low_memory";
        error["path"] = remoteBase_.empty() ? std::string("/") : remoteBase_;
        error["message"] = "not enough memory to relay the chunk";
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
    std::string downloadName_;      // the client's, for the download's opening chunk
    std::atomic<bool> noRoom_{false};  // the last envelope had no room (build())
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

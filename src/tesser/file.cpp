// File nodes and transfers (docs/DESIGN.md section 4.6).
#include "tesser/file.h"

#include <stdio.h>
#include <string.h>

#include "tesser/api.h"
#include "tesser/budget.h"
#include "tesser/call.h"
#include "tesser/datagram.h"
#include "tesser/envelope.h"
#include "tesser/remote.h"
#include "tesser/sink.h"

namespace tesser {

namespace {

class TextSource : public FileSource {
public:
    explicit TextSource(std::string text) : text_(std::move(text)) {}
    int64_t size() const override { return static_cast<int64_t>(text_.size()); }
    int read(uint8_t* buf, size_t cap) override {
        size_t n = text_.size() - pos_;
        if (n > cap) n = cap;
        memcpy(buf, text_.data() + pos_, n);
        pos_ += n;
        return static_cast<int>(n);
    }

private:
    std::string text_;
    size_t pos_ = 0;
};

// What a relayed transfer's state takes (RemoteNode::relayFile()), checked
// before it is made.
constexpr size_t kRelayState = 512;
// And a reply detached to wait for the peer's (EnvelopeReply::detach()).
constexpr size_t kDetachedReply = 320;

class TextSink : public FileSink {
public:
    explicit TextSink(std::shared_ptr<const FileNode::TextSetter> set) : set_(std::move(set)) {}
    // The whole file is held: room for the size announced, taken at once
    // within the byte accountant (docs/DESIGN.md section 15). False if the
    // heap can't.
    bool reserve(uint64_t size) {
        if (size <= 256) return true;
        Reservation room = memoryBudget().reserve(static_cast<size_t>(size), 0, true, true,
                                                  mallocPool(static_cast<size_t>(size)));
        if (!room) return false;
        reserveExactly(text_, static_cast<size_t>(size));
        return true;
    }
    Check write(const uint8_t* data, size_t len) override {
        // More than announced (HTTP without a length): grown as it comes.
        if (!growFor(text_, text_.size() + len)) return Check::fail(Status::Busy, "not enough memory for the file");
        text_.append(reinterpret_cast<const char*>(data), len);
        return Check::ok();
    }
    Check finish(JsonWriter& reply) override { return (*set_)(text_, reply); }

private:
    // Shared with the node, which may be gone by now (a list element's).
    std::shared_ptr<const FileNode::TextSetter> set_;
    std::string text_;
};

}  // namespace

FileNode& FileNode::text(TextGetter get, TextSetter set) {
    if (!maxSize_) maxSize_ = 16384;
    if (get) {
        download([get = std::move(get)](FileRequest&) { return std::unique_ptr<FileSource>(new TextSource(get())); });
    }
    if (set) {
        auto shared = std::make_shared<const TextSetter>(std::move(set));
        upload([shared](FileRequest& r) -> std::unique_ptr<FileSink> {
            std::unique_ptr<TextSink> sink(new TextSink(shared));
            if (!sink->reserve(r.size)) {
                r.refuse(Status::Busy, "not enough memory for the file");
                return nullptr;
            }
            return sink;
        });
    }
    return *this;
}

FileTransfer::~FileTransfer() {
    MutexGuard guard(mutex_);
    close();
}

void FileTransfer::close() {
    sink_.reset();  // without finish(): aborted
    source_.reset();
    if (claimed_) api_.releaseTransfer(path_);
    claimed_ = false;
    if (relay_) relay_->close();
    relay_.reset();
    relayBuf_.clear();
    relayBuf_.shrink_to_fit();
    relayAt_ = 0;
    relayReply_.clear();
    relaySize_ = -1;
    relayPosition_ = 0;
    relayEnd_ = false;
}

void FileTransfer::cancel() {
    MutexGuard guard(mutex_);
    close();
}

bool FileTransfer::active() const {
    MutexGuard guard(mutex_);
    return sink_ || source_ || (relay_ && relay_->open());
}

bool FileTransfer::expireIfIdle(uint32_t nowMs, uint32_t idleMs) {
    // Busy (in a call, or being destroyed): not idle. Never waits, since
    // the caller holds the API lock, which a call takes after this mutex.
    if (!idleMs || !mutex_.tryLock()) return false;
    bool idle = claimed_ && nowMs - activeMs_ >= idleMs;
    if (idle) {
        close();
        expired_ = true;
    }
    mutex_.unlock();
    return idle;
}

FileTransfer::Open FileTransfer::open(std::string_view path, const Client& client, Op op, uint64_t size,
                                      std::string_view filename, Reply& reply) {
    MutexGuard transferGuard(mutex_);
    close();
    expired_ = false;
    Request req;
    req.op = op;
    req.path = path;
    req.client = client;
    path_.assign(path.data(), path.size());
    if (path_.size() > 1 && path_.back() == '/') path_.pop_back();
    Api::RemoteTarget remote;
    {
        MutexGuard guard(api_.mutex());
        bool notFile = false;
        // Destroyed before the guard: list elements are built and freed
        // under the lock. Their file nodes' handlers run while they exist.
        std::vector<std::unique_ptr<Node>> elements;
        FileNode* node = api_.fileNode(req, reply, notFile, elements, &remote);
        if (notFile) return Open::NotFile;
        if (node) return openLocal(*node, client, op, size, filename, reply);
        if (!remote.node) return Open::Failed;
        // The relay's state, only with room for it (docs/DESIGN.md section 15).
        Reservation room = memoryBudget().reserve(kRelayState);
        if (!room) {
            writeError(reply, Status::Busy, Reason::LowMemory, path_, "not enough memory for a transfer");
            return Open::Failed;
        }
        relay_ = remote.node->relayFile(api_, remote.rest, remote.base, remote.level);
    }
    // Relayed: the peer's replies come from another task, outside the lock.
    if (!relay_) {
        writeError(reply, Status::Internal, path_, "no endpoint");
        return Open::Failed;
    }
    return openRelay(op, size, filename, reply);
}

FileTransfer::Open FileTransfer::openLocal(FileNode& file, const Client& client, Op op, uint64_t size,
                                           std::string_view filename, Reply& reply) {
    FileNode* node = &file;
    bool upload = op == Op::Set;
    if (upload ? !node->writable() : !node->readable()) {
        writeError(reply, Status::NotAllowed, path_, upload ? "this file can't be uploaded" : "this file can't be downloaded");
        return Open::Failed;
    }
    if (upload && node->maxSize() && size > node->maxSize()) {
        writeError(reply, Status::TooLarge, path_, "file too large");
        return Open::Failed;
    }
    if (!api_.claimTransfer(path_, this)) {
        writeError(reply, Status::Busy, Reason::TransferBusy, path_, "another transfer is in progress");
        return Open::Failed;
    }
    claimed_ = true;
    FileRequest r(client);
    r.size = size;
    r.filename.assign(filename.data(), filename.size());
    if (upload) {
        sink_ = node->openUpload(r);
    } else {
        source_ = node->openDownload(r);
        if (r.filename.empty()) r.filename = node->name();
    }
    if (!sink_ && !source_) {
        close();
        writeError(reply, r.status != Status::Ok ? r.status : Status::Internal, path_,
                   r.message ? r.message : "can't open the file");
        return Open::Failed;
    }
    filename_ = std::move(r.filename);
    contentType_ = node->contentType();
    expected_ = size;
    received_ = 0;
    activeMs_ = millis32();
    return Open::Ok;
}

namespace {
void noTransfer(Reply& reply, const std::string& path, bool expired, const char* what) {
    if (expired) {
        writeError(reply, Status::Timeout, Reason::Idle, path, "the transfer was idle too long");
    } else {
        writeError(reply, Status::Internal, path, what);
    }
}
}  // namespace

namespace {

// One relay call waited for (FileTransfer::Remotes::Wait).
struct Waited {
    Signal signal;
    Status status = Status::Timeout;
    JsonDocument body;
    std::vector<uint8_t> data;  // a download's chunk, decoded
};

Status waitFor(FileRelay& relay, const std::function<void(FileRelay::Done)>& start, JsonDocument& body) {
    auto w = std::make_shared<Waited>();
    start([w](Status s, JsonVariantConst b) {
        w->status = s;
        w->body.set(b);
        w->signal.give();
    });
    // The relay always answers, after its attempts. This only guards
    // against calls dropped without an answer (their remote node destroyed).
    for (int round = 0; !w->signal.take(relay.patienceMs()); round++) {
        if (round > 0 || !relay.busy()) {
            if (w->signal.take(100)) break;
            body.clear();
            return Status::Timeout;
        }
    }
    body = std::move(w->body);
    return w->status;
}

// A download's chunk at `offset`, waited for: its body in `body`, its data
// decoded in `data`, within the byte accountant (busy, low_memory if not).
Status waitForChunk(FileRelay& relay, uint64_t offset, JsonDocument& body, std::vector<uint8_t>& data) {
    auto w = std::make_shared<Waited>();
    relay.download(offset, 0, [w](Status s, JsonVariantConst b, std::string_view text) {
        w->status = s;
        w->body.set(b);
        if (s == Status::Ok) {
            size_t n = text.size() / 4 * 3;
            Reservation room = memoryBudget().reserve(n, 0, true, true, mallocPool(n));
            if (!room) {
                w->status = Status::Busy;
                w->body.clear();
                w->body["error"] = "busy";
                w->body["reason"] = "low_memory";
                w->body["message"] = "not enough memory for the chunk";
            } else {
                w->data.reserve(n);
                room.settle();
                if (!detail::base64Decode(text, w->data)) {
                    w->status = Status::Internal;
                    w->body.clear();
                    w->body["error"] = "internal";
                    w->body["message"] = "bad download reply";
                }
            }
        }
        w->signal.give();
    });
    for (int round = 0; !w->signal.take(relay.patienceMs()); round++) {
        if (round > 0 || !relay.busy()) {
            if (w->signal.take(100)) break;
            body.clear();
            return Status::Timeout;
        }
    }
    body = std::move(w->body);
    data = std::move(w->data);
    return w->status;
}

}  // namespace

FileTransfer::Open FileTransfer::openRelay(Op op, uint64_t size, std::string_view filename, Reply& reply) {
    expected_ = size;
    received_ = 0;
    activeMs_ = millis32();
    if (op == Op::Get) relay_->downloadName(filename);
    if (remotes_ == Remotes::Return) {
        if (!relay_->claim(reply)) {
            close();
            return Open::Failed;
        }
        return Open::Remote;
    }
    // The client named no operation (HTTP): whether the peer's node is a
    // file decides, as for a local path. The peer's other answers about the
    // path are left to the request forwarded as usual; no answer (timeout,
    // busy, offline) is an error.
    // What the peer's schema said of the path, as long as it is unchanged,
    // spares the round trip.
    JsonDocument body;
    bool isFile = false;
    if (!relay_->knownFile(isFile)) {
        Status s = waitFor(*relay_, [this](FileRelay::Done d) { relay_->probe(std::move(d)); }, body);
        if (s == Status::Timeout || s == Status::Busy || s == Status::Internal) {
            relay_->writeReply(reply, s, body.as<JsonVariantConst>());
            close();
            return Open::Failed;
        }
        isFile = s == Status::Ok && body["type"] == "file";
        if (s == Status::Ok) relay_->learnedFile(isFile);
    }
    if (!isFile) {
        close();
        return Open::NotFile;
    }
    Status s;
    if (!relay_->claim(reply)) {
        close();
        return Open::Failed;
    }
    if (op == Op::Set) {
        s = waitFor(*relay_, [&](FileRelay::Done d) { relay_->openUpload(size, filename, std::move(d)); }, body);
        if (s == Status::Ok && relay_->finished()) serializeJson(body, relayReply_);  // size 0
        filename_.assign(filename.data(), filename.size());
    } else {
        std::vector<uint8_t> data;
        s = waitForChunk(*relay_, 0, body, data);
        if (s == Status::Ok) takeChunk(body.as<JsonVariantConst>(), std::move(data));
        if (s == Status::Ok) {
            relaySize_ = body["size"].is<int64_t>() ? body["size"].as<int64_t>() : -1;
            filename_ = body["filename"] | "";
            contentType_ = body["contentType"] | "application/octet-stream";
        }
    }
    if (s != Status::Ok) {
        relay_->writeReply(reply, s, body.as<JsonVariantConst>());
        close();
        return Open::Failed;
    }
    activeMs_ = millis32();
    return Open::Ok;
}

void FileTransfer::takeChunk(JsonVariantConst body, std::vector<uint8_t> data) {
    relayBuf_ = std::move(data);
    relayAt_ = 0;
    relayPosition_ = body["offset"].as<uint64_t>() + relayBuf_.size();
    relayEnd_ = body["end"] == true;
}

bool FileTransfer::fetchRelay() {
    JsonDocument body;
    std::vector<uint8_t> data;
    if (waitForChunk(*relay_, relayPosition_, body, data) != Status::Ok) return false;
    takeChunk(body.as<JsonVariantConst>(), std::move(data));
    return true;
}

bool FileTransfer::flushRelay(Reply& reply, bool all) {
    if (!relay_->open()) {
        noTransfer(reply, path_, relay_->expired(), "no upload in progress");
        close();
        return false;
    }
    size_t chunk = relay_->chunk();
    size_t n = all || !chunk ? relayBuf_.size() : relayBuf_.size() / chunk * chunk;
    if (!n) return true;
    std::vector<uint8_t> part(relayBuf_.begin(), relayBuf_.begin() + static_cast<std::ptrdiff_t>(n));
    relayBuf_.erase(relayBuf_.begin(), relayBuf_.begin() + static_cast<std::ptrdiff_t>(n));
    JsonDocument body;
    Status s = waitFor(*relay_, [&](FileRelay::Done d) { relay_->write(std::move(part), std::move(d)); }, body);
    if (s != Status::Ok) {
        relay_->writeReply(reply, s, body.as<JsonVariantConst>());
        close();
        return false;
    }
    if (relay_->finished()) serializeJson(body, relayReply_);
    activeMs_ = millis32();
    return true;
}

FileTransfer::Open FileTransfer::openUpload(std::string_view path, const Client& client, uint64_t size, Reply& reply,
                                            std::string_view filename) {
    return open(path, client, Op::Set, size, filename, reply);
}

FileTransfer::Open FileTransfer::openDownload(std::string_view path, const Client& client, Reply& reply,
                                              std::string_view filename) {
    return open(path, client, Op::Get, 0, filename, reply);
}

bool FileTransfer::write(const uint8_t* data, size_t len, Reply& reply) {
    MutexGuard guard(mutex_);
    if (!sink_ && !relay_) {
        noTransfer(reply, path_, expired_, "no upload in progress");
        return false;
    }
    if (expected_ && received_ + len > expected_) {
        close();
        writeError(reply, Status::TooLarge, path_, "more bytes than announced");
        return false;
    }
    if (relay_) {
        // Whole chunks of the peer's size, and the rest at the end; grown to
        // its exact need within the byte budget (docs/DESIGN.md section 15).
        size_t need = relayBuf_.size() + len;
        if (need > relayBuf_.capacity()) {
            Reservation room = memoryBudget().reserve(need, 0, true, true, mallocPool(need));
            if (!room) {
                close();
                writeError(reply, Status::Busy, Reason::LowMemory, path_, "not enough memory to relay the upload");
                return false;
            }
            relayBuf_.reserve(need);
        }
        relayBuf_.insert(relayBuf_.end(), data, data + len);
        received_ += len;
        activeMs_ = millis32();
        return flushRelay(reply, received_ == expected_);
    }
    Check c = sink_->write(data, len);
    if (!c.isOk()) {
        close();
        writeError(reply, c.status, path_, c.message ? c.message : "write failed");
        return false;
    }
    received_ += len;
    activeMs_ = millis32();
    return true;
}

void FileTransfer::finish(Reply& reply) {
    MutexGuard guard(mutex_);
    if (relay_) {
        if (received_ != expected_) {
            close();
            writeError(reply, Status::BadRequest, path_, "upload cut short");
            return;
        }
        if (relayReply_.empty() && !flushRelay(reply, true)) return;
        if (relayReply_.empty()) {
            close();
            writeError(reply, Status::Internal, path_, "the remote node didn't end the upload");
            return;
        }
        JsonWriter out(reply.begin(Status::Ok));
        out.raw(relayReply_);
        detail::finishBody(reply, out, path_);
        close();
        return;
    }
    if (!sink_) {
        noTransfer(reply, path_, expired_, "no upload in progress");
        return;
    }
    if (received_ != expected_) {
        close();
        writeError(reply, Status::BadRequest, path_, "upload cut short");
        return;
    }
    std::string body;
    StringSink bodySink(body);
    JsonWriter w(bodySink);
    Check c = sink_->finish(w);
    sink_.reset();
    close();
    if (!c.isOk()) {
        writeError(reply, c.status, path_, c.message ? c.message : "rejected");
        return;
    }
    JsonWriter out(reply.begin(Status::Ok));
    if (body.empty()) {
        out.null();
    } else {
        out.raw(body);
    }
    detail::finishBody(reply, out, path_);
}

int64_t FileTransfer::size() const {
    MutexGuard guard(mutex_);
    if (relay_) return relaySize_;
    return source_ ? source_->size() : -1;
}

int FileTransfer::read(uint8_t* buf, size_t cap) {
    MutexGuard guard(mutex_);
    if (relay_) {
        while (relayAt_ == relayBuf_.size()) {
            if (relayEnd_) {
                close();
                return 0;
            }
            if (!fetchRelay()) {
                close();
                return -1;
            }
            activeMs_ = millis32();
        }
        size_t n = relayBuf_.size() - relayAt_;
        if (n > cap) n = cap;
        memcpy(buf, relayBuf_.data() + relayAt_, n);
        relayAt_ += n;
        return static_cast<int>(n);
    }
    if (!source_) return -1;
    int n = source_->read(buf, cap);
    if (n <= 0) close();
    activeMs_ = millis32();
    return n;
}

// ---- FileSession: file transfers over message transports -----------------

namespace detail {

namespace {
const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
}  // namespace

void base64Encode(const uint8_t* data, size_t len, std::string& out) {
    out.reserve(out.size() + (len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16 | uint32_t(data[i + 1]) << 8 | data[i + 2];
        out += kBase64[v >> 18];
        out += kBase64[(v >> 12) & 63];
        out += kBase64[(v >> 6) & 63];
        out += kBase64[v & 63];
    }
    if (i < len) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i + 1]) << 8;
        out += kBase64[v >> 18];
        out += kBase64[(v >> 12) & 63];
        out += i + 1 < len ? kBase64[(v >> 6) & 63] : '=';
        out += '=';
    }
}

bool base64Decode(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    if (text.size() % 4) return false;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        bool last = i + 4 == text.size();
        int pad = 0;
        if (last && text[i + 3] == '=') pad = text[i + 2] == '=' ? 2 : 1;
        uint32_t v = 0;
        for (size_t j = 0; j < 4; j++) {
            int d = j < 4 - size_t(pad) ? base64Value(text[i + j]) : 0;
            if (d < 0) return false;
            v = v << 6 | uint32_t(d);
        }
        out.push_back(uint8_t(v >> 16));
        if (pad < 2) out.push_back(uint8_t(v >> 8));
        if (pad < 1) out.push_back(uint8_t(v));
    }
    return true;
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc) {
    // A nibble at a time: 64 bytes of table, fast enough for 4 KB chunks.
    static const uint32_t kTable[16] = {0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4,
                                        0x4db26158, 0x5005713c, 0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
                                        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c};
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        crc = (crc >> 4) ^ kTable[crc & 15];
        crc = (crc >> 4) ^ kTable[crc & 15];
    }
    return ~crc;
}

bool crc32OfBase64(std::string_view text, uint32_t& crc) {
    crc = 0;
    if (text.size() % 4) return false;
    for (size_t i = 0; i < text.size(); i += 4) {
        bool last = i + 4 == text.size();
        int pad = 0;
        if (last && text[i + 3] == '=') pad = text[i + 2] == '=' ? 2 : 1;
        uint32_t v = 0;
        for (size_t j = 0; j < 4; j++) {
            int d = j < 4 - size_t(pad) ? base64Value(text[i + j]) : 0;
            if (d < 0) return false;
            v = v << 6 | uint32_t(d);
        }
        uint8_t bytes[3] = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        crc = crc32(bytes, size_t(3 - pad), crc);
    }
    return true;
}

}  // namespace detail

namespace {

constexpr size_t kMaxChunk = 4096;  // bytes of file data per envelope, at most

// Data bytes whose base64 fits in `limit` bytes of envelope, leaving room
// for the rest of it (id, path, filename).
size_t chunkWithin(size_t limit) {
    size_t n = limit > 512 ? (limit - 256) / 4 * 3 : 192;
    return n < kMaxChunk ? n : kMaxChunk;
}

std::string_view text(JsonVariantConst v) {
    JsonString s = v.as<JsonString>();
    return std::string_view(s.c_str(), s.size());
}

}  // namespace

struct FileSession::State {
    explicit State(Api& api) : transfer(api) {}
    FileTransfer transfer;
    Access level = Access::Public;  // the client's when it opened
    bool upload = false;
    std::string path;  // normalized
    // Download.
    int64_t size = -1;  // the file's (download), or the upload's
    uint64_t position = 0;     // bytes sent: where the next chunk starts
    uint64_t lastOffset = 0;   // where the last chunk started
    std::vector<uint8_t> chunk;  // read from lastOffset on: the last chunk, kept to send it again, and any read ahead
    bool sent = false;           // a chunk is out
    bool end = false;            // the file has no more to read
};

FileSession::FileSession(Api& api) : api_(api) {}
FileSession::~FileSession() = default;

size_t FileSession::uploadChunk(const Config& config) { return chunkWithin(config.maxRequestBody); }
size_t FileSession::downloadChunk(const Config& config) { return chunkWithin(config.maxResponse); }
size_t FileSession::chunkFor(size_t limit) { return chunkWithin(limit); }

size_t FileSession::chunkInRoom(size_t max, size_t thirds) {
    // The envelope's other members, and what the replies' own copies round
    // up to.
    constexpr size_t kOverhead = 1024;
    size_t n = max;
    size_t room = heapRoom();
    if (room != SIZE_MAX) {
        room = room > kOverhead ? room - kOverhead : 0;
        if (room / thirds * 3 < n) n = room / thirds * 3;
    }
    // The reply's text is one block, with the headroom a reservation
    // leaves.
    size_t block = heapRoom(true);
    if (block != SIZE_MAX) {
        block = block > kOverhead ? block - kOverhead : 0;
        if (block / 4 * 3 < n) n = block / 4 * 3;
    }
    // Shrunk: in steps of 384 bytes (whole base64 groups), so the buffers
    // that come and go are of a few sizes, which a heap reuses.
    if (n < max) n = n / FileRelay::kMinPart * FileRelay::kMinPart;
    size_t least = max < FileRelay::kMinPart ? max : FileRelay::kMinPart;
    return n > least ? n : least;
}

bool FileSession::handles(JsonObjectConst envelope) {
    JsonVariantConst op = envelope["op"];
    std::string_view name;
    Access ceiling;
    return op.is<const char*>() && splitOp(text(op), name, ceiling) && (name == "upload" || name == "download");
}

void FileSession::reset() { state_.reset(); }

bool FileSession::done() const {
    if (!state_) return true;
    if (const std::shared_ptr<FileRelay>& relay = state_->transfer.relay()) return relay->finished();
    return !state_->upload && state_->end && state_->position == state_->lastOffset + state_->chunk.size();
}

bool FileSession::busy() const {
    if (!state_) return false;
    const std::shared_ptr<FileRelay>& relay = state_->transfer.relay();
    return !relay || relay->open();
}

void FileSession::handle(JsonObjectConst envelope, const Client& from, Reply& reply, std::string_view raw) {
    // "upload@user": a gateway's client (docs/DESIGN.md section 13.1).
    std::string_view op;
    Access ceiling = Access::Admin;
    splitOp(text(envelope["op"]), op, ceiling);
    Client client = from;
    client.cap(ceiling);
    // The client's level fell since the transfer opened (its token was
    // revoked, Api::tokensChanged()): the transfer ends, and this envelope
    // is taken as the next one's.
    if (state_ && client.level() < state_->level) state_.reset();
    const char* message = nullptr;
    Reason reason = Reason::None;
    Status strict = checkStrict(envelope, {"v", "id", "op", "path", "offset", "size", "filename", "data", "length", "crc32"},
                                message, &reason);
    // Parsed without its data (uploadFilter()), it may have lost members:
    // a strict envelope's keys are checked in its text.
    JsonVariantConst v = envelope["v"];
    if (strict == Status::Ok && !raw.empty() && v.is<uint8_t>() && v.as<uint8_t>() >= 2) {
        bool known = true;
        jsonKeys(raw, [&known](std::string_view k) {
            for (const char* ok : {"v", "id", "op", "path", "offset", "size", "filename", "data", "length", "crc32"}) {
                if (k == ok) return true;
            }
            known = false;
            return false;
        });
        if (!known) {
            strict = Status::BadRequest;
            message = "an option this node doesn't know (the request is strict: \"v\")";
            reason = Reason::UnknownOption;
        }
    }
    if (strict != Status::Ok) {
        writeError(reply, Status::BadRequest, reason, std::string_view(), message);
        return;
    }
    JsonVariantConst p = envelope["path"];
    std::string_view path = "/";
    if (p.is<const char*>()) {
        path = text(p);
    } else if (!p.isNull()) {
        writeError(reply, Status::BadRequest, std::string_view(), "path must be a string");
        return;
    }
    if (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    JsonVariantConst off = envelope["offset"];
    if (!off.isNull() && !off.is<uint64_t>()) {
        writeError(reply, Status::BadRequest, path, "offset must be a non-negative integer");
        return;
    }
    uint64_t offset = off.as<uint64_t>();
    if (op == "upload") {
        upload(envelope, raw, path, offset, client, reply);
    } else {
        download(envelope, path, offset, client, reply);
    }
}

void FileSession::upload(JsonObjectConst envelope, std::string_view raw, std::string_view path, uint64_t offset,
                         const Client& client, Reply& reply) {
    std::vector<uint8_t> data;
    JsonVariantConst d = envelope["data"];
    // Left out of the parsed envelope: read from its text, a JSON string
    // (base64 has no escapes).
    std::string_view rawData;
    bool fromRaw = d.isNull() && !raw.empty() && jsonMember(raw, "data", rawData);
    std::string_view b64;  // the data's base64
    bool bad = false;
    if (fromRaw) {
        bad = rawData.size() < 2 || rawData.front() != '"' || rawData.back() != '"';
        if (!bad) b64 = rawData.substr(1, rawData.size() - 2);
    } else if (!d.isNull()) {
        bad = !d.is<const char*>();
        if (!bad) b64 = text(d);
    }
    // The client's check of the bytes it meant (a serial line can lose some
    // and still be base64): of the whole chunk, from its text, compared
    // before anything is written, and refused for the client to send again
    // (docs/DESIGN.md section 10.1.1).
    JsonVariantConst crc = envelope["crc32"];
    uint32_t sum = 0;
    if (!bad && !crc.isNull()) bad = !detail::crc32OfBase64(b64, sum);
    if (bad || b64.size() % 4) {
        writeError(reply, Status::BadRequest, path, "data must be base64");
        return;
    }
    if (!crc.isNull()) {
        if (!crc.is<uint32_t>()) {
            writeError(reply, Status::BadRequest, path, "crc32 must be an unsigned 32-bit integer");
            return;
        }
        if (sum != crc.as<uint32_t>()) {
            writeError(reply, Status::BadRequest, Reason::Checksum, path,
                       "the data doesn't match its crc32: send the chunk again");
            return;
        }
    }
    // As much of it as the heap holds now, decoded in one block, only with
    // room for it (docs/DESIGN.md section 15): the reply's `received` says
    // where the client goes on from.
    size_t pad = b64.size() >= 4 && b64.back() == '=' ? (b64[b64.size() - 2] == '=' ? 2 : 1) : 0;
    size_t whole = b64.size() / 4 * 3 - pad;
    // Not the chunk that completes the upload: its reply is the upload's
    // (section 10.1.1), which a client waits for once it has sent the end.
    JsonVariantConst opening = envelope["size"];
    uint64_t total = opening.is<uint64_t>() ? opening.as<uint64_t>()
                     : state_ && state_->upload && state_->path == path && state_->size >= 0
                         ? static_cast<uint64_t>(state_->size)
                         : UINT64_MAX;
    if (whole > 256 && offset + whole < total) {
        bool local = state_ && state_->upload && state_->path == path && !state_->transfer.relay() &&
                     opening.isNull();
        size_t take = chunkInRoom(whole, local ? kLocalUploadThirds : kRelayedUploadThirds);
        if (take < whole) b64 = b64.substr(0, take / 3 * 4);
    }
    if (whole > 256) {
        Reservation room = memoryBudget().reserve(b64.size() / 4 * 3, 0, true);
        if (!room) {
            writeError(reply, Status::Busy, Reason::LowMemory, path, "not enough memory for the chunk");
            return;
        }
        data.reserve(b64.size() / 4 * 3);
    }
    if (!b64.empty() && !detail::base64Decode(b64, data)) {
        writeError(reply, Status::BadRequest, path, "data must be base64");
        return;
    }
    JsonVariantConst size = envelope["size"];
    if (!size.isNull()) {
        // A size starts a new upload, ending whatever was in progress.
        JsonVariantConst filename = envelope["filename"];
        if (!size.is<uint64_t>()) {
            writeError(reply, Status::BadRequest, path, "size must be a non-negative integer");
            return;
        }
        if (offset) {
            writeError(reply, Status::BadRequest, path, "a new upload starts at offset 0");
            return;
        }
        if (!filename.isNull() && !filename.is<const char*>()) {
            writeError(reply, Status::BadRequest, path, "filename must be a string");
            return;
        }
        state_.reset();
        // The transfer's state, only with room for it.
        Reservation room = memoryBudget().reserve(sizeof(State));
        if (!room) {
            writeError(reply, Status::Busy, Reason::LowMemory, path, "not enough memory for a transfer");
            return;
        }
        auto state = std::unique_ptr<State>(new State(api_));
        std::string_view name = filename.isNull() ? std::string_view() : text(filename);
        FileTransfer::Open o = state->transfer.openUpload(path, client, size.as<uint64_t>(), reply, name);
        if (o == FileTransfer::Open::NotFile) writeError(reply, Status::NotAllowed, path, "not a file");
        if (o != FileTransfer::Open::Ok && o != FileTransfer::Open::Remote) return;
        state->upload = true;
        state->level = client.level();
        state->size = static_cast<int64_t>(size.as<uint64_t>());
        state->path.assign(path.data(), path.size());
        state_ = std::move(state);
        if (o == FileTransfer::Open::Remote) return relayUpload(true, size.as<uint64_t>(), name, 0, std::move(data), reply);
    } else if (!state_ || !state_->upload || state_->path != path || !state_->transfer.active()) {
        bool expired = state_ && state_->upload && state_->path == path && state_->transfer.expired();
        if (expired) {
            writeError(reply, Status::Timeout, Reason::Idle, path, "the transfer was idle too long");
        } else {
            writeError(reply, Status::BadRequest, path, "no upload in progress at this path");
        }
        return;
    } else if (state_->transfer.relay()) {
        return relayUpload(false, 0, std::string_view(), offset, std::move(data), reply);
    }
    FileTransfer& t = state_->transfer;
    uint64_t received = t.received();
    if (offset > received) {
        char message[64];
        snprintf(message, sizeof(message), "offset must be at most %llu", static_cast<unsigned long long>(received));
        writeError(reply, Status::BadRequest, path, message);
        return;
    }
    // Bytes before `received` arrived already (a chunk sent again after a
    // lost reply): only the rest is new.
    uint64_t skip = received - offset;
    if (skip < data.size() && !t.write(data.data() + skip, data.size() - static_cast<size_t>(skip), reply)) {
        state_.reset();
        return;
    }
    if (t.received() == t.expected()) {
        t.finish(reply);
        state_.reset();
        return;
    }
    JsonWriter w(reply.begin(Status::Ok));
    w.beginObject();
    w.key("received");
    w.uinteger(t.received());
    if (!size.isNull()) {
        w.key("chunk");
        w.uinteger(uploadChunk(api_.config()));
    }
    w.endObject();
    detail::finishBody(reply, w, path);
}

void FileSession::download(JsonObjectConst envelope, std::string_view path, uint64_t offset, const Client& client,
                           Reply& reply) {
    // Within what the client takes, too (a peer's advertised limit,
    // docs/DESIGN.md section 14.1).
    size_t max = downloadChunk(api_.config());
    if (reply.limit() && chunkWithin(reply.limit()) < max) max = chunkWithin(reply.limit());
    size_t length = max;
    JsonVariantConst len = envelope["length"];
    if (!len.isNull()) {
        if (!len.is<uint32_t>() || len.as<uint32_t>() == 0) {
            writeError(reply, Status::BadRequest, path, "length must be a positive integer");
            return;
        }
        if (len.as<uint32_t>() < max) length = len.as<uint32_t>();
    }
    // The reply carries the chunk's CRC-32 when asked (docs/DESIGN.md
    // section 10.1.1).
    JsonVariantConst wantCrc = envelope["crc32"];
    if (!wantCrc.isNull() && !wantCrc.is<bool>()) {
        writeError(reply, Status::BadRequest, path, "crc32 must be a boolean");
        return;
    }
    bool crc = wantCrc.as<bool>();
    bool opening = offset == 0;
    if (opening) {
        JsonVariantConst filename = envelope["filename"];
        if (!filename.isNull() && !filename.is<const char*>()) {
            writeError(reply, Status::BadRequest, path, "filename must be a string");
            return;
        }
        // Offset 0 starts a new download, ending whatever was in progress.
        state_.reset();
        // The transfer's state, only with room for it.
        Reservation room = memoryBudget().reserve(sizeof(State));
        if (!room) {
            writeError(reply, Status::Busy, Reason::LowMemory, path, "not enough memory for a transfer");
            return;
        }
        auto state = std::unique_ptr<State>(new State(api_));
        std::string_view name = filename.isNull() ? std::string_view() : text(filename);
        FileTransfer::Open o = state->transfer.openDownload(path, client, reply, name);
        if (o == FileTransfer::Open::NotFile) writeError(reply, Status::NotAllowed, path, "not a file");
        if (o != FileTransfer::Open::Ok && o != FileTransfer::Open::Remote) return;
        state->path.assign(path.data(), path.size());
        state->level = client.level();
        state->size = state->transfer.size();
        state_ = std::move(state);
        if (o == FileTransfer::Open::Remote) return relayDownload(true, 0, length, max, crc, reply);
    } else if (state_ && !state_->upload && state_->path == path && state_->transfer.relay()) {
        return relayDownload(false, offset, length, max, crc, reply);
    } else if (!state_ || state_->upload || state_->path != path) {
        writeError(reply, Status::BadRequest, path, "no download in progress at this path");
        return;
    }
    State& s = *state_;
    // s.chunk holds what was read from lastOffset on: the bytes up to
    // position went out with the last chunk; the rest (that chunk asked
    // again for less: a gateway short of memory, docs/DESIGN.md section 14)
    // go first with the next.
    bool again = !opening && s.sent && offset == s.lastOffset;
    if (!again && offset != s.position) {
        char message[64];
        snprintf(message, sizeof(message), "offset must be %llu", static_cast<unsigned long long>(s.position));
        writeError(reply, Status::BadRequest, path, message);
        return;
    }
    if (!again) {
        // As much as the heap holds now, on its way out (docs/DESIGN.md
        // section 15): the client goes on from where the reply ends.
        length = chunkInRoom(length, kLocalDownloadThirds);
        size_t sent = static_cast<size_t>(s.position - s.lastOffset);
        // Room for the chunk before anything changes: refused, it is asked
        // for again.
        if (!s.end && s.chunk.capacity() < length) {
            Reservation room = memoryBudget().reserve(length, 0, true);
            if (!room) {
                writeError(reply, Status::Busy, Reason::LowMemory, path, "not enough memory for the chunk");
                return;
            }
            s.chunk.reserve(length);
        }
        s.chunk.erase(s.chunk.begin(), s.chunk.begin() + static_cast<std::ptrdiff_t>(sent));
        s.lastOffset = s.position;
        // Then the file, until the chunk is full or the file ends.
        size_t got = s.chunk.size();
        if (!s.end && got < length) {
            s.chunk.resize(length);
            while (got < length) {
                int n = s.transfer.read(s.chunk.data() + got, length - got);
                if (n < 0) {
                    bool expired = s.transfer.expired();
                    state_.reset();
                    writeError(reply, expired ? Status::Timeout : Status::Internal, expired ? Reason::Idle : Reason::None,
                               path, expired ? "the transfer was idle too long" : "read failed");
                    return;
                }
                if (n == 0) {
                    s.end = true;
                    break;
                }
                got += static_cast<size_t>(n);
            }
            s.chunk.resize(got);
            if (s.size >= 0 && s.lastOffset + got >= static_cast<uint64_t>(s.size)) s.end = true;
            if (s.end) s.transfer.cancel();  // releases the path; the chunk stays
        }
        s.sent = true;
    }
    size_t n = s.chunk.size() < length ? s.chunk.size() : length;
    s.position = s.lastOffset + n;
    bool last = s.end && n == s.chunk.size();
    // The data is encoded straight into the reply: no copy of the chunk is
    // held as text (docs/DESIGN.md section 15).
    JsonWriter w(reply.begin(Status::Ok));
    w.beginObject();
    if (opening) {
        if (s.size >= 0) {
            w.key("size");
            w.integer(s.size);
        }
        w.key("filename");
        w.string(s.transfer.filename());
        w.key("contentType");
        w.string(s.transfer.contentType());
        w.key("chunk");
        w.uinteger(max);
    }
    w.key("offset");
    w.uinteger(s.lastOffset);
    w.key("data");
    w.base64(s.chunk.data(), n);
    if (crc) {
        w.key("crc32");
        w.uinteger(detail::crc32(s.chunk.data(), n));
    }
    w.key("end");
    w.boolean(last);
    w.endObject();
    detail::finishBody(reply, w, path);
}

// ---- relayed transfers ------------------------------------------------------

namespace {

// Defers `reply` for a relay call: false, with the error written, if it can't.
Reply* deferFor(Api& api, Reply& reply, const std::string& path) {
    // The detached reply (an envelope's id, its buffer), only with room.
    Reservation room = memoryBudget().reserve(kDetachedReply);
    if (!room) {
        writeError(reply, Status::Busy, Reason::LowMemory, path, "not enough memory to wait for the remote node");
        return nullptr;
    }
    if (!api.pendingBegin()) {
        writeError(reply, Status::Busy, Reason::PendingLimit, path, "too many pending calls");
        return nullptr;
    }
    Reply* detached = reply.detach();
    if (!detached) {
        api.pendingDone();
        writeError(reply, Status::NotAllowed, Reason::CannotWait, path, "this transport can't wait for remote nodes");
    }
    return detached;
}

// Whether `relay` can take the next envelope; otherwise the error is written.
bool relayReady(const FileRelay& relay, Reply& reply, const std::string& path, bool upload) {
    if (relay.busy()) {
        writeError(reply, Status::Busy, Reason::TransferBusy, path, "the last chunk is still on its way");
        return false;
    }
    if (relay.open()) return true;
    if (relay.expired()) {
        writeError(reply, Status::Timeout, Reason::Idle, path, "the transfer was idle too long");
    } else {
        writeError(reply, Status::BadRequest, path,
                   upload ? "no upload in progress at this path" : "no download in progress at this path");
    }
    return false;
}

}  // namespace

void FileSession::relayUpload(bool opening, uint64_t size, std::string_view filename, uint64_t offset,
                              std::vector<uint8_t> data, Reply& reply) {
    std::shared_ptr<FileRelay> relay = state_->transfer.relay();
    const std::string& path = state_->path;
    if (!relayReady(*relay, reply, path, true)) return;
    if (!opening) {
        uint64_t received = relay->received();
        if (offset > received) {
            char message[64];
            snprintf(message, sizeof(message), "offset must be at most %llu", static_cast<unsigned long long>(received));
            writeError(reply, Status::BadRequest, path, message);
            return;
        }
        // Bytes the peer has already (a chunk sent again): only the rest.
        uint64_t skip = received - offset;
        data.erase(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(skip < data.size() ? skip : data.size()));
    }
    Reply* detached = deferFor(api_, reply, path);
    if (!detached) return;
    Api* api = &api_;
    size_t chunk = uploadChunk(api_.config());
    // The client's reply: the peer's progress, or the upload's reply.
    auto answer = [relay, detached, api, opening, chunk, path](Status s, JsonVariantConst body) {
        if (s != Status::Ok || relay->finished()) {
            relay->writeReply(*detached, s, body);
        } else {
            JsonWriter w(detached->begin(Status::Ok));
            w.beginObject();
            w.key("received");
            w.uinteger(relay->received());
            if (opening) {
                // Chunks the gateway takes and the peer too: one call each.
                size_t c = relay->chunk();
                w.key("chunk");
                w.uinteger(c && c < chunk ? c : chunk);
            }
            w.endObject();
            detail::finishBody(*detached, w, path);
        }
        detached->release();
        api->pendingDone();
    };
    if (opening) {
        // The chunk waits for the opening's reply moved, not copied: a
        // callback (std::function) copies what it captures.
        auto held = std::make_shared<std::vector<uint8_t>>(std::move(data));
        relay->openUpload(size, filename, [relay, held, answer](Status s, JsonVariantConst body) {
            if (s != Status::Ok || relay->finished() || held->empty()) return answer(s, body);
            relay->write(std::move(*held), answer);
        });
    } else if (data.empty()) {
        answer(Status::Ok, JsonVariantConst());
    } else {
        relay->write(std::move(data), answer);
    }
}

namespace {

// One relayed download chunk, written to `detached` when the peer's reply
// comes. A reply this node has no memory to hold is asked for again, half as
// long: the peer sends the start of the same chunk (docs/DESIGN.md section
// 14).
void relayChunk(std::shared_ptr<FileRelay> relay, Reply* detached, Api* api, size_t max, bool crc, std::string path,
                uint64_t offset, size_t length) {
    relay->download(offset, length, [=](Status s, JsonVariantConst body, std::string_view data) {
        if (s != Status::Ok || !body.is<JsonObjectConst>()) {
            relay->writeReply(*detached, s, body);
        } else {
            // The peer's chunk as it came, base64 and all, straight from its
            // reply's text; only its largest chunk is capped at ours.
            JsonWriter w(detached->begin(Status::Ok));
            w.beginObject();
            // `data`, where the peer had it (after "offset"), and its crc32
            // if asked: of the bytes as they came from the peer, decoded a
            // group at a time, so the hop to the client is the one checked.
            auto writeData = [&] {
                w.key("data");
                w.string(data);
                uint32_t sum = 0;
                if (crc && detail::crc32OfBase64(data, sum)) {
                    w.key("crc32");
                    w.uinteger(sum);
                }
            };
            bool wrote = false;
            for (JsonPairConst kv : body.as<JsonObjectConst>()) {
                if (kv.key() == "data") continue;
                w.key(std::string_view(kv.key().c_str(), kv.key().size()));
                if (kv.key() == "chunk") {
                    size_t c = kv.value().as<size_t>();
                    w.uinteger(c && c < max ? c : max);
                } else {
                    w.variant(kv.value());
                }
                if (kv.key() == "offset") {
                    writeData();
                    wrote = true;
                }
            }
            if (!wrote) writeData();
            w.endObject();
            size_t got = data.size() / 4 * 3;
            if (!w.ok() && detached->outOfMemory() && got > FileRelay::kMinPart && detached->rollback()) {
                size_t half = got / 2 / 3 * 3;
                relayChunk(relay, detached, api, max, crc, path, offset,
                           half > FileRelay::kMinPart ? half : FileRelay::kMinPart);
                return;
            }
            detail::finishBody(*detached, w, path);
        }
        detached->release();
        api->pendingDone();
    });
}

}  // namespace

void FileSession::relayDownload(bool opening, uint64_t offset, size_t length, size_t max, bool crc, Reply& reply) {
    // The relay's parts are at most our upload chunk (RemoteFile::download()).
    if (max > uploadChunk(api_.config())) max = uploadChunk(api_.config());
    std::shared_ptr<FileRelay> relay = state_->transfer.relay();
    const std::string& path = state_->path;
    if (!opening && !relayReady(*relay, reply, path, false)) return;
    Reply* detached = deferFor(api_, reply, path);
    if (!detached) return;
    relayChunk(relay, detached, &api_, max, crc, path, offset, length);
}

}  // namespace tesser

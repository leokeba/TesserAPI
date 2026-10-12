// File nodes and transfers (docs/DESIGN.md section 4.6).
#include "tesser/file.h"

#include <stdio.h>
#include <string.h>

#include "tesser/api.h"
#include "tesser/call.h"

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

class TextSink : public FileSink {
public:
    explicit TextSink(std::shared_ptr<const FileNode::TextSetter> set) : set_(std::move(set)) {}
    Check write(const uint8_t* data, size_t len) override {
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
        upload([shared](FileRequest&) { return std::unique_ptr<FileSink>(new TextSink(shared)); });
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
}

void FileTransfer::cancel() {
    MutexGuard guard(mutex_);
    close();
}

bool FileTransfer::active() const {
    MutexGuard guard(mutex_);
    return sink_ || source_;
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
    MutexGuard guard(api_.mutex());
    bool notFile = false;
    // Destroyed before the guard: list elements are built and freed under
    // the lock. Their file nodes' handlers run while they exist.
    std::vector<std::unique_ptr<Node>> elements;
    FileNode* node = api_.fileNode(req, reply, notFile, elements);
    if (notFile) return Open::NotFile;
    if (!node) return Open::Failed;
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
        writeError(reply, Status::Busy, path_, "another transfer is in progress");
        return Open::Failed;
    }
    claimed_ = true;
    FileRequest r(client);
    r.size = size;
    if (upload) {
        r.filename.assign(filename.data(), filename.size());
        sink_ = node->openUpload(r);
    } else {
        r.filename = node->name();
        source_ = node->openDownload(r);
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

FileTransfer::Open FileTransfer::openUpload(std::string_view path, const Client& client, uint64_t size, Reply& reply,
                                            std::string_view filename) {
    return open(path, client, Op::Set, size, filename, reply);
}

FileTransfer::Open FileTransfer::openDownload(std::string_view path, const Client& client, Reply& reply) {
    return open(path, client, Op::Get, 0, std::string_view(), reply);
}

namespace {
void noTransfer(Reply& reply, const std::string& path, bool expired, const char* what) {
    if (expired) {
        writeError(reply, Status::Timeout, path, "the transfer was idle too long");
    } else {
        writeError(reply, Status::Internal, path, what);
    }
}
}  // namespace

bool FileTransfer::write(const uint8_t* data, size_t len, Reply& reply) {
    MutexGuard guard(mutex_);
    if (!sink_) {
        noTransfer(reply, path_, expired_, "no upload in progress");
        return false;
    }
    if (expected_ && received_ + len > expected_) {
        close();
        writeError(reply, Status::TooLarge, path_, "more bytes than announced");
        return false;
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
    return source_ ? source_->size() : -1;
}

int FileTransfer::read(uint8_t* buf, size_t cap) {
    MutexGuard guard(mutex_);
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
    bool upload = false;
    std::string path;  // normalized
    // Download.
    int64_t size = -1;
    uint64_t position = 0;     // bytes sent: where the next chunk starts
    uint64_t lastOffset = 0;   // where the last chunk started
    std::vector<uint8_t> chunk;  // the last chunk, kept to send it again
    bool sent = false;           // a chunk is out
    bool end = false;            // it was the last
};

FileSession::FileSession(Api& api) : api_(api) {}
FileSession::~FileSession() = default;

size_t FileSession::uploadChunk(const Config& config) { return chunkWithin(config.maxRequestBody); }
size_t FileSession::downloadChunk(const Config& config) { return chunkWithin(config.maxResponse); }

bool FileSession::handles(JsonObjectConst envelope) {
    JsonVariantConst op = envelope["op"];
    return op == "upload" || op == "download";
}

void FileSession::reset() { state_.reset(); }

void FileSession::handle(JsonObjectConst envelope, const Client& client, Reply& reply) {
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
    if (envelope["op"] == "upload") {
        upload(envelope, path, offset, client, reply);
    } else {
        download(envelope, path, offset, client, reply);
    }
}

void FileSession::upload(JsonObjectConst envelope, std::string_view path, uint64_t offset, const Client& client,
                         Reply& reply) {
    std::vector<uint8_t> data;
    JsonVariantConst d = envelope["data"];
    if (!d.isNull() && (!d.is<const char*>() || !detail::base64Decode(text(d), data))) {
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
        auto state = std::unique_ptr<State>(new State(api_));
        FileTransfer::Open o =
            state->transfer.openUpload(path, client, size.as<uint64_t>(), reply,
                                       filename.isNull() ? std::string_view() : text(filename));
        if (o == FileTransfer::Open::NotFile) writeError(reply, Status::NotAllowed, path, "not a file");
        if (o != FileTransfer::Open::Ok) return;
        state->upload = true;
        state->path.assign(path.data(), path.size());
        state_ = std::move(state);
    } else if (!state_ || !state_->upload || state_->path != path || !state_->transfer.active()) {
        bool expired = state_ && state_->upload && state_->path == path && state_->transfer.expired();
        if (expired) {
            writeError(reply, Status::Timeout, path, "the transfer was idle too long");
        } else {
            writeError(reply, Status::BadRequest, path, "no upload in progress at this path");
        }
        return;
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
    size_t max = downloadChunk(api_.config());
    size_t length = max;
    JsonVariantConst len = envelope["length"];
    if (!len.isNull()) {
        if (!len.is<uint32_t>() || len.as<uint32_t>() == 0) {
            writeError(reply, Status::BadRequest, path, "length must be a positive integer");
            return;
        }
        if (len.as<uint32_t>() < max) length = len.as<uint32_t>();
    }
    bool opening = offset == 0;
    if (opening) {
        // Offset 0 starts a new download, ending whatever was in progress.
        state_.reset();
        auto state = std::unique_ptr<State>(new State(api_));
        FileTransfer::Open o = state->transfer.openDownload(path, client, reply);
        if (o == FileTransfer::Open::NotFile) writeError(reply, Status::NotAllowed, path, "not a file");
        if (o != FileTransfer::Open::Ok) return;
        state->path.assign(path.data(), path.size());
        state->size = state->transfer.size();
        state_ = std::move(state);
    } else if (!state_ || state_->upload || state_->path != path) {
        writeError(reply, Status::BadRequest, path, "no download in progress at this path");
        return;
    }
    State& s = *state_;
    if (!opening && s.sent && offset == s.lastOffset) {
        // The last chunk again: its reply was lost.
    } else if (offset != s.position) {
        char message[64];
        snprintf(message, sizeof(message), "offset must be %llu", static_cast<unsigned long long>(s.position));
        writeError(reply, Status::BadRequest, path, message);
        return;
    } else if (s.end) {
        s.chunk.clear();  // past the end: nothing more
        s.lastOffset = s.position;
    } else {
        // The next chunk: read until it is full or the file ends.
        s.chunk.resize(length);
        size_t got = 0;
        while (got < length) {
            int n = s.transfer.read(s.chunk.data() + got, length - got);
            if (n < 0) {
                bool expired = s.transfer.expired();
                state_.reset();
                writeError(reply, expired ? Status::Timeout : Status::Internal, path,
                           expired ? "the transfer was idle too long" : "read failed");
                return;
            }
            if (n == 0) {
                s.end = true;
                break;
            }
            got += static_cast<size_t>(n);
        }
        s.chunk.resize(got);
        s.lastOffset = s.position;
        s.position += got;
        if (s.size >= 0 && s.position >= static_cast<uint64_t>(s.size)) s.end = true;
        if (s.end) s.transfer.cancel();  // releases the path; the chunk stays
        s.sent = true;
    }
    std::string data = "\"";
    detail::base64Encode(s.chunk.data(), s.chunk.size(), data);
    data += '"';
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
    w.raw(data);
    w.key("end");
    w.boolean(s.end);
    w.endObject();
    detail::finishBody(reply, w, path);
}

}  // namespace tesser

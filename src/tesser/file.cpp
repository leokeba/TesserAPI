// File nodes and transfers (docs/DESIGN.md section 4.6).
#include "tesser/file.h"

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
    explicit TextSink(const FileNode::TextSetter& set) : set_(set) {}
    Check write(const uint8_t* data, size_t len) override {
        text_.append(reinterpret_cast<const char*>(data), len);
        return Check::ok();
    }
    Check finish(JsonWriter& reply) override { return set_(text_, reply); }

private:
    const FileNode::TextSetter& set_;  // the node's: nodes outlive transfers
    std::string text_;
};

}  // namespace

FileNode& FileNode::text(TextGetter get, TextSetter set) {
    if (!maxSize_) maxSize_ = 16384;
    if (get) {
        download([get = std::move(get)](FileRequest&) { return std::unique_ptr<FileSource>(new TextSource(get())); });
    }
    if (set) {
        auto shared = std::make_shared<TextSetter>(std::move(set));
        upload([shared](FileRequest&) { return std::unique_ptr<FileSink>(new TextSink(*shared)); });
    }
    return *this;
}

FileTransfer::~FileTransfer() { close(); }

void FileTransfer::close() {
    sink_.reset();  // without finish(): aborted
    source_.reset();
    if (node_) node_->release();
    node_ = nullptr;
}

FileTransfer::Open FileTransfer::open(std::string_view path, const Client& client, Op op, uint64_t size,
                                      Reply& reply) {
    close();
    Request req;
    req.op = op;
    req.path = path;
    req.client = client;
    path_.assign(path.data(), path.size());
    if (path_.size() > 1 && path_.back() == '/') path_.pop_back();
    MutexGuard guard(api_.mutex());
    bool notFile = false;
    FileNode* node = api_.fileNode(req, reply, notFile);
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
    if (!node->claim()) {
        writeError(reply, Status::Busy, path_, "another transfer is in progress");
        return Open::Failed;
    }
    node_ = node;
    FileRequest r(client);
    r.size = size;
    r.filename = node->name();
    if (upload) {
        sink_ = node->openUpload(r);
    } else {
        source_ = node->openDownload(r);
    }
    if (!sink_ && !source_) {
        close();
        writeError(reply, r.status != Status::Ok ? r.status : Status::Internal, path_,
                   r.message ? r.message : "can't open the file");
        return Open::Failed;
    }
    filename_ = std::move(r.filename);
    expected_ = size;
    received_ = 0;
    return Open::Ok;
}

FileTransfer::Open FileTransfer::openUpload(std::string_view path, const Client& client, uint64_t size, Reply& reply) {
    return open(path, client, Op::Set, size, reply);
}

FileTransfer::Open FileTransfer::openDownload(std::string_view path, const Client& client, Reply& reply) {
    return open(path, client, Op::Get, 0, reply);
}

bool FileTransfer::write(const uint8_t* data, size_t len, Reply& reply) {
    if (!sink_) {
        writeError(reply, Status::Internal, path_, "no upload in progress");
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
    return true;
}

void FileTransfer::finish(Reply& reply) {
    if (!sink_) {
        writeError(reply, Status::Internal, path_, "no upload in progress");
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

int64_t FileTransfer::size() const { return source_ ? source_->size() : -1; }

int FileTransfer::read(uint8_t* buf, size_t cap) {
    if (!source_) return -1;
    int n = source_->read(buf, cap);
    if (n <= 0) close();
    return n;
}

}  // namespace tesser

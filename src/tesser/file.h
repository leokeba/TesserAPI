#pragma once

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "tesser/file_session.h"
#include "tesser/node.h"
#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class Api;

// Holds a path in the API's transfers: one transfer per path
// (Api::claimTransfer()).
class TransferHolder {
public:
    // Under the API lock: ends the transfer if it has been idle for `idleMs`
    // (0: never) and isn't busy. True if it did, releasing its claim.
    virtual bool expireIfIdle(uint32_t nowMs, uint32_t idleMs) = 0;

protected:
    ~TransferHolder() = default;
};

// A file transfer relayed to a file node below a remote node (docs/DESIGN.md
// section 14): upload and download envelopes (section 10.1.1) sent to the
// peer, one at a time, in its chunks. Opened by RemoteNode::relayFile(),
// which claims the peer: the peer keeps one transfer per client, and the
// gateway is one client. Asynchronous: `done` runs later, in the endpoint's
// process(), with the peer's reply (Status::Timeout or Busy without a body if
// it didn't come). A chunk whose reply timed out is sent again, twice at
// most. An error other than bad_request ends the transfer, and so does its
// last reply. Shared with its calls in flight; thread-safe.
class FileRelay {
public:
    using Done = std::function<void(Status status, JsonVariantConst body)>;
    // A download's reply: its body without "data", and the data apart (the
    // base64 text as the peer wrote it, valid during the call), so a chunk
    // is never copied into a document.
    using Chunk = std::function<void(Status status, JsonVariantConst body, std::string_view data)>;
    // The smallest part a relay halves its parts to when memory is short
    // (512 bytes of base64).
    static constexpr size_t kMinPart = 384;
    virtual ~FileRelay() = default;

    // Claims the peer (Api::claimTransfer()); false, with busy written to
    // `reply`, if another transfer to it is in progress.
    virtual bool claim(Reply& reply) = 0;
    // Asks the peer for the path's schema, at depth 0: whether it is a file
    // node. Needs no claim.
    virtual void probe(Done done) = 0;
    // What a probe found, remembered while the peer's schema is unchanged:
    // true, with `isFile`, when known, so the probe is skipped
    // (docs/DESIGN.md section 14).
    virtual bool knownFile(bool& isFile) const {
        (void)isFile;
        return false;
    }
    virtual void learnedFile(bool isFile) { (void)isFile; }
    // Upload: opens it, without data. `done` gets {"received": 0, "chunk": n},
    // or the upload's reply when `size` is 0.
    virtual void openUpload(uint64_t size, std::string_view filename, Done done) = 0;
    // Download: the name the client gave (FileRequest::filename), sent with
    // the chunk that opens it (offset 0). Set before the first download().
    virtual void downloadName(std::string_view filename) = 0;
    // Upload: sends `data`, the bytes from received() on, in the peer's
    // chunks. `done` gets the last reply: {"received": n}, or the upload's
    // reply once all `size` bytes are in (finished()).
    virtual void write(std::vector<uint8_t> data, Done done) = 0;
    // Download: the chunk at `offset`, up to `length` bytes (0: the peer's
    // largest). Offset 0 opens the download. `done` gets the peer's reply.
    virtual void download(uint64_t offset, size_t length, Chunk done) = 0;
    // Ends the transfer here and releases the peer. The peer's side ends
    // when it has been idle for its Config::transferIdleMs, or with the next
    // transfer relayed to it.
    virtual void close() = 0;

    // Writes a reply of the peer to `reply`, under the local path: error
    // paths are mapped back as for a forwarded get or set.
    virtual void writeReply(Reply& reply, Status status, JsonVariantConst body) const = 0;
    // Upload: bytes the peer has. Both: the peer's largest chunk (0 before
    // it said).
    virtual uint64_t received() const = 0;
    virtual size_t chunk() const = 0;
    // A call is in flight.
    virtual bool busy() const = 0;
    // The upload is complete, or the download reached its end.
    virtual bool finished() const = 0;
    // Not closed (an error or the upload's end close it).
    virtual bool open() const = 0;
    // Taken over while idle (Config::transferIdleMs).
    virtual bool expired() const = 0;
    // How long a waiting caller should give one call: the node's timeout,
    // times the attempts.
    virtual uint32_t patienceMs() const = 0;
};

// One file transfer (docs/DESIGN.md section 4.6), driven by a transport:
// open, then write() / read() chunks, then finish(). Opening resolves the
// path, checks access and calls the node's handler under the API lock; the
// chunks run outside it. One transfer per path at a time; one that moves no
// bytes for Config::transferIdleMs is taken over by the next. Destroying an
// unfinished upload aborts it (its sink is destroyed without finish()).
// Driven by one task; only a takeover comes from another.
//
// Below a remote node (docs/DESIGN.md section 14), a transfer is relayed to
// the peer. With Remotes::Wait, this transfer relays it itself, each call
// waiting for the peer's replies, so it must not run in the task that
// processes the peer's endpoint (HTTP does this). Opening first asks the
// peer for the path's schema: a path that isn't a file node there is
// NotFile, as locally. With Remotes::Return, opening returns Open::Remote
// with the relay in relay(), for the caller to drive without waiting
// (FileSession does this).
class FileTransfer : public TransferHolder {
public:
    enum class Open {
        NotFile,  // the path isn't a file node: handle the request as usual
        Failed,   // refused: the error was written to the reply
        Ok,
        Remote,   // below a remote node: relay() is open (Remotes::Return)
    };
    enum class Remotes { Return, Wait };

    explicit FileTransfer(Api& api, Remotes remotes = Remotes::Return) : api_(api), remotes_(remotes) {}
    ~FileTransfer();
    FileTransfer(const FileTransfer&) = delete;
    FileTransfer& operator=(const FileTransfer&) = delete;

    // An upload of `size` bytes to `path`, under the name the client gave
    // (empty if none: FileRequest::filename).
    Open openUpload(std::string_view path, const Client& client, uint64_t size, Reply& reply,
                    std::string_view filename = std::string_view());
    // A download of `path`, under the name the client gave (empty if none:
    // FileRequest::filename).
    Open openDownload(std::string_view path, const Client& client, Reply& reply,
                      std::string_view filename = std::string_view());

    // Upload: the next bytes. On failure the error is written to `reply`
    // and the upload is over.
    bool write(const uint8_t* data, size_t len, Reply& reply);
    // Upload: all bytes arrived. Writes the reply (the sink's body, or an
    // error, also when fewer bytes than announced arrived).
    void finish(Reply& reply);

    // Download: total bytes, or -1 if unknown.
    int64_t size() const;
    // Download: up to `cap` bytes; 0 at the end, -1 on error.
    int read(uint8_t* buf, size_t cap);
    const std::string& filename() const { return filename_; }
    const char* contentType() const { return contentType_.c_str(); }
    const std::string& path() const { return path_; }
    // Upload: bytes announced, and written so far.
    uint64_t expected() const { return expected_; }
    uint64_t received() const { return received_; }
    // An upload or download is open.
    bool active() const;
    // The last transfer was taken over while idle (Config::transferIdleMs).
    bool expired() const { return expired_ || (relay_ && relay_->expired()); }
    // Ends the transfer: an unfinished upload is aborted.
    void cancel();
    // The relay of a transfer below a remote node, or null.
    const std::shared_ptr<FileRelay>& relay() const { return relay_; }

    // Used by Api::claimTransfer(), under the API lock: aborts the transfer
    // if it has been idle for `idleMs` (0: never) and isn't busy. True if it
    // did, releasing its claim.
    bool expireIfIdle(uint32_t nowMs, uint32_t idleMs) override;

private:
    Open open(std::string_view path, const Client& client, Op op, uint64_t size, std::string_view filename,
              Reply& reply);
    Open openLocal(FileNode& node, const Client& client, Op op, uint64_t size, std::string_view filename,
                   Reply& reply);
    Open openRelay(Op op, uint64_t size, std::string_view filename, Reply& reply);
    // Relayed, waiting for the peer. Upload: sends the buffered bytes (all,
    // or whole chunks only); false with the error written. Download: the
    // next chunk into relayBuf_.
    bool flushRelay(Reply& reply, bool all);
    bool fetchRelay();
    void takeChunk(JsonVariantConst body, std::vector<uint8_t> data);
    void close();

    Api& api_;
    Remotes remotes_;
    // Held by each call, so a takeover never lands in the middle of one.
    mutable Mutex mutex_;
    bool claimed_ = false;  // path_ is claimed in the API
    bool expired_ = false;  // taken over while idle
    uint32_t activeMs_ = 0;
    std::unique_ptr<FileSink> sink_;
    std::unique_ptr<FileSource> source_;
    std::string path_;
    std::string filename_;
    std::string contentType_ = "application/octet-stream";  // copied: an element's node is gone after open
    uint64_t expected_ = 0;
    uint64_t received_ = 0;
    // Relayed (Remotes::Wait): bytes not yet sent (upload) or read
    // (download), the upload's reply, the download's size and end.
    std::shared_ptr<FileRelay> relay_;
    std::vector<uint8_t> relayBuf_;
    size_t relayAt_ = 0;
    std::string relayReply_;
    int64_t relaySize_ = -1;
    uint64_t relayPosition_ = 0;
    bool relayEnd_ = false;
};

}  // namespace tesser

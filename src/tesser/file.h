#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>

#include "tesser/file_session.h"
#include "tesser/node.h"
#include "tesser/platform.h"
#include "tesser/request.h"

namespace tesser {

class Api;

// One file transfer (docs/DESIGN.md section 4.6), driven by a transport:
// open, then write() / read() chunks, then finish(). Opening resolves the
// path, checks access and calls the node's handler under the API lock; the
// chunks run outside it. One transfer per path at a time; one that moves no
// bytes for Config::transferIdleMs is taken over by the next. Destroying an
// unfinished upload aborts it (its sink is destroyed without finish()).
// Driven by one task; only a takeover comes from another.
class FileTransfer {
public:
    enum class Open {
        NotFile,  // the path isn't a file node: handle the request as usual
        Failed,   // refused: the error was written to the reply
        Ok,
    };

    explicit FileTransfer(Api& api) : api_(api) {}
    ~FileTransfer();
    FileTransfer(const FileTransfer&) = delete;
    FileTransfer& operator=(const FileTransfer&) = delete;

    // An upload of `size` bytes to `path`, under the name the client gave
    // (empty if none: FileRequest::filename).
    Open openUpload(std::string_view path, const Client& client, uint64_t size, Reply& reply,
                    std::string_view filename = std::string_view());
    // A download of `path`.
    Open openDownload(std::string_view path, const Client& client, Reply& reply);

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
    bool expired() const { return expired_; }
    // Ends the transfer: an unfinished upload is aborted.
    void cancel();

    // Used by Api::claimTransfer(), under the API lock: aborts the transfer
    // if it has been idle for `idleMs` (0: never) and isn't busy. True if it
    // did, releasing its claim.
    bool expireIfIdle(uint32_t nowMs, uint32_t idleMs);

private:
    Open open(std::string_view path, const Client& client, Op op, uint64_t size, std::string_view filename,
              Reply& reply);
    void close();

    Api& api_;
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
};

}  // namespace tesser

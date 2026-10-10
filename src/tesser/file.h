#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>

#include "tesser/node.h"
#include "tesser/request.h"

namespace tesser {

class Api;

// One file transfer (docs/DESIGN.md section 4.6), driven by a transport:
// open, then write() / read() chunks, then finish(). Opening resolves the
// path and checks access under the API lock; the chunks run outside it.
// Destroying an unfinished upload aborts it (its sink is destroyed without
// finish()).
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

    // An upload of `size` bytes to `path`.
    Open openUpload(std::string_view path, const Client& client, uint64_t size, Reply& reply);
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
    const char* contentType() const { return node_ ? node_->contentType() : "application/octet-stream"; }
    const std::string& path() const { return path_; }

private:
    Open open(std::string_view path, const Client& client, Op op, uint64_t size, Reply& reply);
    void close();

    Api& api_;
    FileNode* node_ = nullptr;
    std::unique_ptr<FileSink> sink_;
    std::unique_ptr<FileSource> source_;
    std::string path_;
    std::string filename_;
    uint64_t expected_ = 0;
    uint64_t received_ = 0;
};

}  // namespace tesser

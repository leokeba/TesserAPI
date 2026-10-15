#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/request.h"

namespace tesser {

class Api;
struct Config;

// File transfers over a message transport (docs/DESIGN.md section 10.1.1): the
// "upload" and "download" envelopes of one client, which move a file node's
// bytes in base64 chunks, each one a request carrying its offset. One
// transfer at a time; a new one replaces the last. Platform-independent: the
// serial, Bluetooth, WebSocket and NowTP transports keep one per client and
// hand it these envelopes. Holds nothing while no transfer is open. A path
// below a remote node is relayed to the peer without waiting for it: each
// envelope's reply is deferred until the peer's comes.
class FileSession {
public:
    explicit FileSession(Api& api);
    ~FileSession();
    FileSession(const FileSession&) = delete;
    FileSession& operator=(const FileSession&) = delete;

    // Whether `envelope` is a file envelope ("op": "upload" or "download").
    static bool handles(JsonObjectConst envelope);
    // Handles one, from `client`, replying through `reply`. The node's
    // handler runs under the API lock, the transfer itself outside it.
    // `raw`: the envelope's text, which an upload's "data" is read from
    // when the parsed envelope left it out (uploadFilter()).
    void handle(JsonObjectConst envelope, const Client& client, Reply& reply, std::string_view raw = {});
    // Ends the transfer in progress: the client is gone or its level fell.
    // An unfinished upload is aborted.
    void reset();
    // A transfer is open, or its last chunk can still be sent again.
    bool busy() const;
    // Nothing in progress: no transfer, or a download whose last chunk went
    // out (it can still be asked for again, until another transfer needs
    // the room).
    bool done() const;

    // The largest chunk of file data, in bytes, that an upload envelope may
    // carry and a download reply carries, under `config`'s limits.
    static size_t uploadChunk(const Config& config);
    static size_t downloadChunk(const Config& config);

private:
    struct State;
    void upload(JsonObjectConst envelope, std::string_view raw, std::string_view path, uint64_t offset,
                const Client& client,
                Reply& reply);
    void download(JsonObjectConst envelope, std::string_view path, uint64_t offset, const Client& client,
                  Reply& reply);
    // Below a remote node: the transfer is relayed (docs/DESIGN.md section
    // 14), and the reply deferred until the peer's comes.
    void relayUpload(bool opening, uint64_t size, std::string_view filename, uint64_t offset,
                     std::vector<uint8_t> data, Reply& reply);
    void relayDownload(bool opening, uint64_t offset, size_t length, size_t max, bool crc, Reply& reply);

    Api& api_;
    std::unique_ptr<State> state_;
};

namespace detail {
// Standard base64 with padding (RFC 4648).
void base64Encode(const uint8_t* data, size_t len, std::string& out);
// False on a character outside the alphabet or a bad length.
bool base64Decode(std::string_view text, std::vector<uint8_t>& out);
// CRC-32 (IEEE 802.3, as zlib's crc32()) of `len` bytes, continuing `crc`.
uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);
// The same of the bytes `text` encodes, decoded a group at a time (no
// buffer); false if it isn't base64.
bool crc32OfBase64(std::string_view text, uint32_t& crc);
}  // namespace detail

}  // namespace tesser

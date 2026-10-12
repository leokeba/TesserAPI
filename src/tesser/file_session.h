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
// hand it these envelopes. Holds nothing while no transfer is open.
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
    void handle(JsonObjectConst envelope, const Client& client, Reply& reply);
    // Ends the transfer in progress: the client is gone or its level fell.
    // An unfinished upload is aborted.
    void reset();
    // A transfer is open, or its last chunk can still be sent again.
    bool busy() const { return state_ != nullptr; }

    // The largest chunk of file data, in bytes, that an upload envelope may
    // carry and a download reply carries, under `config`'s limits.
    static size_t uploadChunk(const Config& config);
    static size_t downloadChunk(const Config& config);

private:
    struct State;
    void upload(JsonObjectConst envelope, std::string_view path, uint64_t offset, const Client& client,
                Reply& reply);
    void download(JsonObjectConst envelope, std::string_view path, uint64_t offset, const Client& client,
                  Reply& reply);

    Api& api_;
    std::unique_ptr<State> state_;
};

namespace detail {
// Standard base64 with padding (RFC 4648).
void base64Encode(const uint8_t* data, size_t len, std::string& out);
// False on a character outside the alphabet or a bad length.
bool base64Decode(std::string_view text, std::vector<uint8_t>& out);
}  // namespace detail

}  // namespace tesser

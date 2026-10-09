#pragma once

#include <stdint.h>

namespace tesser {

// Transport-neutral outcome of a request. Each transport encodes it its own
// way (HTTP status code, envelope "status" string, ...).
enum class Status : uint8_t {
    Ok,
    BadRequest,    // malformed request, envelope, body or query
    NotFound,      // path or patch key does not exist
    InvalidValue,  // type mismatch, out of range, string too long
    ReadOnly,      // write to a read-only value
    NotAllowed,    // operation not valid on this node
    Unauthorized,  // rejected by the authorizer
    TooLarge,      // request body or response exceeds the configured limit
    Busy,          // queue full, too many pending calls
    Timeout,       // deferred call did not complete in time
    Internal,      // bug, out of memory, reply dropped
};

// Wire name: "ok", "bad_request", ...
const char* toString(Status s);

// Parses a wire name; returns false if unknown.
bool parseStatus(const char* name, Status& out);

int httpCode(Status s);

}  // namespace tesser

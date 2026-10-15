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

// Why a request failed, for clients that act on it (docs/DESIGN.md section
// 10.1.2): an error body's "reason", next to its English "message", which
// may change or be compiled out. None: no reason given.
enum class Reason : uint8_t {
    None,
    LowMemory,          // busy: not enough heap for the reply or the remote's state; try again, or ask for less
    PendingLimit,       // busy: too many calls waiting for remote nodes or deferred actions; try again soon
    QueueFull,          // busy: a request queue is full; try again soon
    Unreachable,        // busy: the remote node couldn't be sent to; try again soon
    SubscriptionLimit,  // busy: no more subscriptions or streams for this client or node; poll instead
    TransferBusy,       // busy: another file transfer holds the node or the path
    PeerOffline,        // timeout: the remote node is known to be offline; nothing was sent
    NoAnswer,           // timeout: the remote node or a deferred action didn't answer in time
    Idle,               // timeout: a file transfer was idle too long and was dropped
    CannotWait,         // not_allowed: this transport can't wait for a deferred reply or deliver notifications
    NotAuthorized,      // unauthorized: the client's level or the authorizer refuses it
    CeilingUnsupported, // unauthorized: a remote node too old to limit a gateway's client (only admins reach it)
    UnknownOption,      // bad_request: a strict request ("v") named an option this node doesn't know
    SendFailed,         // timeout: the link couldn't deliver to the remote node, now offline
    AuthFailed,         // timeout: the remote node holds another pairing key (NowTP), or none
    Rejected,           // internal: the remote node refused the message (no listener on the port, too large)
    LineOverrun,        // busy: bytes of the request were lost on the line (a UART overrun); send it again
    Checksum,           // bad_request: a file chunk's data doesn't match its crc32; send it again
};

// Wire name: "low_memory", ...; "" for None.
const char* toString(Reason r);
// False if unknown (a newer node's reason: act on the status alone).
bool parseReason(const char* name, Reason& out);

// The envelope protocol this library speaks (docs/DESIGN.md section 10.1.2).
// 1: every node before versions were announced.
constexpr int kProtocolVersion = 2;

// Wire name: "ok", "bad_request", ...
const char* toString(Status s);

// Parses a wire name; returns false if unknown.
bool parseStatus(const char* name, Status& out);

int httpCode(Status s);

}  // namespace tesser

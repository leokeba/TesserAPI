#pragma once

// Internal helpers shared by a remote node's units (remote.cpp,
// remote_upstream.cpp, remote_stream.cpp, remote_relay.cpp).

#include <string>

#include <ArduinoJson.h>

#include "tesser/request.h"

namespace tesser {
namespace detail {

// Merges a change notification into a copy (objects merge recursively,
// anything else replaces).
void mergeJson(JsonVariant dst, JsonVariantConst patch);

// A remote node's reply, written to a local reply: error paths are mapped
// back under the local path of the remote node.
void writeMapped(Reply& reply, Status s, JsonVariantConst body, const std::string& localBase,
                 const std::string& remoteBase);

}  // namespace detail
}  // namespace tesser

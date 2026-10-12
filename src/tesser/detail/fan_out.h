// Internal: set requests with a '*' path segment (docs/DESIGN.md section 6.5).
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tesser/request.h"

namespace tesser {

class Api;

namespace detail {

// One node a '*' segment matched: a child's name, or a list element's key or
// index.
struct Match {
    std::string name;
    bool offline = false;  // a remote node whose peer is lost: reported, not sent to
};

// Runs `request` as one set per match, the '*' replaced by the match's name:
// `prefix` is the path before the '*' ("" for the root), `rest` the path after
// it ("" or "/..."), `path` the whole request path. Replies once every match
// has answered. Called by the request handler, under the API lock.
void fanOut(Api& api, const Request& request, Reply& reply, std::string_view prefix, std::string_view rest,
            std::string_view path, std::vector<Match>& matches);

// Handles a request on a handler allocated on the heap, keeping the caller's
// stack small. Takes the API lock (recursive).
void handleNested(Api& api, const Request& request, Reply& reply);

}  // namespace detail
}  // namespace tesser

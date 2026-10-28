#pragma once

#include <stdint.h>

#include <functional>
#include <utility>
#include <vector>

#include "tesser/platform.h"
#include "tesser/request.h"
#include "tesser/route.h"
#include "tesser/status.h"

namespace tesser {

class RemoteNode;

// What an application hears from the remote nodes of one Api, and the
// order their routes are tried in (docs/DESIGN.md section 14.3): each Api
// has its own (Api::remoteHooks()), so two applications in one program
// don't replace each other's, and each holds several handlers. A node uses
// its Api's once it knows it (from its first tick), the process-wide ones
// (RemoteNode's statics) as well. Thread-safe.
class RemoteHooks {
public:
    using StateHandler = std::function<void(RemoteNode& node)>;
    using AccessHandler = std::function<void(RemoteNode& node, Status status, Access level)>;
    using Id = uint32_t;
    // As RemoteNode::onStateChange() and onAccess(), for this Api's nodes;
    // several of each. The id removes it (remove()).
    Id onStateChange(StateHandler fn);
    Id onAccess(AccessHandler fn);
    void remove(Id id);
    // The order of this Api's nodes without their own (empty, the default:
    // RemoteNode::setDefaultRouteOrder()'s).
    void setRouteOrder(std::vector<RouteKind> order);
    std::vector<RouteKind> routeOrder() const;

    // For RemoteNode: calls the handlers, without the hooks' lock.
    void stateChanged(RemoteNode& node) const;
    void accessAnswered(RemoteNode& node, Status status, Access level) const;

private:
    mutable Mutex mutex_;  // a leaf: handlers are copied out, then called
    Id next_ = 1;
    std::vector<std::pair<Id, StateHandler>> state_;
    std::vector<std::pair<Id, AccessHandler>> access_;
    std::vector<RouteKind> order_;
};

}  // namespace tesser

#include "tesser/remote_hooks.h"

#include "tesser/detail/remote_routes.h"

namespace tesser {

RemoteHooks::Id RemoteHooks::onStateChange(StateHandler fn) {
    MutexGuard guard(mutex_);
    Id id = next_++;
    state_.emplace_back(id, std::move(fn));
    return id;
}

RemoteHooks::Id RemoteHooks::onAccess(AccessHandler fn) {
    MutexGuard guard(mutex_);
    Id id = next_++;
    access_.emplace_back(id, std::move(fn));
    return id;
}

void RemoteHooks::remove(Id id) {
    MutexGuard guard(mutex_);
    for (size_t i = 0; i < state_.size(); i++) {
        if (state_[i].first == id) state_.erase(state_.begin() + static_cast<std::ptrdiff_t>(i--));
    }
    for (size_t i = 0; i < access_.size(); i++) {
        if (access_[i].first == id) access_.erase(access_.begin() + static_cast<std::ptrdiff_t>(i--));
    }
}

void RemoteHooks::setRouteOrder(std::vector<RouteKind> order) {
    {
        MutexGuard guard(mutex_);
        order_ = std::move(order);
    }
    detail::routeOrderChanged();
}

std::vector<RouteKind> RemoteHooks::routeOrder() const {
    MutexGuard guard(mutex_);
    return order_;
}

void RemoteHooks::stateChanged(RemoteNode& node) const {
    std::vector<StateHandler> fns;
    {
        MutexGuard guard(mutex_);
        for (const auto& s : state_) fns.push_back(s.second);
    }
    for (const StateHandler& fn : fns) {
        if (fn) fn(node);
    }
}

void RemoteHooks::accessAnswered(RemoteNode& node, Status status, Access level) const {
    std::vector<AccessHandler> fns;
    {
        MutexGuard guard(mutex_);
        for (const auto& a : access_) fns.push_back(a.second);
    }
    for (const AccessHandler& fn : fns) {
        if (fn) fn(node, status, level);
    }
}

}  // namespace tesser

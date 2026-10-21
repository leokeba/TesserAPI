#pragma once

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "tesser/datagram.h"
#include "tesser/request.h"

namespace tesser {

// An entry of a route order (docs/DESIGN.md section 14.3): a transport, and
// optionally the interface it leaves by. A bare transport (TransportKind::Udp
// converts) matches its routes on any interface.
struct RouteKind {
    TransportKind transport = TransportKind::Local;
    NetInterface iface = NetInterface::Any;

    RouteKind() = default;
    RouteKind(TransportKind t, NetInterface i = NetInterface::Any) : transport(t), iface(i) {}  // NOLINT: implicit
    bool operator==(const RouteKind& o) const { return transport == o.transport && iface == o.iface; }
    bool operator!=(const RouteKind& o) const { return !(*this == o); }
    // This entry covers routes of `kind` leaving by `i`.
    bool covers(TransportKind kind, NetInterface i) const {
        return transport == kind && (iface == NetInterface::Any || iface == i);
    }
};
// "udp", "udp/ethernet".
std::string toString(const RouteKind& kind);
// "udp/ethernet, nowtp" (commas, spaces allowed): false, with `out`
// untouched, if an entry isn't a transport or transport/interface.
bool parseRouteOrder(std::string_view text, std::vector<RouteKind>& out);
// The other way round: "udp/ethernet, nowtp".
std::string formatRouteOrder(const std::vector<RouteKind>& order);

// A way to reach a remote node's device (RemoteNode::Route).
struct RemoteRoute {
    DatagramEndpoint* endpoint = nullptr;
    PeerAddress peer;
    bool online = true;
    uint32_t heardMs = 0;  // when a message last came this way (endpoint clock; 0: never)
    uint32_t rttMs = 0;    // smoothed round trip of the calls made this way (0: none yet)
    // The local interface it leaves by (UDP; Any when unknown or for
    // NowTP), as the endpoint saw it when the route came online.
    NetInterface iface = NetInterface::Any;
    TransportKind kind() const;
    // "nowtp", "udp/ethernet" (or "udp" when the interface is unknown).
    std::string name() const { return toString(RouteKind(kind(), iface)); }
};

}  // namespace tesser

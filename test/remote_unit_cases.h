// A remote node's units on their own, without a node, an endpoint or a link
// (docs/DESIGN.md section 16): the streamed-set queue and the route table.
#pragma once

#include <string>
#include <vector>

#include "check.h"
#include "tesser/detail/remote_routes.h"
#include "tesser/detail/remote_stream.h"

namespace remote_unit_cases {

using tesser::Status;
using tesser::detail::SetStream;

inline ArduinoJson::JsonDocument parsed(const char* json) {
    ArduinoJson::JsonDocument d;
    deserializeJson(d, json);
    return d;
}

TEST(unit_stream_merges_and_orders) {
    SetStream s;
    CHECK_EQ(s.push("out", parsed("{\"level\":1}")), Status::Ok);  // "out" is "/out"
    CHECK_EQ(s.push("/out/", parsed("{\"on\":true}")), Status::Ok);  // merged
    CHECK_EQ(s.push("/out/level", parsed("2")), Status::Ok);  // below it: queued after
    CHECK_EQ(s.push("/out", parsed("{\"level\":3}")), Status::Ok);  // can't merge past it
    CHECK_EQ(s.size(), 3u);
    CHECK_EQ(s.stats().queued, 4u);
    CHECK_EQ(s.stats().merged, 1u);

    std::string path, body;
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/out");
    CHECK_EQ(body, "{\"level\":1,\"on\":true}");
    CHECK(!s.next(true, path, body));  // one in flight at a time
    CHECK(s.done(Status::Ok, ArduinoJson::JsonVariantConst()));
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/out/level");
    CHECK_EQ(body, "2");
    CHECK(s.done(Status::Ok, ArduinoJson::JsonVariantConst()));
    CHECK(s.next(true, path, body));
    CHECK_EQ(body, "{\"level\":3}");
    CHECK(s.done(Status::Ok, ArduinoJson::JsonVariantConst()));
    CHECK_EQ(s.size(), 0u);
    CHECK_EQ(s.stats().sent, 3u);
}

TEST(unit_stream_limit_and_retries) {
    SetStream s;
    s.setLimit(2);
    CHECK_EQ(s.push("/a", parsed("{\"x\":1}")), Status::Ok);
    CHECK_EQ(s.push("/b", parsed("{\"x\":1}")), Status::Ok);
    CHECK_EQ(s.push("/c", parsed("{\"x\":1}")), Status::Busy);
    CHECK_EQ(s.push("/b", parsed("{\"y\":2}")), Status::Ok);  // merging needs no room

    std::vector<std::string> failed;
    s.onError([&](const std::string& path, Status st, ArduinoJson::JsonVariantConst) {
        failed.push_back(path);
        CHECK_EQ(st, Status::Timeout);
    });
    std::string path, body;
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/a");
    // A newer set to the path comes while it is in flight: a timed-out one
    // goes again with it merged on top.
    CHECK_EQ(s.push("/a", parsed("{\"x\":5}")), Status::Ok);
    CHECK(s.done(Status::Timeout, ArduinoJson::JsonVariantConst()));
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/a");
    CHECK_EQ(body, "{\"x\":5}");
    // Busy: sent again later (the node's tick), not at once.
    CHECK(!s.done(Status::Busy, ArduinoJson::JsonVariantConst()));
    CHECK(s.next(true, path, body));
    CHECK(s.done(Status::Timeout, ArduinoJson::JsonVariantConst()));  // the third attempt
    CHECK_EQ(failed.size(), 1u);
    CHECK_EQ(s.stats().failed, 1u);

    // Couldn't be sent: it waits at the front, not counted as sent.
    uint32_t sent = s.stats().sent;
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/b");
    s.unsent();
    CHECK_EQ(s.stats().sent, sent);
    CHECK_EQ(s.size(), 1u);

    // Unreachable: dropped and reported.
    CHECK(!s.next(false, path, body));
    CHECK_EQ(s.size(), 0u);
    CHECK_EQ(failed.size(), 2u);
    if (failed.size() == 2) CHECK_EQ(failed[1], "/b");
    CHECK_EQ(s.stats().failed, 2u);
    CHECK(!s.done(Status::Ok, ArduinoJson::JsonVariantConst()));  // nothing in flight
}

// Endpoints a route table points at: only their kind matters to it.
struct TwoLinks {
    tesser::DatagramEndpoint udp{nullptr, tesser::TransportKind::Udp, nullptr, nullptr};
    tesser::DatagramEndpoint nowtp{nullptr, tesser::TransportKind::NowTP, nullptr, nullptr};
    tesser::PeerAddress a = tesser::PeerAddress::fromBytes(reinterpret_cast<const uint8_t*>("\x0a\0\0\1"), 4);
    tesser::PeerAddress b = tesser::PeerAddress::fromBytes(reinterpret_cast<const uint8_t*>("\x0a\0\0\2"), 4);
    tesser::PeerAddress mac = tesser::PeerAddress::fromBytes(reinterpret_cast<const uint8_t*>("\1\2\3\4\5\6"), 6);
};

TEST(unit_routes_order_and_failover) {
    TwoLinks l;
    tesser::detail::RouteTable t;
    bool registered = true;
    CHECK(t.add(l.udp, l.a, tesser::NetInterface::Any, registered));
    CHECK(!registered);
    CHECK(t.add(l.nowtp, l.mac, tesser::NetInterface::Any, registered));
    CHECK(!t.add(l.nowtp, l.mac, tesser::NetInterface::Any, registered));  // has it
    CHECK(t.add(l.udp, l.b, tesser::NetInterface::Any, registered));
    CHECK(registered);  // a route through udp was there
    CHECK_EQ(t.count(&l.udp), 2u);
    CHECK_EQ(t.endpoints().size(), 2u);
    CHECK(!t.onlyThrough(&l.udp));

    // Without an order, in the order they were added.
    tesser::RemoteRoute r;
    CHECK(t.active(r, nullptr));
    CHECK(r.endpoint == &l.udp && r.peer == l.a);
    t.setOrder({tesser::TransportKind::NowTP});
    CHECK(t.active(r, nullptr));
    CHECK(r.endpoint == &l.nowtp);
    std::vector<tesser::RemoteRoute> all = t.list(nullptr);
    CHECK_EQ(all.size(), 3u);
    if (all.size() == 3) CHECK(all[0].endpoint == &l.nowtp && all[1].peer == l.a && all[2].peer == l.b);

    // The first in the order goes offline: the next online one.
    CHECK(!t.setOnline(&l.nowtp, l.mac, false, 100, false, tesser::NetInterface::Any));  // others still online
    CHECK(t.down(&l.nowtp, l.mac));
    CHECK(t.active(r, nullptr));
    CHECK(r.endpoint == &l.udp && r.peer == l.a);
    // All offline: the first in the order.
    CHECK(t.setAll(false));
    CHECK(!t.online());
    CHECK(t.active(r, nullptr));
    CHECK(r.endpoint == &l.nowtp);
    // Back, leaving by another interface now.
    CHECK(t.setOnline(&l.udp, l.b, true, 200, true, tesser::NetInterface::Ethernet));
    CHECK(t.online());
    CHECK(t.up(&l.udp, l.b));
    CHECK_EQ(t.lastHeardMs(), 200u);
    CHECK(t.active(r, nullptr));
    CHECK(r.peer == l.b && r.iface == tesser::NetInterface::Ethernet);

    // The table's own order empty: the process-wide default.
    t.setOrder({});
    tesser::detail::RouteTable::setDefaultOrder({tesser::RouteKind(tesser::TransportKind::Udp, tesser::NetInterface::Wifi)});
    all = t.list(nullptr);
    tesser::detail::RouteTable::setDefaultOrder({});
    if (all.size() == 3) CHECK(all[0].peer == l.a);  // udp/ethernet isn't udp/wifi: added order

    t.roundTrip(&l.udp, l.b, 80);
    t.roundTrip(&l.udp, l.b, 160);
    CHECK(t.active(r, nullptr));
    CHECK_EQ(r.rttMs, 90u);  // smoothed: an eighth of the new sample

    // Detached from an endpoint: its routes go.
    CHECK(t.detach(&l.udp));  // udp/b was the only route online: the table goes offline
    CHECK(!t.online());
    CHECK_EQ(t.count(&l.udp), 0u);
    CHECK(t.onlyThrough(&l.nowtp));
}

TEST(unit_routes_switch_probes_and_replace) {
    TwoLinks l;
    tesser::detail::RouteTable t;
    bool registered;
    t.add(l.udp, l.a, tesser::NetInterface::Any, registered);
    t.add(l.nowtp, l.mac, tesser::NetInterface::Any, registered);
    tesser::RemoteRoute r;
    t.active(r, nullptr);
    tesser::detail::RouteTable::Observed o = t.observe(r, true);
    CHECK(!o.switched && !o.stateChanged);
    t.announce();
    CHECK(t.observe(r, true).stateChanged);
    CHECK(!t.observe(r, true).stateChanged);

    // The route in use goes offline at 1000: the next tick sees the switch.
    t.setOnline(&l.udp, l.a, false, 1000, false, tesser::NetInterface::Any);
    t.active(r, nullptr);
    o = t.observe(r, t.online());
    CHECK(o.switched && o.stateChanged);
    CHECK(o.from.endpoint == &l.udp && !o.from.online);

    // Probes: 5 s after the loss, then waiting twice as long each time.
    CHECK(t.dueProbes(5999).empty());
    std::vector<tesser::RemoteRoute> p = t.dueProbes(6000);
    CHECK_EQ(p.size(), 1u);
    if (p.size() == 1) CHECK(p[0].peer == l.a);
    CHECK(t.dueProbes(15999).empty());
    CHECK_EQ(t.dueProbes(16000).size(), 1u);
    t.setProbe(0);
    CHECK(t.dueProbes(100000).empty());

    // An offline route through the endpoint takes a new address.
    CHECK(t.replaceOffline(l.udp, l.b, tesser::NetInterface::Wifi));
    CHECK(t.has(&l.udp, l.b) && !t.has(&l.udp, l.a));
    CHECK(!t.replaceOffline(l.nowtp, l.b, tesser::NetInterface::Any));  // online there

    // What the routes share.
    CHECK(t.setDevice(l.mac));
    CHECK(!t.setDevice(l.mac));
    CHECK(t.device() == l.mac);
    CHECK_EQ(t.setToken("secret").size(), 2u);
    CHECK_EQ(t.token(), "secret");
}

}  // namespace remote_unit_cases

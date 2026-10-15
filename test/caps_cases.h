// What peers can make a node hold is bounded (docs/DESIGN.md section 14.1):
// mounted nodes, routes per node, file transfers, subscribers, announcements.
#pragma once

#include <string>

#include "datagram_cases.h"
#include "remote_cases.h"
#include "subscription_cases.h"

namespace caps_cases {

using datagram_cases::addr;
using tesser::DatagramEndpoint;
using tesser::PeerAddress;
using tesser::Status;

// A gateway mounting announced peers under /peers, with a real device at
// address 2 (others announce, but nothing answers for them).
struct Rig {
    cases::Device device;
    datagram_cases::Net net;
    DatagramEndpoint deviceEnd;
    tesser::Api gateway;
    DatagramEndpoint gatewayEnd;
    tesser::Object& peers;
    cases::Lines lines;
    tesser::LineTransport line;

    static DatagramEndpoint::Options options() {
        DatagramEndpoint::Options o;
        o.maxMounted = 3;
        o.maxRoutesPerNode = 2;
        return o;
    }
    Rig()
        : deviceEnd(&device.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn()),
          gatewayEnd(&gateway, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn(), options()),
          peers(gateway.object("peers")),
          line(gateway, [this](const char* d, size_t n) { lines.add(d, n); }) {
        net.nodes = {{addr(1), &gatewayEnd}, {addr(2), &deviceEnd}};
        gatewayEnd.mountPeers(peers);
    }
    void seen(uint8_t a, const char* name, const PeerAddress& device = PeerAddress()) {
        gatewayEnd.peerSeen(addr(a), name, 0, device);
        gatewayEnd.process();
    }
    void lost(uint8_t a) {
        gatewayEnd.forgetPeer(addr(a));
        gatewayEnd.process();
    }
    std::string ask(const std::string& envelope) {
        lines.out.clear();
        std::string l = envelope + "\n";
        line.feed(l.data(), l.size());
        net.run();
        return lines.out.size() == 1 ? lines.out[0] : "<" + std::to_string(lines.out.size()) + " lines>";
    }
    std::string names() { return cases::get(gateway, "/peers", cases::depth(1)).body; }
};

TEST(caps_mounted_nodes) {
    Rig r;
    r.seen(2, "dev");
    r.seen(10, "a");
    r.seen(11, "b");
    CHECK_EQ(r.names(), "{\"dev\":null,\"a\":null,\"b\":null}");
    // At the cap, all online: the new peer isn't mounted.
    r.seen(12, "c");
    CHECK_EQ(r.names(), "{\"dev\":null,\"a\":null,\"b\":null}");
    CHECK_EQ(r.gatewayEnd.stats().unmounted, 1u);
    // A subscription keeps an offline node; one nothing uses makes room.
    CHECK(r.ask("{\"id\":1,\"op\":\"sub\",\"path\":\"/peers/dev/lamp\"}").find("\"status\":\"ok\"") !=
          std::string::npos);
    r.lost(2);
    r.lost(10);
    r.seen(12, "c");
    CHECK_EQ(r.names(), "{\"dev\":null,\"b\":null,\"c\":null}");
    CHECK_EQ(r.gatewayEnd.stats().evicted, 1u);
    // Gone, the evicted node answers not found; the rest work.
    CHECK(r.ask("{\"id\":2,\"op\":\"get\",\"path\":\"/peers/a/lamp\"}").find("\"not_found\"") != std::string::npos);
    // The one heard longest ago goes first.
    r.net.clock += 1000;
    r.seen(11, "b");  // heard again
    r.lost(11);
    r.lost(12);
    r.seen(13, "d");
    CHECK_EQ(r.names(), "{\"dev\":null,\"b\":null,\"d\":null}");
    // Without its subscription, the device's node is like the others.
    CHECK(r.ask("{\"id\":3,\"op\":\"unsub\",\"path\":\"/peers/dev/lamp\"}").find("\"status\":\"ok\"") !=
          std::string::npos);
    r.lost(13);
    r.seen(14, "e");  // the device answered the unsub, so it is online again: b goes
    CHECK_EQ(r.names(), "{\"dev\":null,\"d\":null,\"e\":null}");
    CHECK_EQ(r.gatewayEnd.stats().evicted, 3u);
    // A relayed transfer holds its node too.
    auto* d = static_cast<tesser::RemoteNode*>(r.peers.child("d"));
    CHECK(d != nullptr);
    if (d) d->relayHeld(true);
    r.lost(14);
    r.seen(15, "f");  // d is held: e goes
    CHECK_EQ(r.names(), "{\"dev\":null,\"d\":null,\"f\":null}");
    if (d) d->relayHeld(false);
    // A node declared by the application is never evicted, nor counted.
    r.gateway.remote("kitchen", r.gatewayEnd, addr(2));
    r.lost(15);
    r.seen(16, "g");
    CHECK(r.gateway.child("kitchen") != nullptr);
}

TEST(caps_mirrored_node_evicted) {
    // A mirrored node keeps a copy and a subscription upstream: evicted,
    // it lets go of both (ASan checks).
    Rig r;
    r.gatewayEnd.mountPeers(r.peers, 200);
    r.seen(2, "dev");
    r.seen(10, "a");
    r.seen(11, "b");
    for (int i = 0; i < 3; i++) {
        r.net.clock += 300;
        r.gatewayEnd.process();
        r.net.run();
        r.gateway.poll(r.net.clock);
    }
    auto* dev = static_cast<tesser::RemoteNode*>(r.peers.child("dev"));
    CHECK(dev && dev->mirrored());
    CHECK(dev && dev->hasCopy());
    r.lost(2);
    r.net.run();
    r.seen(12, "c");
    r.net.run();
    CHECK(r.peers.child("dev") == nullptr);  // a and b online: the mirrored one went
    CHECK_EQ(r.gatewayEnd.stats().evicted, 1u);
    r.net.clock += 1000;
    r.gatewayEnd.process();
    r.net.run();
    r.gateway.poll(r.net.clock);
}

TEST(caps_routes_per_node) {
    Rig r;
    PeerAddress dev = addr(40);
    // Over UDP a device is what the peer says it is: many addresses can
    // announce it. Its node keeps two routes here; an offline one makes room.
    r.seen(20, "x", dev);
    r.seen(21, "x", dev);
    r.seen(22, "x", dev);
    auto* x = static_cast<tesser::RemoteNode*>(r.peers.child("x"));
    CHECK(x != nullptr);
    if (!x) return;
    CHECK_EQ(x->routeCount(&r.gatewayEnd), 2u);
    CHECK(!x->hasRoute(&r.gatewayEnd, addr(22)));
    r.lost(20);
    r.seen(22, "x", dev);
    CHECK_EQ(x->routeCount(&r.gatewayEnd), 2u);
    CHECK(x->hasRoute(&r.gatewayEnd, addr(22)));
    CHECK(!x->hasRoute(&r.gatewayEnd, addr(20)));
}

TEST(caps_peer_subscribers_and_file_sessions) {
    cases::Device d;
    std::string big(5000, 'b');
    d.api.file("big").text([&] { return big; }, nullptr);
    datagram_cases::Net net;
    DatagramEndpoint::Options o;
    o.maxFileSessions = 1;
    DatagramEndpoint ep(&d.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn(), o);
    auto ask = [&](uint8_t from, const std::string& text) {
        net.frames.clear();
        ep.receive(addr(from), text.data(), text.size(), true);
        ep.process();
        return net.frames.empty() ? std::string() : net.frames.back().text;
    };
    // A refused subscription leaves no subscriber behind; an unsub of the
    // last one neither.
    CHECK(ask(5, "{\"id\":1,\"op\":\"sub\",\"path\":\"/nope\"}").find("not_found") != std::string::npos);
    CHECK_EQ(ep.peerSubscribers(), 0u);
    CHECK(ask(5, "{\"id\":2,\"op\":\"sub\",\"path\":\"/lamp\"}").find("\"ok\"") != std::string::npos);
    CHECK_EQ(ep.peerSubscribers(), 1u);
    CHECK(ask(5, "{\"id\":3,\"op\":\"unsub\",\"path\":\"/lamp\"}").find("\"ok\"") != std::string::npos);
    CHECK_EQ(ep.peerSubscribers(), 0u);
    // One transfer at a time here: another peer's is busy while it is open,
    // and goes once it ends.
    CHECK(ask(6, "{\"id\":4,\"op\":\"download\",\"path\":\"/big\",\"length\":1000}").find("\"ok\"") !=
          std::string::npos);
    CHECK(ask(7, "{\"id\":5,\"op\":\"download\",\"path\":\"/big\",\"length\":1000}").find("too many transfers") !=
          std::string::npos);
    CHECK_EQ(ep.fileSessions(), 1u);
    for (int at = 1000; at < 5000; at += 1000) {
        ask(6, "{\"op\":\"download\",\"path\":\"/big\",\"offset\":" + std::to_string(at) + ",\"length\":1000}");
    }
    CHECK(ask(7, "{\"id\":6,\"op\":\"download\",\"path\":\"/big\",\"length\":1000}").find("\"ok\"") !=
          std::string::npos);
}

}  // namespace caps_cases

// Routes and streamed sets: a master reaching a device over UDP and NowTP,
// failing over between them, streaming state in order over lossy links, and
// the device noticing when its master is gone.
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"
#include "datagram_cases.h"
#include "udp_cases.h"

namespace route_cases {

using tesser::DatagramEndpoint;
using tesser::PeerAddress;
using tesser::RemoteNode;
using tesser::Status;
using tesser::TransportKind;
using tesser::UdpLink;
using udp_cases::ip;

inline std::string hex(const uint8_t* b, size_t n) {
    std::string s;
    char h[3];
    for (size_t i = 0; i < n; i++) {
        snprintf(h, sizeof(h), "%02x", b[i]);
        s += h;
    }
    return s;
}

// A device at 192.168.1.20 and MAC addr(0x7c), and a master at 192.168.1.1
// and addr(1) that mounts it under /peers over both links. One clock (the
// UDP network's) drives everything.
struct Rig {
    tesser::Api dev;
    int level = 0;
    bool on = false;
    std::vector<int> applied;  // levels in the order the device applied them
    std::string writer;        // the device of the last client that wrote the level
    udp_cases::Net net;
    datagram_cases::Net radio;
    UdpLink devUdp;
    DatagramEndpoint devNow;
    tesser::Api master;
    tesser::Object& peers;
    UdpLink gwUdp;
    DatagramEndpoint gwNow;
    PeerAddress mac = datagram_cases::addr(0x7c);
    PeerAddress masterMac = datagram_cases::addr(1);

    Rig()
        : devUdp(&dev, net.sender(ip(20)), net.clockFn()),
          devNow(&dev, TransportKind::NowTP, radio.sender(datagram_cases::addr(0x7c)), net.clockFn()),
          peers(master.object("peers")),
          gwUdp(&master, net.sender(ip(1)), net.clockFn()),
          gwNow(&master, TransportKind::NowTP, radio.sender(datagram_cases::addr(1)), net.clockFn()) {
        auto& out = dev.object("out");
        out.value(
            "level", [this] { return level; },
            [this](int v) {
                applied.push_back(v);
                level = v;
                const tesser::Client* c = dev.currentClient();
                writer = c ? hex(c->device, c->deviceLength) : "?";
            });
        out.value("on", on);
        net.nodes = {{ip(20), &devUdp}, {ip(1), &gwUdp}};
        radio.nodes = {{mac, &devNow}, {masterMac, &gwNow}};
        devUdp.setName("lamp");
        devUdp.setDeviceId(mac.bytes);
        devUdp.advertise();
        gwUdp.setName("master");
        gwUdp.setDeviceId(masterMac.bytes);
        gwUdp.advertise();
        gwNow.mountPeers(peers);
        gwUdp.mountPeers(peers);
        net.start();
        gwNow.peerSeen(mac, "lamp", dev.schemaHash());  // NowTP discovery
        run();
    }
    void run() {
        for (int i = 0; i < 10; i++) {
            net.run();
            radio.run();
        }
    }
    void advance(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            net.clock += 10;
            run();
        }
    }
    RemoteNode& node() { return *RemoteNode::byDevice(peers, mac); }
    TransportKind kind() {
        RemoteNode::Route r;
        return node().route(r) ? r.kind() : TransportKind::Local;
    }
    std::string stub() {
        tesser::Query q = cases::schema(1);
        q.keys = "lamp";
        return cases::get(master, "/peers", q).body;
    }
};

// Restores the library-wide settings a test changed.
struct Defaults {
    ~Defaults() {
        RemoteNode::setDefaultRouteOrder({});
        RemoteNode::onStateChange(nullptr);
    }
};

TEST(route_one_node_per_device) {
    Defaults d;
    Rig r;
    // One node, a route per link, in the order they were announced (UDP
    // first here).
    CHECK_EQ(cases::get(r.master, "/peers", cases::depth(1)).body, "{\"lamp\":null}");
    CHECK(RemoteNode::byDevice(r.peers, r.mac) != nullptr);
    CHECK(RemoteNode::byDevice(r.peers, datagram_cases::addr(0x7d)) == nullptr);
    std::vector<RemoteNode::Route> routes = r.node().routes();
    CHECK_EQ(routes.size(), 2u);
    CHECK(routes[0].kind() == TransportKind::Udp);
    CHECK(routes[0].peer == ip(20));
    CHECK(routes[1].kind() == TransportKind::NowTP);
    CHECK(r.kind() == TransportKind::Udp);
    // The default order, then the node's own.
    RemoteNode::setDefaultRouteOrder({TransportKind::NowTP});
    CHECK(r.kind() == TransportKind::NowTP);
    CHECK(r.node().routes()[0].kind() == TransportKind::NowTP);
    r.node().routeOrder({TransportKind::Udp, TransportKind::NowTP});
    CHECK(r.kind() == TransportKind::Udp);
    r.node().routeOrder({});
    CHECK(r.kind() == TransportKind::NowTP);
    RemoteNode::setDefaultRouteOrder({TransportKind::Udp, TransportKind::NowTP});
    CHECK(r.kind() == TransportKind::Udp);
    // The stub shows the route in use and the device.
    char hash[9];
    snprintf(hash, sizeof(hash), "%08lx", static_cast<unsigned long>(r.dev.schemaHash()));
    CHECK_EQ(r.stub(), std::string("{\"type\":\"object\",\"children\":{\"lamp\":{\"type\":\"remote\",\"route\":"
                                   "\"udp\",\"schema\":\"") +
                           hash + "\",\"id\":\"10521c75f37c\"}}}");
    // Requests go that way, and time the round trip.
    tesser::LocalClient client(r.master);
    udp_cases::Result got;
    client.get("/peers/lamp/out/level", [&](Status s, JsonVariantConst body) {
        got.calls++;
        got.status = s;
        serializeJson(body, got.body);
    });
    for (int i = 0; i < 5; i++) {
        r.run();
        client.process();
    }
    CHECK_EQ(got.status, Status::Ok);
    CHECK_EQ(got.body, "0");
    CHECK_EQ(r.gwNow.stats().requests + r.gwNow.stats().responses, 0u);
    CHECK(r.node().routes()[0].heardMs > 0);
}

TEST(route_mount_under_several_parents) {
    // An endpoint mounts its peers under every parent it was given: here
    // UDP's alone under /lan, next to /peers where both links meet.
    Defaults d;
    Rig r;
    auto& lan = r.master.object("lan");
    r.gwUdp.mountPeers(lan);
    r.gwUdp.mountPeers(r.peers, 0);  // again: no second mount
    r.advance(5100);                  // the next advertisement
    CHECK_EQ(cases::get(r.master, "/lan", cases::depth(1)).body, "{\"lamp\":null}");
    CHECK_EQ(cases::get(r.master, "/peers", cases::depth(1)).body, "{\"lamp\":null}");
    CHECK_EQ(RemoteNode::byDevice(lan, r.mac)->routes().size(), 1u);
    CHECK_EQ(r.node().routes().size(), 2u);
}

TEST(route_failover_and_back) {
    Defaults d;
    RemoteNode::setDefaultRouteOrder({TransportKind::Udp, TransportKind::NowTP});
    Rig r;
    RemoteNode& n = r.node();
    std::vector<std::string> states;
    RemoteNode::onStateChange([&](RemoteNode& node) {
        RemoteNode::Route cur;
        node.route(cur);
        states.push_back(std::string(node.online() ? "online " : "offline ") + tesser::toString(cur.kind()));
    });
    r.run();
    CHECK_EQ(n.stream("/out", "{\"level\":1}"), Status::Ok);
    r.run();
    CHECK_EQ(r.level, 1);
    states.clear();

    // The Ethernet cable is pulled: the set in flight, and those after it,
    // go over NowTP once UDP gives up (sendTimeoutMs), in order: the set in
    // flight again, with the newer ones merged on top.
    r.net.lose = [](const udp_cases::Net::Datagram&) { return true; };
    CHECK_EQ(n.stream("/out", "{\"level\":2}"), Status::Ok);
    CHECK_EQ(n.stream("/out", "{\"on\":true}"), Status::Ok);
    CHECK_EQ(n.stream("/out", "{\"level\":3}"), Status::Ok);  // merged with the one above
    CHECK_EQ(n.streaming(), 2u);
    r.advance(2900);
    CHECK_EQ(r.level, 1);
    r.advance(200);
    CHECK(r.kind() == TransportKind::NowTP);
    CHECK(n.online());
    CHECK_EQ(r.level, 3);
    CHECK(r.on);
    CHECK((r.applied == std::vector<int>{1, 3}));
    CHECK_EQ(n.streaming(), 0u);
    CHECK_EQ(n.streamStats().merged, 1u);
    CHECK_EQ(n.streamStats().failed, 0u);
    CHECK((states == std::vector<std::string>{"online nowtp"}));
    CHECK(r.stub().find("\"route\":\"nowtp\"") != std::string::npos);

    // Plugged back: the next advertisement brings UDP back.
    r.net.lose = nullptr;
    r.advance(5100);
    CHECK(r.kind() == TransportKind::Udp);
    CHECK((states == std::vector<std::string>{"online nowtp", "online udp"}));
    uint32_t radioRequests = r.devNow.stats().requests;
    CHECK_EQ(n.stream("/out", "{\"level\":4}"), Status::Ok);
    r.run();
    CHECK_EQ(r.level, 4);
    CHECK_EQ(r.devNow.stats().requests, radioRequests);
}

TEST(route_stream_in_order_under_loss) {
    Defaults d;
    Rig r;
    // UDP only, losing a datagram in three.
    r.radio.dropAll = true;
    r.gwNow.forgetPeer(r.mac);
    r.run();
    CHECK(r.kind() == TransportKind::Udp);
    int count = 0;
    r.net.lose = [&](const udp_cases::Net::Datagram&) { return ++count % 3 == 0; };
    RemoteNode& n = r.node();
    for (int v = 1; v <= 60; v++) {
        // A note on and its note off, sent while a set is in flight: merged.
        char body[48];
        snprintf(body, sizeof(body), "{\"level\":%d,\"on\":%s}", v, v % 2 ? "true" : "false");
        CHECK_EQ(n.stream("/out", body), Status::Ok);
        if (v % 4 == 0) r.advance(10);
    }
    r.advance(3000);
    CHECK_EQ(r.level, 60);
    CHECK(!r.on);
    CHECK(r.net.lost > 0);
    // Never an older value after a newer one.
    bool ordered = true;
    for (size_t i = 1; i < r.applied.size(); i++) ordered = ordered && r.applied[i] > r.applied[i - 1];
    CHECK(ordered);
    CHECK(r.applied.size() < 60u);
    CHECK_EQ(n.streamStats().queued, 60u);
    CHECK_EQ(n.streamStats().merged, 60u - r.applied.size());
    CHECK_EQ(n.streamStats().failed, 0u);
}

TEST(route_stream_queue) {
    Defaults d;
    Rig r;
    RemoteNode& n = r.node();
    CHECK_EQ(n.stream("/out", "not json"), Status::BadRequest);
    // Different paths keep their order; a path above or below one that is
    // queued isn't merged across it.
    r.radio.dropAll = true;  // nothing completes for now
    r.net.lose = [](const udp_cases::Net::Datagram&) { return true; };
    CHECK_EQ(n.stream("/out/level", "1"), Status::Ok);  // in flight
    CHECK_EQ(n.stream("/out/level", "2"), Status::Ok);
    CHECK_EQ(n.stream("/out", "{\"level\":3}"), Status::Ok);
    CHECK_EQ(n.stream("/out/level", "4"), Status::Ok);  // not merged into "2": "/out" is between
    CHECK_EQ(n.stream("/out", "{\"on\":true}"), Status::Ok);  // nor into the "/out" above: "4" is between
    CHECK_EQ(n.streaming(), 5u);
    CHECK_EQ(n.streamStats().merged, 0u);
    n.streamLimit(4);
    CHECK_EQ(n.stream("/out/on", "true"), Status::Busy);
    CHECK_EQ(n.stream("/out", "{\"on\":false}"), Status::Ok);  // the last one queued: merged
    CHECK_EQ(n.streamStats().merged, 1u);

    // Every route lost: the set in flight takes "2" on top of it, and then
    // what is queued is dropped, each with an error.
    std::vector<std::string> errors;
    n.onStreamError([&](const std::string& path, Status s, JsonVariantConst body) {
        errors.push_back(path + " " + tesser::toString(s) + " " + (body["message"] | ""));
    });
    r.gwNow.forgetPeer(r.mac);
    r.advance(3100);
    CHECK(!n.online());
    CHECK_EQ(errors.size(), 4u);
    if (errors.size() == 4) {
        CHECK_EQ(errors[0], "/out/level timeout peer offline");
        CHECK_EQ(errors[3], "/out timeout peer offline");
    }
    CHECK_EQ(n.streaming(), 0u);
    CHECK_EQ(n.streamStats().failed, 4u);
    // Known to be offline: refused at once.
    CHECK_EQ(n.stream("/out/level", "5"), Status::Timeout);
}

TEST(route_peer_lost_on_the_device) {
    Defaults d;
    Rig r;
    // The device knows the master's device: its advertisement over UDP, its
    // address over NowTP.
    CHECK_EQ(hex(r.devUdp.endpoint().deviceOf(ip(1)).bytes, 6), "10521c75f301");
    RemoteNode::setDefaultRouteOrder({TransportKind::NowTP});
    CHECK_EQ(r.node().stream("/out", "{\"level\":7}"), Status::Ok);
    r.run();
    CHECK_EQ(r.writer, "10521c75f301");
    RemoteNode::setDefaultRouteOrder({TransportKind::Udp});
    CHECK_EQ(r.node().stream("/out", "{\"level\":8}"), Status::Ok);
    r.run();
    CHECK_EQ(r.writer, "10521c75f301");
    CHECK_EQ(r.level, 8);
    CHECK(r.devNow.deviceOnline(r.masterMac));
    CHECK(r.devUdp.endpoint().deviceOnline(r.masterMac));
    // Outside a request there is no client.
    CHECK(r.dev.currentClient() == nullptr);

    std::vector<std::string> lost, all;
    r.devUdp.endpoint().onPeerLost([&](const PeerAddress& peer, const PeerAddress& device) {
        all.push_back(tesser::formatUdpAddress(peer) + " " + hex(device.bytes, device.length));
    });
    // The master leaves the LAN (a goodbye): still there over NowTP.
    r.gwUdp.stop();
    r.run();
    CHECK((all == std::vector<std::string>{"192.168.1.1:8484 10521c75f301"}));
    CHECK(!r.devUdp.endpoint().deviceOnline(r.masterMac));
    r.gwUdp.start(0x9999);
    r.run();
    CHECK(r.devUdp.endpoint().deviceOnline(r.masterMac));

    tesser::watchDevices({&r.devUdp.endpoint(), &r.devNow},
                         [&](const PeerAddress&, const PeerAddress& device) { lost.push_back(hex(device.bytes, 6)); });
    r.gwUdp.stop();
    r.run();
    CHECK(lost.empty());
    // Gone from NowTP too: now it is lost.
    r.devNow.forgetPeer(r.masterMac);
    r.devNow.process();
    CHECK((lost == std::vector<std::string>{"10521c75f301"}));
    // A peer forgotten again isn't reported twice.
    r.devNow.forgetPeer(r.masterMac);
    r.devNow.process();
    CHECK_EQ(lost.size(), 1u);
}

TEST(route_declared_device) {
    // A node declared by its UDP address takes the device it advertises,
    // and the device's other routes join it instead of mounting it again.
    Defaults d;
    Rig r;
    tesser::Api gw;
    auto& peers = gw.object("peers");
    UdpLink udp(&gw, r.net.sender(ip(2)), r.net.clockFn());
    DatagramEndpoint now(&gw, TransportKind::NowTP, r.radio.sender(datagram_cases::addr(2)), r.net.clockFn());
    r.net.nodes.push_back({ip(2), &udp});
    r.radio.nodes.push_back({datagram_cases::addr(2), &now});
    RemoteNode& declared = peers.remote("far-lamp", udp.endpoint(), ip(20));
    std::vector<RemoteNode*> announced;
    RemoteNode::onStateChange([&](RemoteNode& node) { announced.push_back(&node); });
    udp.mountPeers(peers);
    now.mountPeers(peers);
    udp.start(0x2222);
    r.run();
    CHECK_EQ(declared.device().length, 6);
    now.peerSeen(r.mac, "lamp", 1);
    r.run();
    CHECK_EQ(cases::get(gw, "/peers", cases::depth(1)).body, "{\"far-lamp\":null,\"master\":null}");
    CHECK_EQ(declared.routes().size(), 2u);
    CHECK(RemoteNode::byDevice(peers, r.mac) == &declared);
    // Created, then a route added: the hook hears of both.
    CHECK(!announced.empty());
    bool onlyDeclared = true;
    for (RemoteNode* a : announced) {
        onlyDeclared = onlyDeclared && (a == &declared || a == peers.child("master"));
    }
    CHECK(onlyDeclared);
}

}  // namespace route_cases

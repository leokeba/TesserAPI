// Remote nodes: a gateway forwarding to a device over an in-memory link.
#pragma once

#include <string>

#include "core_cases.h"
#include "datagram_cases.h"
#include "subscription_cases.h"

namespace remote_cases {

using datagram_cases::addr;
using tesser::Status;

// A gateway at addr(1) mounting the device at addr(2) as /kitchen; requests
// to the gateway come in through a serial line.
// Declaration order matters: endpoints must outlive the trees holding remote
// nodes that use them.
struct Rig {
    cases::Device device;
    tesser::EventNode& pressed;
    datagram_cases::Net net;
    tesser::DatagramEndpoint deviceEnd;
    tesser::DatagramEndpoint gatewayEnd;
    tesser::Api gateway;
    tesser::RemoteNode& kitchen;
    cases::Lines lines;
    tesser::LineTransport line;
    int local = 1;

    Rig()
        : pressed(device.api.object("button").event("pressed")),
          deviceEnd(&device.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn()),
          gatewayEnd(&gateway, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn()),
          kitchen(gateway.remote("kitchen", gatewayEnd, addr(2))),
          line(gateway, [this](const char* d, size_t n) { lines.add(d, n); }) {
        gateway.value("local", local);
        net.nodes = {{addr(1), &gatewayEnd}, {addr(2), &deviceEnd}};
    }

    std::string ask(const std::string& envelope) {
        lines.out.clear();
        std::string l = envelope + "\n";
        line.feed(l.data(), l.size());
        net.run();
        return lines.out.size() == 1 ? lines.out[0] : "<" + std::to_string(lines.out.size()) + " lines>";
    }
};

TEST(remote_forwarding) {
    Rig r;
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/kitchen/lamp\",\"keys\":\"on\"}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"on\":false}}");
    CHECK_EQ(r.ask("{\"id\":2,\"op\":\"set\",\"path\":\"/kitchen/lamp/brightness\",\"body\":9}"),
             "{\"id\":2,\"status\":\"ok\",\"body\":9}");
    CHECK_EQ(r.device.lamp.brightness, 9);
    CHECK_EQ(r.ask("{\"id\":3,\"op\":\"get\",\"path\":\"/kitchen\",\"depth\":1}"),
             "{\"id\":3,\"status\":\"ok\",\"body\":{\"lamp\":{},\"sensors\":{},\"config\":{},\"button\":{}}}");
    // Error paths come back as local paths.
    CHECK_EQ(r.ask("{\"id\":4,\"op\":\"get\",\"path\":\"/kitchen/nope/x\"}"),
             "{\"id\":4,\"status\":\"not_found\",\"body\":{\"error\":\"not_found\",\"path\":\"/kitchen/nope\","
             "\"message\":\"no such node\"}}");
    CHECK_EQ(r.ask("{\"id\":5,\"op\":\"set\",\"path\":\"/kitchen/lamp\",\"body\":{\"brightness\":999}}"),
             "{\"id\":5,\"status\":\"invalid_value\",\"body\":{\"error\":\"invalid_value\","
             "\"path\":\"/kitchen/lamp/brightness\",\"message\":\"out of range\"}}");
    // Without a mirror, a parent read shows null; the schema says what it is.
    CHECK_EQ(r.ask("{\"id\":6,\"op\":\"get\",\"path\":\"/\"}"),
             "{\"id\":6,\"status\":\"ok\",\"body\":{\"kitchen\":null,\"local\":1}}");
    CHECK_EQ(r.ask("{\"id\":7,\"op\":\"get\",\"path\":\"/\",\"view\":\"schema\",\"depth\":1}"),
             "{\"id\":7,\"status\":\"ok\",\"body\":{\"type\":\"object\",\"children\":{\"kitchen\":{\"type\":\"remote\"},"
             "\"local\":{\"type\":\"integer\",\"writable\":true}}}}");
    CHECK_EQ(r.ask("{\"id\":8,\"op\":\"get\",\"path\":\"/kitchen/sensors/serial\",\"view\":\"schema\"}"),
             "{\"id\":8,\"status\":\"ok\",\"body\":{\"type\":\"integer\"}}");
    // Patches can't span the gateway and a remote node.
    CHECK(r.ask("{\"id\":9,\"op\":\"set\",\"path\":\"/\",\"body\":{\"kitchen\":{}}}").find("not_allowed") !=
          std::string::npos);
    // Transports that can't wait get an error instead of a hang.
    CHECK_EQ(cases::get(r.gateway, "/kitchen/lamp").status, Status::NotAllowed);
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(remote_subpath_and_timeout) {
    Rig r;
    r.gateway.remote("light", r.gatewayEnd, addr(2), "/lamp/");
    r.gateway.remote("ghost", r.gatewayEnd, addr(9)).timeout(500);
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/light/on\"}"), "{\"id\":1,\"status\":\"ok\",\"body\":false}");
    CHECK_EQ(r.ask("{\"id\":2,\"op\":\"get\",\"path\":\"/light/x\"}"),
             "{\"id\":2,\"status\":\"not_found\",\"body\":{\"error\":\"not_found\",\"path\":\"/light/x\","
             "\"message\":\"no such node\"}}");
    CHECK_EQ(r.ask("{\"id\":3,\"op\":\"get\",\"path\":\"/light\"}"),
             "{\"id\":3,\"status\":\"ok\",\"body\":{\"on\":false,\"brightness\":128,\"label\":\"desk\"}}");

    CHECK_EQ(r.ask("{\"id\":4,\"op\":\"get\",\"path\":\"/ghost/a\"}"), "<0 lines>");
    r.net.clock += 600;
    r.gatewayEnd.process();
    CHECK_EQ(r.lines.out.size(), 1u);
    if (!r.lines.out.empty()) {
        CHECK_EQ(r.lines.out[0],
                 "{\"id\":4,\"status\":\"timeout\",\"body\":{\"error\":\"timeout\",\"path\":\"/ghost\","
                 "\"message\":\"remote node didn't answer\"}}");
    }
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(remote_mirror) {
    Rig r;
    r.kitchen.mirror(0, 30000);
    subscription_cases::FakeSubscriber local;
    // Below a remote node, subscriptions are forwarded, which needs a
    // transport that can wait; the mirrored node itself is subscribed locally.
    CHECK_EQ(subscription_cases::subscribe(r.gateway, &local, "/kitchen/lamp").status, Status::NotAllowed);
    r.net.run();  // the gateway's endpoint subscribes to the device
    CHECK(r.kitchen.hasCopy());
    CHECK_EQ(r.device.api.subscriptionCount(), 1u);
    CHECK(r.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/\",\"keys\":\"kitchen\"}")
              .find("\"kitchen\":{\"lamp\":{\"on\":false,\"brightness\":128,") != std::string::npos);
    CHECK_EQ(subscription_cases::subscribe(r.gateway, &local, "/", cases::keys("kitchen")).status, Status::Ok);

    // A change on the device updates the copy and reaches local subscribers.
    cases::set(r.device.api, "/lamp/brightness", "42");
    r.device.api.poll(1000);
    r.net.run();
    CHECK(r.ask("{\"id\":2,\"op\":\"get\",\"path\":\"/\",\"keys\":\"kitchen\"}").find("\"brightness\":42") !=
          std::string::npos);
    r.gateway.poll(1000);
    CHECK(local.last().find("{\"op\":\"change\",\"path\":\"/\",\"body\":{\"kitchen\":{\"lamp\":") == 0);
    CHECK(local.last().find("\"brightness\":42") != std::string::npos);

    // Events on the device come out under the gateway's path.
    r.pressed.emit(7);
    r.net.run();
    CHECK_EQ(local.last(), "{\"op\":\"event\",\"path\":\"/kitchen/button/pressed\",\"body\":7}");

    // Renewal after refreshMs replaces the subscription instead of adding one.
    r.net.clock += 31000;
    r.net.run();
    CHECK_EQ(r.device.api.subscriptionCount(), 1u);
    CHECK_EQ(r.gatewayEnd.stats().timeouts, 0u);
}

TEST(remote_forwarded_subscriptions) {
    Rig r;
    // The first subscriber waits for the upstream subscription's snapshot.
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"sub\",\"path\":\"/kitchen/lamp\",\"keys\":\"on\"}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"on\":false}}");
    CHECK_EQ(r.device.api.subscriptionCount(), 1u);
    CHECK_EQ(r.kitchen.upstreamCount(), 1u);
    // Later ones share it, and get their snapshot from its copy at once.
    subscription_cases::FakeSubscriber local;
    CHECK_EQ(subscription_cases::subscribe(r.gateway, &local, "/kitchen/lamp").body,
             "{\"on\":false,\"brightness\":128,\"label\":\"desk\"}");
    CHECK_EQ(r.device.api.subscriptionCount(), 1u);
    CHECK_EQ(r.gateway.subscriptionCount(), 2u);

    // Changes are relayed through each subscriber's own filter.
    r.lines.out.clear();
    cases::set(r.device.api, "/lamp/brightness", "42");
    r.device.api.poll(1000);
    r.net.run();
    CHECK_EQ(local.last(), "{\"op\":\"change\",\"path\":\"/kitchen/lamp\",\"body\":{\"brightness\":42}}");
    CHECK_EQ(r.lines.out.size(), 0u);  // keys=on: nothing for the serial client
    cases::set(r.device.api, "/lamp/on", "true");
    r.device.api.poll(1010);
    r.net.run();
    CHECK_EQ(r.lines.out.size(), 1u);
    if (!r.lines.out.empty()) {
        CHECK_EQ(r.lines.out[0], "{\"op\":\"change\",\"path\":\"/kitchen/lamp\",\"body\":{\"on\":true}}");
    }

    // The remote node itself, unmirrored: depth applies, events come through.
    CHECK_EQ(r.ask("{\"id\":2,\"op\":\"sub\",\"path\":\"/kitchen\",\"depth\":1,\"snapshot\":false}"),
             "{\"id\":2,\"status\":\"ok\",\"body\":null}");
    CHECK_EQ(r.kitchen.upstreamCount(), 2u);
    r.lines.out.clear();
    cases::set(r.device.api, "/lamp/brightness", "43");  // two levels down: not for depth 1
    r.device.api.poll(1020);
    r.net.run();
    CHECK_EQ(r.lines.out.size(), 0u);
    r.pressed.emit(4);  // two levels down too
    r.net.run();
    CHECK_EQ(r.lines.out.size(), 0u);
    CHECK_EQ(r.ask("{\"id\":4,\"op\":\"sub\",\"path\":\"/kitchen/button\"}"), "{\"id\":4,\"status\":\"ok\",\"body\":{}}");
    r.lines.out.clear();
    r.pressed.emit(5);
    r.net.run();
    CHECK_EQ(r.lines.out.size(), 1u);
    if (!r.lines.out.empty()) CHECK_EQ(r.lines.out[0], "{\"op\":\"event\",\"path\":\"/kitchen/button/pressed\",\"body\":5}");

    // Renewal replaces the upstream subscriptions; a change the remote never
    // announced comes out as the whole state.
    r.device.lamp.brightness = 77;  // no changed(): nobody is told
    r.net.clock += 31000;
    r.net.run();
    CHECK_EQ(r.device.api.subscriptionCount(), 3u);
    CHECK_EQ(local.last(),
             "{\"op\":\"change\",\"path\":\"/kitchen/lamp\",\"body\":{\"on\":true,\"brightness\":77,\"label\":\"desk\"}}");

    // The upstream subscription ends with its last local subscriber.
    CHECK_EQ(r.ask("{\"id\":3,\"op\":\"unsub\",\"path\":\"/kitchen/lamp\"}"), "{\"id\":3,\"status\":\"ok\",\"body\":1}");
    CHECK_EQ(r.kitchen.upstreamCount(), 3u);
    r.gateway.dropSubscriber(&local);
    r.net.run();
    CHECK_EQ(r.kitchen.upstreamCount(), 2u);
    CHECK_EQ(r.device.api.subscriptionCount(), 2u);
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(remote_forwarded_subscription_failures) {
    Rig r;
    r.gateway.remote("ghost", r.gatewayEnd, addr(9)).timeout(500);
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"sub\",\"path\":\"/ghost/a\"}"), "<0 lines>");
    CHECK_EQ(r.gateway.subscriptionCount(), 1u);  // waiting
    r.net.clock += 600;
    r.gatewayEnd.process();
    CHECK_EQ(r.lines.out.size(), 1u);
    if (!r.lines.out.empty()) {
        CHECK_EQ(r.lines.out[0],
                 "{\"id\":1,\"status\":\"timeout\",\"body\":{\"error\":\"timeout\",\"path\":\"/ghost\","
                 "\"message\":\"remote node didn't answer\"}}");
    }
    CHECK_EQ(r.gateway.subscriptionCount(), 0u);
    CHECK_EQ(r.gateway.pending(), 0);
    // A remote error comes back under the local path, and nothing is kept.
    CHECK_EQ(r.ask("{\"id\":2,\"op\":\"sub\",\"path\":\"/kitchen/nope\"}"),
             "{\"id\":2,\"status\":\"not_found\",\"body\":{\"error\":\"not_found\",\"path\":\"/kitchen/nope\","
             "\"message\":\"no such node\"}}");
    CHECK_EQ(r.gateway.subscriptionCount(), 0u);
    CHECK_EQ(r.kitchen.upstreamCount(), 0u);
    // A client that leaves while waiting is forgotten; the late upstream
    // subscription is undone.
    subscription_cases::FakeSubscriber gone;
    {
        // A detachable reply, as message transports have.
        struct Detachable : tesser::StringReply {
            Reply* detach() override { return new tesser::StringReply(); }
        } reply;
        tesser::Request req;
        req.op = tesser::Op::Subscribe;
        req.path = "/kitchen/sensors";
        req.subscriber = &gone;
        r.gateway.handle(req, reply);
    }
    r.gateway.dropSubscriber(&gone);
    CHECK_EQ(r.gateway.pending(), 0);
    r.net.run();
    CHECK_EQ(r.device.api.subscriptionCount(), 0u);
    CHECK_EQ(r.kitchen.upstreamCount(), 0u);
}

TEST(remote_discovered_peers) {
    Rig r;
    cases::Device other;
    tesser::DatagramEndpoint otherEnd(&other.api, tesser::TransportKind::NowTP, r.net.sender(addr(5)), r.net.clockFn());
    r.net.nodes.push_back({addr(5), &otherEnd});
    auto& peers = r.gateway.object("peers");
    r.gatewayEnd.mountPeers(peers);
    // A remote node's own reads are forwarded; its stub is in its parent's schema.
    auto stub = [&](const char* parent, const char* name) {
        tesser::Query q = cases::schema(1);
        q.keys = name;
        return cases::get(r.gateway, parent, q).body;
    };
    subscription_cases::FakeSubscriber watcher;
    CHECK_EQ(subscription_cases::subscribe(r.gateway, &watcher, "/peers", cases::depth(1)).body, "{}");

    // A peer mounted already (as /kitchen) isn't mounted again, but shows
    // the schema hash it advertises.
    r.gatewayEnd.peerSeen(addr(2), "kitchen", 0xabc);
    r.gatewayEnd.process();
    CHECK_EQ(stub("/", "kitchen"),
             "{\"type\":\"object\",\"children\":{\"kitchen\":{\"type\":\"remote\",\"schema\":\"00000abc\"}}}");

    // An announcement mounts the peer under its name; repeats don't.
    r.gatewayEnd.peerSeen(addr(5), "kitchen lamp!", 0x1234abcd);
    r.gatewayEnd.process();
    r.gatewayEnd.peerSeen(addr(5), "kitchen lamp!", 0x1234abcd);
    r.gatewayEnd.process();
    CHECK_EQ(cases::get(r.gateway, "/peers", cases::schema()).body,
             "{\"type\":\"object\",\"children\":{\"kitchen-lamp\":{\"type\":\"remote\",\"schema\":\"1234abcd\"}}}");
    r.gateway.poll(1000);
    CHECK_EQ(watcher.last(), "{\"op\":\"change\",\"path\":\"/peers\",\"body\":{\"kitchen-lamp\":null}}");
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/peers/kitchen-lamp/lamp/on\"}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":false}");

    // A name already taken gets the address's last bytes; an empty one is
    // only those.
    r.gatewayEnd.peerSeen(addr(3), "kitchen-lamp", 1);
    r.gatewayEnd.peerSeen(addr(4), "", 0);
    r.gatewayEnd.process();
    CHECK_EQ(cases::get(r.gateway, "/peers", cases::depth(1)).body,
             "{\"kitchen-lamp\":null,\"kitchen-lamp-75f303\":null,\"75f304\":null}");

    // Lost peers show offline until they are heard from again.
    r.gateway.poll(1050);
    watcher.messages.clear();
    r.gatewayEnd.forgetPeer(addr(5));
    r.gatewayEnd.process();
    CHECK_EQ(stub("/peers", "kitchen-lamp"), "{\"type\":\"object\",\"children\":{\"kitchen-lamp\":"
                                             "{\"type\":\"remote\",\"online\":false,\"schema\":\"1234abcd\"}}}");
    r.gateway.poll(1100);
    CHECK_EQ(watcher.last(), "{\"op\":\"change\",\"path\":\"/peers\",\"body\":{\"kitchen-lamp\":null}}");
    r.ask("{\"id\":2,\"op\":\"get\",\"path\":\"/peers/kitchen-lamp/lamp/on\"}");  // the device answers
    CHECK_EQ(stub("/peers", "kitchen-lamp"),
             "{\"type\":\"object\",\"children\":{\"kitchen-lamp\":{\"type\":\"remote\",\"schema\":\"1234abcd\"}}}");
    // A new schema hash is shown as soon as it is announced.
    r.gatewayEnd.peerSeen(addr(5), "kitchen lamp!", 0xfeed);
    r.gatewayEnd.process();
    CHECK_EQ(stub("/peers", "kitchen-lamp"),
             "{\"type\":\"object\",\"children\":{\"kitchen-lamp\":{\"type\":\"remote\",\"schema\":\"0000feed\"}}}");

    // Advertisements.
    std::string ad = tesser::buildAdvertisement(84, 0xfeed);
    CHECK_EQ(ad, "{\"tesser\":84,\"schema\":\"0000feed\"}");
    uint8_t port = 0;
    uint32_t hash = 0;
    CHECK(tesser::parseAdvertisement(ad, port, hash));
    CHECK_EQ(port, 84);
    CHECK_EQ(hash, 0xfeedu);
    CHECK(!tesser::parseAdvertisement("{\"name\":\"x\"}", port, hash));
}

TEST(remote_mutual_mirrors_dont_loop) {
    // Two gateways mounting each other: each mirror leaves out the other
    // side's mirrors, so copies never nest.
    datagram_cases::Net net;
    tesser::Api x, y;
    tesser::DatagramEndpoint ex(&x, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    tesser::DatagramEndpoint ey(&y, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    net.nodes = {{addr(1), &ex}, {addr(2), &ey}};
    int vx = 1, vy = 2;
    x.value("v", vx);
    y.value("v", vy);
    {
        tesser::RemoteNode& toY = x.remote("y", ex, addr(2));
        tesser::RemoteNode& toX = y.remote("x", ey, addr(1));
        toY.mirror(0);
        toX.mirror(0);
        net.run();
        CHECK_EQ(cases::get(x, "/").body, "{\"v\":1,\"y\":{\"v\":2}}");
        CHECK_EQ(cases::get(y, "/").body, "{\"v\":2,\"x\":{\"v\":1}}");
        CHECK_EQ(cases::get(x, "/", cases::depth(1)).body, "{\"v\":1,\"y\":{}}");
        tesser::Query noRemotes;
        noRemotes.remotes = false;
        CHECK_EQ(cases::get(x, "/", noRemotes).body, "{\"v\":1}");

        cases::set(y, "/v", "5");
        y.poll(1000);
        net.run();
        CHECK_EQ(cases::get(x, "/").body, "{\"v\":1,\"y\":{\"v\":5}}");
    }
    // The endpoints are destroyed before the trees: they detach the remote
    // nodes first.
}

}  // namespace remote_cases

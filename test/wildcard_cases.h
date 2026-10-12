// Set with a '*' path segment (docs/DESIGN.md section 6.5): one write fanned
// out to every child, locally and through remote nodes.
#pragma once

#include <string>
#include <vector>

#include "client_cases.h"
#include "core_cases.h"

namespace wildcard_cases {

using datagram_cases::addr;
using tesser::Status;

struct Fixture {
    struct Lamp {
        bool on = false;
        int level = 0;
    };
    struct Net {
        std::string ssid;
        bool dhcp = true;
    };
    Lamp a, b, c, hidden;
    const int count = 3;
    bool x = false, y = false;
    std::vector<Net> nets = {{"home", true}, {"work", true}};
    std::vector<Net> plain = {{"p", true}, {"q", true}};
    std::vector<int> arr = {1, 2};
    std::vector<tesser::Pending> pendings;
    tesser::Api api;

    Fixture() {
        auto& lamps = api.object("lamps");
        for (auto* l : {&a, &b, &c, &hidden}) {
            const char* name = l == &a ? "a" : l == &b ? "b" : l == &c ? "c" : "hidden";
            auto& o = lamps.object(name);
            o.value("on", l->on);
            o.value("level", l->level).range(0, 10);
            o.action("later", [this](tesser::Call& call) { pendings.push_back(call.defer()); });
            if (l == &hidden) o.readAccess(tesser::Access::User);
        }
        lamps.value("count", count);
        auto describe = [](tesser::Object& e, Net& n) {
            e.value("ssid", n.ssid);
            e.value("dhcp", n.dhcp);
        };
        api.list("nets", nets, describe).key("ssid");
        api.list("plain", plain, describe);
        api.array("arr", arr);
        auto& groups = api.object("groups");
        groups.object("g1").object("x").value("on", x);
        groups.object("g2").object("y").value("on", y);
    }
};

TEST(wildcard_set) {
    Fixture f;
    // Every child the client can read; one without the path (count) is skipped.
    auto r = cases::set(f.api, "/lamps/*/on", "true");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "{\"a\":true,\"b\":true,\"c\":true}");
    CHECK(f.a.on && f.b.on && f.c.on);
    CHECK(!f.hidden.on);
    // A patch for each; count can't take it.
    r = cases::set(f.api, "/lamps/*", "{\"level\":5}");
    CHECK_EQ(r.status, Status::ReadOnly);
    CHECK_EQ(r.body,
             "{\"error\":\"read_only\",\"path\":\"/lamps/*\",\"message\":\"1 of 4 failed: count: read-only\","
             "\"partial\":true,\"failed\":{\"count\":{\"error\":\"read_only\",\"path\":\"/lamps/count\","
             "\"message\":\"read-only\"}},\"ok\":{\"a\":{\"level\":5},\"b\":{\"level\":5},\"c\":{\"level\":5}}}");
    CHECK_EQ(f.c.level, 5);
    // Each match is validated on its own: all fail, nothing is partial.
    r = cases::set(f.api, "/lamps/*/level", "11");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("\"message\":\"3 of 3 failed: a: ") != std::string::npos);
    CHECK(r.body.find("\"partial\"") == std::string::npos);
    CHECK(r.body.find("\"ok\"") == std::string::npos);
    CHECK_EQ(f.a.level, 5);
    // A patch key that doesn't exist is an error, not a missing path.
    r = cases::set(f.api, "/lamps/*", "{\"nope\":1}");
    CHECK_EQ(r.status, Status::NotFound);
    CHECK(r.body.find("\"a\":{\"error\":\"not_found\",\"path\":\"/lamps/a/nope\"") != std::string::npos);
    // No match at all.
    CHECK_EQ(cases::set(f.api, "/lamps/*/nope", "1").body,
             "{\"error\":\"not_found\",\"path\":\"/lamps/*/nope\",\"message\":\"no match\"}");
    CHECK_EQ(cases::set(f.api, "/lamps/count/*", "1").body,
             "{\"error\":\"not_found\",\"path\":\"/lamps/count/*\",\"message\":\"no such node\"}");
    // Only set takes '*'.
    CHECK_EQ(cases::get(f.api, "/lamps/*/on").status, Status::BadRequest);
    CHECK_EQ(cases::get(f.api, "/lamps/*/on", cases::keys("on")).status, Status::BadRequest);
}

TEST(wildcard_containers) {
    Fixture f;
    // The root, and several '*'.
    CHECK_EQ(cases::set(f.api, "/*/a/on", "true").body, "{\"lamps\":true}");
    CHECK_EQ(cases::set(f.api, "/groups/*/*/on", "true").body, "{\"g1\":{\"x\":true},\"g2\":{\"y\":true}}");
    CHECK(f.x && f.y);
    // Keyed lists by key, other lists and arrays by index.
    CHECK_EQ(cases::set(f.api, "/nets/*/dhcp", "false").body, "{\"home\":false,\"work\":false}");
    CHECK(!f.nets[0].dhcp && !f.nets[1].dhcp);
    CHECK_EQ(cases::set(f.api, "/plain/*/dhcp", "false").body, "{\"0\":false,\"1\":false}");
    CHECK(!f.plain[1].dhcp);
    CHECK_EQ(cases::set(f.api, "/arr/*", "7").body, "{\"0\":7,\"1\":7}");
    CHECK_EQ(f.arr[1], 7);
}

TEST(wildcard_deferred_actions) {
    Fixture f;
    // A transport that can't wait: each deferring match fails.
    auto r = cases::set(f.api, "/lamps/*/later", "1");
    CHECK_EQ(r.status, Status::NotAllowed);
    CHECK(r.body.find("\"message\":\"3 of 3 failed: a: transport can't defer replies\"") != std::string::npos);
    CHECK(f.pendings.empty() || !f.pendings[0]);
    f.pendings.clear();
    CHECK_EQ(f.api.pending(), 0);

    // One that can: the reply waits for every match. An admin client also
    // matches the lamp a public one can't read.
    tesser::LocalClient c(f.api);
    client_cases::Log log;
    c.set("/lamps/*/later", "1", log.handler());
    c.process();
    CHECK(log.lines.empty());
    CHECK_EQ(f.pendings.size(), 4u);
    CHECK_EQ(f.api.pending(), 4);
    if (f.pendings.size() == 4) {
        f.pendings[3].reply(3);
        f.pendings[2].reply(2);
        f.pendings[0].reply(0);
        c.process();
        CHECK(log.lines.empty());
        f.pendings[1].fail(Status::Busy, "motor busy");
    }
    c.process();
    CHECK_EQ(log.lines.size(), 1u);
    if (!log.lines.empty()) {
        CHECK_EQ(log.lines[0],
                 "busy {\"error\":\"busy\",\"path\":\"/lamps/*/later\",\"message\":\"1 of 4 failed: b: motor busy\","
                 "\"partial\":true,\"failed\":{\"b\":{\"error\":\"busy\",\"path\":\"/lamps/b/later\","
                 "\"message\":\"motor busy\"}},\"ok\":{\"a\":0,\"c\":2,\"hidden\":3}}");
    }
    CHECK_EQ(f.api.pending(), 0);
}

// A gateway with peers mounted under /peers: two devices, one node without
// their tree, and one that doesn't answer.
struct Peers {
    cases::Device a, b;
    tesser::Api bare;
    int x = 0;
    datagram_cases::Net net;
    tesser::DatagramEndpoint aEnd, bEnd, bareEnd, gatewayEnd;
    tesser::Api gateway;
    tesser::RemoteNode& ghost;
    cases::Lines lines;
    tesser::LineTransport line;

    Peers()
        : aEnd(&a.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn()),
          bEnd(&b.api, tesser::TransportKind::NowTP, net.sender(addr(3)), net.clockFn()),
          bareEnd(&bare, tesser::TransportKind::NowTP, net.sender(addr(4)), net.clockFn()),
          gatewayEnd(&gateway, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn()),
          ghost(gateway.object("peers").remote("ghost", gatewayEnd, addr(9))),
          line(gateway, [this](const char* d, size_t n) { lines.add(d, n); }) {
        bare.value("x", x);
        auto& peers = *static_cast<tesser::Object*>(gateway.child("peers"));
        peers.remote("a", gatewayEnd, addr(2));
        peers.remote("b", gatewayEnd, addr(3));
        peers.remote("bare", gatewayEnd, addr(4));
        ghost.timeout(500);
        net.nodes = {{addr(1), &gatewayEnd}, {addr(2), &aEnd}, {addr(3), &bEnd}, {addr(4), &bareEnd}};
    }

    std::string ask(const std::string& envelope) {
        lines.out.clear();
        std::string l = envelope + "\n";
        line.feed(l.data(), l.size());
        net.run();
        return lines.out.size() == 1 ? lines.out[0] : "<" + std::to_string(lines.out.size()) + " lines>";
    }
};

TEST(wildcard_remote_nodes) {
    Peers p;
    // An offline peer is reported at once; a peer without the path is skipped.
    p.ghost.setOnline(false);
    CHECK_EQ(p.ask("{\"id\":1,\"op\":\"set\",\"path\":\"/peers/*/lamp/brightness\",\"body\":7}"),
             "{\"id\":1,\"status\":\"timeout\",\"body\":{\"error\":\"timeout\",\"path\":\"/peers/*/lamp/brightness\","
             "\"message\":\"1 of 3 failed: ghost: peer offline\",\"partial\":true,"
             "\"failed\":{\"ghost\":{\"error\":\"timeout\",\"path\":\"/peers/ghost\",\"message\":\"peer offline\"}},"
             "\"ok\":{\"a\":7,\"b\":7}}}");
    CHECK_EQ(p.a.lamp.brightness, 7);
    CHECK_EQ(p.b.lamp.brightness, 7);
    CHECK_EQ(p.gateway.pending(), 0);

    // A '*' below a remote node is the remote's to expand.
    CHECK_EQ(p.ask("{\"id\":2,\"op\":\"set\",\"path\":\"/peers/a/*/on\",\"body\":true}"),
             "{\"id\":2,\"status\":\"ok\",\"body\":{\"lamp\":true}}");
    CHECK(p.a.lamp.on);

    // A silent peer that looks online: the reply waits for its timeout.
    p.ghost.setOnline(true);
    CHECK_EQ(p.ask("{\"id\":3,\"op\":\"set\",\"path\":\"/peers/*/lamp/brightness\",\"body\":8}"), "<0 lines>");
    CHECK_EQ(p.a.lamp.brightness, 8);
    p.net.clock += 600;
    p.gatewayEnd.process();
    CHECK_EQ(p.lines.out.size(), 1u);
    if (!p.lines.out.empty()) {
        CHECK(p.lines.out[0].find("\"message\":\"1 of 3 failed: ghost: remote node didn't answer\"") !=
              std::string::npos);
        CHECK(p.lines.out[0].find("\"ok\":{\"a\":8,\"b\":8}") != std::string::npos);
    }
    CHECK_EQ(p.gateway.pending(), 0);

    // Each waiting match takes a pending slot.
    p.ghost.setOnline(false);
    p.gateway.config().maxPending = 1;
    std::string busy = p.ask("{\"id\":4,\"op\":\"set\",\"path\":\"/peers/*/lamp/brightness\",\"body\":9}");
    // The first failure in tree order sets the status (ghost was mounted first).
    CHECK(busy.find("\"status\":\"timeout\"") != std::string::npos);
    CHECK(busy.find("\"message\":\"3 of 4 failed: ghost: peer offline\"") != std::string::npos);
    CHECK(busy.find("\"b\":{\"error\":\"busy\",\"path\":\"/peers/b/lamp/brightness\","
                    "\"message\":\"too many pending calls\"}") != std::string::npos);
    CHECK(busy.find("\"ok\":{\"a\":9}") != std::string::npos);
    CHECK_EQ(p.gateway.pending(), 0);
    p.gateway.config().maxPending = 4;

    // A transport that can't wait gets not_allowed for each remote match.
    p.ghost.setOnline(true);
    auto r = cases::set(p.gateway, "/peers/*/lamp/brightness", "10");
    CHECK_EQ(r.status, Status::NotAllowed);
    CHECK(r.body.find("\"message\":\"4 of 4 failed: ghost: this transport can't wait for remote nodes\"") !=
          std::string::npos);
    CHECK_EQ(p.gateway.pending(), 0);
    CHECK_EQ(p.a.lamp.brightness, 9);
}

}  // namespace wildcard_cases

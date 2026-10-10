// view=hash and automatic re-advertisement (docs/DESIGN.md sections 7 and
// 10.3).
#pragma once

#include <stdio.h>

#include <string>

#include "access_cases.h"
#include "remote_cases.h"

namespace housekeeping_cases {

using tesser::Access;
using tesser::Api;
using tesser::Query;
using tesser::Status;
using tesser::View;

inline Query hash() {
    Query q;
    q.view = View::Hash;
    return q;
}

inline std::string hex(uint32_t h) {
    char buf[16];
    snprintf(buf, sizeof(buf), "\"%08lx\"", static_cast<unsigned long>(h));
    return buf;
}

TEST(hash_view) {
    cases::Device d;
    // The root's hash is schemaHash(); a subtree's is that of its own schema.
    CHECK_EQ(cases::get(d.api, "/", hash()).body, hex(d.api.schemaHash()));
    std::string lamp = cases::get(d.api, "/lamp", hash()).body;
    tesser::HashSink sink;
    sink.write(cases::get(d.api, "/lamp", cases::schema()).body.data(), cases::get(d.api, "/lamp", cases::schema()).body.size());
    CHECK_EQ(lamp, hex(sink.hash));
    CHECK_EQ(cases::get(d.api, "/lamp/on", hash()).status, Status::Ok);
    // depth doesn't apply: a hash always covers the whole schema.
    Query shallow = hash();
    shallow.depth = 0;
    CHECK_EQ(cases::get(d.api, "/lamp", shallow).body, lamp);
    // It covers the whole schema, so no selection.
    Query k = hash();
    k.keys = "on";
    CHECK_EQ(cases::get(d.api, "/lamp", k).status, Status::BadRequest);
    CHECK_EQ(cases::get(d.api, "/lamp", hash(), "{\"on\":null}").status, Status::BadRequest);
    // A schema change changes it.
    int extra = 0;
    d.api.object("lamp").value("extra", extra);
    CHECK(cases::get(d.api, "/lamp", hash()).body != lamp);
    // Over the envelope.
    CHECK_EQ(cases::roundTrip(d.api, "{\"id\":1,\"op\":\"get\",\"path\":\"/\",\"view\":\"hash\"}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":" + hex(d.api.schemaHash()) + "}");
    // Not a value: subscriptions and writes refuse it.
    CHECK_EQ(cases::request(d.api, tesser::Op::Set, "/lamp/on", "true", hash()).status, Status::BadRequest);
}

TEST(hash_view_follows_access) {
    // Clients see the schema their access level allows, so its hash too.
    access_cases::Device d;
    std::string pub = access_cases::get(d.api, Access::Public, "/", hash()).body;
    std::string admin = access_cases::get(d.api, Access::Admin, "/", hash()).body;
    CHECK(pub != admin);
    CHECK_EQ(admin, hex(d.api.schemaHash()));
}

TEST(hash_view_forwarded) {
    remote_cases::Rig r;
    std::string got = r.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/kitchen\",\"view\":\"hash\"}");
    CHECK_EQ(got, "{\"id\":1,\"status\":\"ok\",\"body\":" + hex(r.device.api.schemaHash()) + "}");
}

TEST(schema_revision) {
    uint32_t r0 = tesser::schemaRevision();
    Api api;
    std::vector<int> items(3);
    api.list("items", items, [](tesser::Object& o, int& v) { o.value("v", v).range(0, 9); });
    uint32_t r1 = tesser::schemaRevision();
    CHECK(r1 != r0);
    // Reading list elements and schemas builds temporary objects: no change.
    cases::get(api, "/");
    cases::get(api, "/items/1/v");
    cases::get(api, "/", cases::schema());
    cases::set(api, "/items", "[{\"v\":1}]");
    CHECK_EQ(tesser::schemaRevision(), r1);
    // Values changing isn't a schema change either.
    int x = 0;
    auto& v = api.value("x", x);
    uint32_t r2 = tesser::schemaRevision();
    CHECK(r2 != r1);
    cases::set(api, "/x", "4");
    CHECK_EQ(tesser::schemaRevision(), r2);
    // Modifiers are.
    v.range(0, 10);
    CHECK(tesser::schemaRevision() != r2);
}

TEST(auto_advertise) {
    remote_cases::Rig r;
    std::vector<uint32_t> published;
    uint32_t initial = r.gateway.schemaHash();
    r.gatewayEnd.autoAdvertise([&](uint32_t h) { published.push_back(h); }, initial, 1000);
    r.net.clock = 5000;
    r.gatewayEnd.process();
    CHECK(published.empty());  // nothing changed

    // Mounting peers changes the gateway's schema: one advertisement once
    // the burst is over.
    auto& peers = r.gateway.object("peers");
    r.gatewayEnd.mountPeers(peers);
    r.gatewayEnd.peerSeen(datagram_cases::addr(5), "lamp", 0x11);
    r.gatewayEnd.process();
    r.net.clock = 5500;
    r.gatewayEnd.peerSeen(datagram_cases::addr(6), "fan", 0x22);
    r.gatewayEnd.process();
    r.net.clock = 6000;
    r.gatewayEnd.process();
    CHECK(published.empty());  // still within 1 s of the last change
    r.net.clock = 6600;
    r.gatewayEnd.process();
    CHECK_EQ(published.size(), 1u);
    if (!published.empty()) CHECK_EQ(published[0], r.gateway.schemaHash());
    CHECK(published.empty() || published[0] != initial);

    // Requests and value changes don't re-advertise.
    r.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/\"}");
    r.ask("{\"id\":2,\"op\":\"set\",\"path\":\"/local\",\"body\":5}");
    r.net.clock = 9000;
    r.gatewayEnd.process();
    r.net.clock = 11000;
    r.gatewayEnd.process();
    CHECK_EQ(published.size(), 1u);

    // A peer's state (online, its own advertised hash) shows in the schema
    // but not in its hash: no re-advertisement. Otherwise two gateways
    // mounting each other would re-advertise forever.
    uint32_t before = r.gateway.schemaHash();
    r.gatewayEnd.forgetPeer(datagram_cases::addr(5));
    r.gatewayEnd.process();
    r.gatewayEnd.peerSeen(datagram_cases::addr(6), "fan", 0x33);
    r.gatewayEnd.process();
    CHECK(cases::get(r.gateway, "/peers", cases::schema()).body.find("\"online\":false") != std::string::npos);
    r.net.clock = 12500;
    r.gatewayEnd.process();
    r.net.clock = 14000;
    r.gatewayEnd.process();
    CHECK_EQ(published.size(), 1u);
    CHECK_EQ(r.gateway.schemaHash(), before);
    CHECK_EQ(cases::get(r.gateway, "/", hash()).body, hex(before));

    // A new node is a change.
    int extra = 0;
    r.gateway.value("extra", extra);
    r.gatewayEnd.process();
    r.net.clock = 15500;
    r.gatewayEnd.process();
    CHECK_EQ(published.size(), 2u);
}

}  // namespace housekeeping_cases

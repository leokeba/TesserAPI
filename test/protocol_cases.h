// Protocol version, features, strict requests and error reasons
// (docs/DESIGN.md section 10.1.2).
#pragma once

#include <string>

#include "core_cases.h"
#include "remote_cases.h"
#include "subscription_cases.h"

namespace protocol_cases {

using tesser::Op;
using tesser::Query;
using tesser::Status;

TEST(protocol_info_view) {
    cases::Device d;
    std::string hash = cases::roundTrip(d.api, "{\"id\":1,\"op\":\"get\",\"path\":\"/\",\"view\":\"hash\"}");
    std::string info = cases::roundTrip(d.api, "{\"id\":2,\"op\":\"get\",\"path\":\"/\",\"view\":\"info\"}");
    CHECK(info.find("\"body\":{\"protocol\":2,\"features\":[\"ceiling\",\"reasons\",\"strict\",\"info\"") !=
          std::string::npos);
    // The hash is view=hash's.
    size_t at = hash.find("\"body\":\"");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) CHECK(info.find("\"hash\":\"" + hash.substr(at + 8, 8) + "\"") != std::string::npos);
    const uint8_t mac[6] = {0x10, 0x52, 0x1c, 0x75, 0xf3, 0x7c};
    d.api.setDeviceId(mac);
    CHECK(cases::roundTrip(d.api, "{\"op\":\"get\",\"path\":\"/\",\"view\":\"info\"}").find("\"id\":\"10521c75f37c\"") !=
          std::string::npos);
    CHECK(cases::roundTrip(d.api, "{\"op\":\"get\",\"path\":\"/\",\"view\":\"info\",\"keys\":\"lamp\"}")
              .find("\"status\":\"bad_request\"") != std::string::npos);

    // Through a gateway: the remote node's own.
    remote_cases::Rig r;
    CHECK(r.ask("{\"id\":3,\"op\":\"get\",\"path\":\"/kitchen\",\"view\":\"info\"}")
              .find("\"status\":\"ok\",\"body\":{\"protocol\":2,") != std::string::npos);
}

TEST(protocol_strict_requests) {
    cases::Device d;
    // Without "v", unknown members are ignored, as they always were.
    CHECK(cases::roundTrip(d.api, "{\"id\":1,\"op\":\"get\",\"path\":\"/lamp/on\",\"later\":1}")
              .find("\"status\":\"ok\"") != std::string::npos);
    CHECK(cases::roundTrip(d.api, "{\"id\":2,\"op\":\"get\",\"path\":\"/lamp/on\",\"later\":1,\"v\":1}")
              .find("\"status\":\"ok\"") != std::string::npos);
    // With "v" of 2 or more, they are refused.
    CHECK_EQ(cases::roundTrip(d.api, "{\"id\":3,\"op\":\"get\",\"path\":\"/lamp/on\",\"later\":1,\"v\":2}"),
             "{\"id\":3,\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"reason\":\"unknown_option\","
             "\"path\":\"/\",\"message\":\"an option this node doesn't know (the request is strict: \\\"v\\\")\"}}");
    CHECK(cases::roundTrip(d.api, "{\"id\":4,\"op\":\"sub\",\"path\":\"/lamp\",\"snapshot\":false,\"v\":3}")
              .find("\"status\":\"ok\"") != std::string::npos);
    CHECK(cases::roundTrip(d.api, "{\"id\":5,\"op\":\"get\",\"path\":\"/\",\"v\":0}").find("\"bad_request\"") !=
          std::string::npos);
    // File and auth envelopes too.
    CHECK(cases::roundTrip(d.api, "{\"id\":6,\"op\":\"download\",\"path\":\"/lamp\",\"chunk\":9,\"v\":2}")
              .find("\"reason\":\"unknown_option\"") != std::string::npos);
    CHECK(cases::roundTrip(d.api, "{\"id\":7,\"op\":\"auth\",\"token\":\"x\",\"scope\":1,\"v\":2}")
              .find("\"reason\":\"unknown_option\"") != std::string::npos);

    // What the library sends with options is strict; plain requests aren't
    // marked, so they cost nothing more.
    Query q;
    q.depth = 1;
    CHECK(tesser::buildRequestEnvelope(1, Op::Get, "/", q, "").find(",\"v\":2") != std::string::npos);
    CHECK_EQ(tesser::buildRequestEnvelope(1, Op::Set, "/a", Query(), "1"), "{\"id\":1,\"op\":\"set\",\"path\":\"/a\",\"body\":1}");
}

TEST(protocol_error_reasons) {
    tesser::Reason r = tesser::Reason::None;
    CHECK(tesser::parseReason("low_memory", r));
    CHECK(r == tesser::Reason::LowMemory);
    CHECK_EQ(std::string(tesser::toString(tesser::Reason::SubscriptionLimit)), "subscription_limit");
    CHECK(!tesser::parseReason("", r));
    CHECK(!tesser::parseReason("newer_reason", r));

    // A busy that passes and one that doesn't are told apart by reason.
    cases::Device d;
    subscription_cases::FakeSubscriber s;
    const char* paths[] = {"/lamp", "/config", "/sensors", "/lamp/on"};
    for (const char* p : paths) CHECK_EQ(subscription_cases::subscribe(d.api, &s, p).status, Status::Ok);
    cases::Resp full = subscription_cases::subscribe(d.api, &s, "/config/level");
    CHECK_EQ(full.status, Status::Busy);
    CHECK(full.body.find("\"reason\":\"subscription_limit\"") != std::string::npos);

    // Relayed by a gateway as the remote wrote it; its own have one too.
    remote_cases::Rig rig;
    rig.net.dropAll = true;
    rig.kitchen.timeout(100);
    rig.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/kitchen/lamp\"}");
    rig.net.clock += 200;
    rig.gatewayEnd.process();
    CHECK_EQ(rig.lines.out.size(), 1u);
    if (!rig.lines.out.empty()) CHECK(rig.lines.out[0].find("\"reason\":\"no_answer\"") != std::string::npos);
    rig.kitchen.setOnline(false);
    CHECK(rig.ask("{\"id\":2,\"op\":\"get\",\"path\":\"/kitchen/lamp\"}").find("\"reason\":\"peer_offline\"") !=
          std::string::npos);
}

TEST(protocol_advertised) {
    uint8_t port = 0, v = 0;
    uint32_t hash = 0;
    CHECK(tesser::parseAdvertisement("{\"tesser\":84,\"schema\":\"0000feed\",\"v\":2}", port, hash, v));
    CHECK_EQ(v, 2);
    CHECK(tesser::parseAdvertisement("{\"tesser\":84,\"schema\":\"0000feed\"}", port, hash, v));
    CHECK_EQ(v, 1);  // before versions
    // A peer's stub shows the version it advertised.
    remote_cases::Rig r;
    tesser::Object& peers = r.gateway.object("peers");
    r.gatewayEnd.mountPeers(peers);
    r.gatewayEnd.peerSeen(datagram_cases::addr(2), "lamp", 0xfeed, tesser::PeerAddress(), 1);
    r.net.run();
    Query q;
    q.view = tesser::View::Schema;
    q.depth = 1;
    CHECK(cases::get(r.gateway, "/peers", q).body.find("\"protocol\":1") != std::string::npos);
}

}  // namespace protocol_cases

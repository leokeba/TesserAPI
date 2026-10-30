// Protocol version, features, strict requests and error reasons
// (docs/DESIGN.md section 10.1.2).
#pragma once

#include <string>

#include "core_cases.h"
#include "remote_cases.h"
#include "tesser/http_checks.h"
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
    CHECK(info.find("\"limits\"") != std::string::npos);
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

TEST(protocol_forwarded_options_are_strict) {
    // A gateway forwards a client's options marked "v": 2, so a peer that
    // doesn't know one refuses it rather than answer something else; a
    // plain get isn't marked.
    remote_cases::Rig r;
    auto sent = [&](const std::string& envelope) {
        r.lines.out.clear();
        std::string l = envelope + "\n";
        r.line.feed(l.data(), l.size());
        std::string out = r.net.frames.empty() ? std::string() : r.net.frames.front().text;
        r.net.run();
        return out;
    };
    CHECK(sent("{\"id\":1,\"op\":\"get\",\"path\":\"/kitchen\",\"keys\":\"lamp\"}").find("\"v\":2") !=
          std::string::npos);
    CHECK(sent("{\"id\":2,\"op\":\"get\",\"path\":\"/kitchen/lamp\"}").find("\"v\":") == std::string::npos);
}

TEST(protocol_advertised) {
    uint8_t port = 0, v = 0;
    uint32_t hash = 0;
    CHECK(tesser::parseAdvertisement("{\"tesser\":84,\"schema\":\"0000feed\",\"v\":2}", port, hash, v));
    CHECK_EQ(v, 2);
    CHECK(tesser::parseAdvertisement("{\"tesser\":84,\"schema\":\"0000feed\"}", port, hash, v));
    CHECK_EQ(v, 1);  // before versions
    // The largest message the node takes (docs/DESIGN.md section 14.1).
    std::string ad = tesser::buildAdvertisement(84, 0xfeed, 16384);
    CHECK_EQ(ad, "{\"tesser\":84,\"schema\":\"0000feed\",\"v\":2,\"max\":16384}");
    size_t max = 0;
    CHECK(tesser::parseAdvertisement(ad, port, hash, v, max));
    CHECK_EQ(max, 16384u);
    CHECK(tesser::parseAdvertisement("{\"tesser\":84,\"schema\":\"0000feed\",\"v\":2}", port, hash, v, max));
    CHECK_EQ(max, 0u);  // not said
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

namespace protocol_cases {

TEST(protocol_secret_equals) {
    CHECK(tesser::secretEquals("test-token", "test-token"));
    CHECK(tesser::secretEquals("", ""));
    CHECK(!tesser::secretEquals("test-tokem", "test-token"));
    CHECK(!tesser::secretEquals("test-toke", "test-token"));    // a prefix
    CHECK(!tesser::secretEquals("test-token1", "test-token"));  // longer
    CHECK(!tesser::secretEquals("", "test-token"));
    CHECK(!tesser::secretEquals(std::string_view("a\0", 2), "a"));  // trailing NUL counts
}

// HttpServer's strict mode (docs/DESIGN.md section 10.2): the header checks.
TEST(protocol_envelopes_sorted_out) {
    using tesser::EnvelopeCall;
    using Kind = EnvelopeCall::Kind;
    auto read = [](const char* text, uint8_t takes, EnvelopeCall& call, JsonDocument& doc) {
        tesser::parseEnvelope(std::string_view(text), doc, 8, takes, call);
    };
    constexpr uint8_t all = tesser::kTakesAuth | tesser::kTakesFiles;
    {
        JsonDocument doc;
        EnvelopeCall c;
        read("{\"id\":7,\"op\":\"get\",\"path\":\"/a\"}", all, c, doc);
        CHECK(c.status == Status::Ok && c.kind == Kind::Request);
        CHECK(c.request.path == "/a");
        CHECK_EQ(c.id.length, 1u);
    }
    {
        JsonDocument doc;
        EnvelopeCall c;
        read("{\"id\":7,\"op\":\"auth\",\"token\":\"t\"}", all, c, doc);
        CHECK(c.status == Status::Ok && c.kind == Kind::Auth);
    }
    {
        JsonDocument doc;
        EnvelopeCall c;
        read("{\"op\":\"upload@user\",\"path\":\"/f\"}", all, c, doc);
        CHECK(c.status == Status::Ok && c.kind == Kind::File);
    }
    // What a transport doesn't take is a bad op, with its id.
    for (const char* op : {"auth", "upload", "download"}) {
        JsonDocument doc;
        EnvelopeCall c;
        std::string text = std::string("{\"id\":7,\"op\":\"") + op + "\"}";
        read(text.c_str(), 0, c, doc);
        CHECK(c.status == Status::BadRequest && c.kind == Kind::Request);
        CHECK_EQ(std::string(c.message), "op must be \"get\", \"set\", \"sub\" or \"unsub\"");
        CHECK_EQ(c.id.length, 1u);
    }
    {
        // A strict "auth" with an option it doesn't know.
        JsonDocument doc;
        EnvelopeCall c;
        read("{\"id\":7,\"op\":\"auth\",\"v\":2,\"bogus\":1}", all, c, doc);
        CHECK(c.status == Status::BadRequest && c.kind == Kind::Auth);
        CHECK(c.reason == tesser::Reason::UnknownOption);
    }
    {
        // Not JSON, and an id it can't use.
        JsonDocument doc;
        EnvelopeCall c;
        read("{nope", all, c, doc);
        CHECK(c.status == Status::BadRequest);
        JsonDocument doc2;
        EnvelopeCall d;
        read("{\"id\":[],\"op\":\"auth\"}", all, d, doc2);
        CHECK(d.status == Status::BadRequest && d.kind == Kind::Auth);
    }
}

TEST(protocol_client_token_rechecked) {
    tesser::Api api;
    api.authenticate(tesser::TokenCheck([](std::string_view token, const tesser::Client& client) {
        return token == "ok" && client.transport == tesser::TransportKind::Serial ? tesser::Access::Admin
                                                                                  : tesser::Access::Public;
    }));
    tesser::ClientToken t;
    tesser::Client client;
    client.transport = tesser::TransportKind::Serial;
    tesser::Access level = tesser::Access::User;
    t.token = "ok";
    CHECK(!t.recheck(api, client, level));  // fixed: its level isn't the check's
    CHECK(level == tesser::Access::User);
    t.fixed = false;
    CHECK(t.recheck(api, client, level));
    CHECK(level == tesser::Access::Admin);
    t.token = "no";
    CHECK(t.recheck(api, client, level));
    CHECK(level == tesser::Access::Public);
    t.token = "ok";
    client.transport = tesser::TransportKind::Http;  // the check sees the client
    CHECK(t.recheck(api, client, level));
    CHECK(level == tesser::Access::Public);
}

TEST(protocol_auth_alike_on_lines_and_peers) {
    // One token, one answer: a line and a datagram peer answer "auth" the
    // same way, and a lower level ends what was opened above it.
    auto check = [](const std::function<std::string(const std::string&)>& ask) {
        CHECK_EQ(ask("{\"id\":1,\"op\":\"auth\",\"token\":\"good\"}"), "{\"id\":1,\"status\":\"ok\",\"body\":{\"access\":\"admin\"}}");
        CHECK_EQ(ask("{\"id\":2,\"op\":\"auth\",\"token\":5}"),
                 "{\"id\":2,\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/\","
                 "\"message\":\"token must be a string\"}}");
        CHECK(ask("{\"id\":3,\"op\":\"auth\",\"v\":2,\"bogus\":1}").find("\"reason\":\"unknown_option\"") != std::string::npos);
        CHECK(ask("{\"id\":4,\"op\":\"sub\",\"path\":\"/level\"}").find("\"status\":\"ok\"") != std::string::npos);
        CHECK_EQ(ask("{\"id\":5,\"op\":\"auth\"}"), "{\"id\":5,\"status\":\"ok\",\"body\":{\"access\":\"public\"}}");
    };
    auto tokens = [](std::string_view t, const tesser::Client&) {
        return t == "good" ? tesser::Access::Admin : tesser::Access::Public;
    };
    {
        tesser::Api api;
        int level = 1;
        api.value("level", level);
        api.authenticate(tesser::TokenCheck(tokens));
        std::vector<std::string> out;
        std::string pending;
        tesser::LineTransport line(api, [&](const char* d, size_t n) { pending.append(d, n); });
        line.setAuthenticated(false);
        check([&](const std::string& text) {
            pending.clear();
            line.feed((text + "\n").data(), text.size() + 1);
            return pending.substr(0, pending.find('\n'));
        });
        CHECK_EQ(api.subscriptionCount(), 0u);
    }
    {
        tesser::Api api;
        int level = 1;
        api.value("level", level);
        api.authenticate(tesser::TokenCheck(tokens));
        std::vector<std::string> sent;
        tesser::PeerAddress peer;
        tesser::DatagramEndpoint ep(&api, tesser::TransportKind::Udp,
                                    [&](const tesser::PeerAddress&, std::string m, tesser::Delivery) {
                                        sent.push_back(std::move(m));
                                        return true;
                                    },
                                    [] { return 1u; });
        peer.length = 6;
        check([&](const std::string& text) {
            sent.clear();
            ep.receive(peer, text.data(), text.size(), true);
            ep.process();
            return sent.empty() ? std::string() : sent.front();
        });
        CHECK_EQ(api.subscriptionCount(), 0u);
    }
}

TEST(protocol_http_text) {
    auto decoded = [](std::string text, bool plus) {
        text.resize(tesser::http::urlDecode(&text[0], text.size(), plus));
        return text;
    };
    CHECK_EQ(decoded("a%20b%2fc%2Fd", false), "a b/c/d");
    CHECK_EQ(decoded("a+b%2Bc", true), "a b+c");
    CHECK_EQ(decoded("a+b", false), "a+b");  // a path's '+' is a '+'
    CHECK_EQ(decoded("100%", false), "100%");  // not an escape: left alone
    CHECK_EQ(decoded("%4", false), "%4");
    CHECK_EQ(decoded("%zz", false), "%zz");
    CHECK_EQ(decoded("", true), "");

    using tesser::http::contentDisposition;
    CHECK_EQ(contentDisposition("cal.json"), "attachment; filename=\"cal.json\"");
    CHECK_EQ(contentDisposition("a\"b\\c\n.txt"), "attachment; filename=\"a_b_c_.txt\"");
    CHECK_EQ(contentDisposition("caf\xc3\xa9 1.txt"),
             "attachment; filename=\"caf__ 1.txt\"; filename*=UTF-8''caf%C3%A9%201.txt");
}

TEST(protocol_http_strict_checks) {
    using namespace tesser::http;
    CHECK(hostName("dev.local:8080") == "dev.local");
    CHECK(hostName("dev.local") == "dev.local");
    CHECK(hostName("192.168.4.1:80") == "192.168.4.1");
    CHECK(hostName("[fe80::1]:80") == "[fe80::1]");
    CHECK(hostName("[fe80::1]") == "[fe80::1]");

    // The device's own page.
    CHECK(sameOrigin("http://192.168.1.59", "192.168.1.59"));
    CHECK(sameOrigin("http://Dev.Local", "dev.local:80"));
    CHECK(sameOrigin("http://dev.local:80", "dev.local"));
    CHECK(sameOrigin("http://dev.local:8080", "dev.local:8080"));
    CHECK(sameOrigin("https://dev.local", "dev.local:443"));
    CHECK(!sameOrigin("http://dev.local:8080", "dev.local"));
    CHECK(!sameOrigin("http://evil.example", "dev.local"));
    CHECK(!sameOrigin("http://dev.local.evil.example", "dev.local"));
    CHECK(!sameOrigin("null", "dev.local"));
    CHECK(!sameOrigin("file://", "dev.local"));
    CHECK(!sameOrigin("http://dev.local", ""));
    CHECK(sameOrigin("http://[fe80::1]", "[fe80::1]:80"));

    // The CORS origin, any with "*", or the device's own page.
    const char* launcher = "https://leokeba.github.io";
    CHECK(originAllowed("https://leokeba.github.io", launcher, "192.168.1.59"));
    CHECK(originAllowed("http://192.168.1.59", launcher, "192.168.1.59"));
    CHECK(!originAllowed("https://leokeba.github.io.evil.example", launcher, "192.168.1.59"));
    CHECK(!originAllowed("http://evil.example", launcher, "192.168.1.59"));
    CHECK(!originAllowed("null", launcher, "192.168.1.59"));
    CHECK(!originAllowed("http://evil.example", nullptr, "192.168.1.59"));
    CHECK(originAllowed("http://evil.example", "*", "192.168.1.59"));
    CHECK(originAllowed("null", "*", ""));

    CHECK(isJsonType("application/json"));
    CHECK(isJsonType("Application/JSON; charset=utf-8"));
    CHECK(isJsonType(" application/json ;charset=utf-8"));
    CHECK(!isJsonType("text/plain"));
    CHECK(!isJsonType("application/x-www-form-urlencoded"));
    CHECK(!isJsonType("multipart/form-data; boundary=x"));
    CHECK(!isJsonType("application/jsonx"));
    CHECK(!isJsonType(""));
}

// An allocator with no memory: the document's first allocation fails.
struct NoMemory : ArduinoJson::Allocator {
    void* allocate(size_t) override { return nullptr; }
    void deallocate(void*) override {}
    void* reallocate(void*, size_t) override { return nullptr; }
};

TEST(envelope_no_memory_is_busy) {
    // A request the heap can't parse is busy (low_memory), under its id so
    // the client sends it again, not bad_request "malformed JSON".
    NoMemory none;
    JsonDocument doc(&none);
    tesser::Request req;
    tesser::EnvelopeId id;
    const char* message = nullptr;
    tesser::Reason reason = tesser::Reason::None;
    std::string text = "{\"id\":42,\"op\":\"get\",\"path\":\"/lamp\"}";
    CHECK_EQ(tesser::parseEnvelope(text, doc, 8, req, id, message, &reason), Status::Busy);
    CHECK(reason == tesser::Reason::LowMemory);
    CHECK_EQ(std::string(id.json, id.length), "42");
}

}  // namespace protocol_cases

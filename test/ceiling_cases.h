// A gateway's clients reach remote nodes at their own level, never the
// gateway's (docs/DESIGN.md section 13.1): forwarded requests, file relays,
// subscriptions, mirrors and events carry the client's level as a ceiling.
#pragma once

#include <string>
#include <vector>

#include "access_cases.h"
#include "core_cases.h"
#include "datagram_cases.h"
#include "file_envelope_cases.h"

namespace ceiling_cases {

using cases::Resp;
using datagram_cases::addr;
using tesser::Access;
using tesser::DatagramEndpoint;
using tesser::Op;
using tesser::Query;
using tesser::Status;

// access_cases' device (net written by admins, users read by admins), with
// a value, an event and a file only admins read, and an event anyone reads.
struct Device : access_cases::Device {
    int logins = 0;
    std::string backup = "secrets";
    tesser::EventNode& pressed;

    Device() : pressed(api.event("pressed")) {
        auto& users = *static_cast<tesser::Object*>(api.child("users"));
        users.value("logins", logins);
        users.file("backup").text([this] { return backup; },
                                  [this](std::string_view text, tesser::JsonWriter&) {
                                      backup = std::string(text);
                                      return tesser::Check::ok();
                                  });
    }
};

// A device at addr(2) that trusts the gateway at addr(1) as an admin peer,
// mounted as /dev; the gateway's own tree declares no levels. Declaration
// order matters: endpoints must outlive the trees holding remote nodes.
struct Rig {
    Device device;
    datagram_cases::Net net;
    DatagramEndpoint deviceEnd;
    DatagramEndpoint gatewayEnd;
    tesser::Api gateway;
    tesser::RemoteNode& dev;

    explicit Rig(DatagramEndpoint::SendFn gatewaySend = nullptr)
        : deviceEnd(&device.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn()),
          gatewayEnd(&gateway, tesser::TransportKind::NowTP, gatewaySend ? gatewaySend : net.sender(addr(1)),
                     net.clockFn()),
          dev(gateway.remote("dev", gatewayEnd, addr(2))) {
        deviceEnd.trust([](const tesser::PeerAddress& p) { return p == addr(1); });
        net.nodes = {{addr(1), &gatewayEnd}, {addr(2), &deviceEnd}};
    }

    void settle(uint32_t now = 1000) {
        for (int i = 0; i < 3; i++) {
            device.api.poll(now);
            net.run();
            gateway.poll(now);
        }
    }
};

// A client of the gateway at one level, collecting its notifications.
struct Client {
    tesser::LocalClient c;
    std::vector<std::string> notes;

    Client(tesser::Api& api, Access level) : c(api) {
        c.setAccess(level);
        c.onNotification([this](JsonObjectConst env) {
            std::string s;
            serializeJson(env, s);
            notes.push_back(s);
        });
    }

    Resp call(Rig& r, Op op, const char* path, const char* body = "", Query q = Query()) {
        Resp out{Status::Internal, "<no reply>"};
        c.request(op, path, body, [&out](Status s, JsonVariantConst b) {
            out.status = s;
            out.body.clear();
            serializeJson(b, out.body);
        }, q);
        r.net.run();
        c.process();
        return out;
    }
    std::string last() const { return notes.empty() ? std::string() : notes.back(); }
    void drain(Rig& r) {
        r.settle();
        c.process();
    }
};

TEST(ceiling_on_the_wire) {
    Query q;
    q.ceiling = Access::User;
    CHECK_EQ(tesser::buildRequestEnvelope(4, Op::Set, "/net/port", q, "81"),
             "{\"id\":4,\"op\":\"set@user\",\"path\":\"/net/port\",\"v\":2,\"body\":81}");
    q.ceiling = Access::Admin;  // admin: no ceiling, as before
    CHECK_EQ(tesser::buildRequestEnvelope(4, Op::Get, "/", q, ""), "{\"id\":4,\"op\":\"get\",\"path\":\"/\"}");

    // On any node: the level is the lower of the client's and the op's.
    Device d;
    CHECK_EQ(cases::roundTrip(d.api, "{\"id\":1,\"op\":\"get@public\",\"path\":\"/users/logins\"}"),
             "{\"id\":1,\"status\":\"unauthorized\",\"body\":{\"error\":\"unauthorized\",\"reason\":\"not_authorized\",\"path\":\"/users/logins\","
             "\"message\":\"not authorized\"}}");
    CHECK(cases::roundTrip(d.api, "{\"id\":2,\"op\":\"set@user\",\"path\":\"/net/port\",\"body\":81}")
              .find("\"status\":\"unauthorized\"") != std::string::npos);
    CHECK_EQ(d.port, 80);
    CHECK(cases::roundTrip(d.api, "{\"id\":3,\"op\":\"set@user\",\"path\":\"/brightness\",\"body\":4}")
              .find("\"status\":\"ok\"") != std::string::npos);
    CHECK(cases::roundTrip(d.api, "{\"id\":4,\"op\":\"get@admin\",\"path\":\"/users/logins\"}")
              .find("\"status\":\"ok\"") != std::string::npos);
    CHECK(cases::roundTrip(d.api, "{\"id\":5,\"op\":\"get@root\",\"path\":\"/\"}").find("\"status\":\"bad_request\"") !=
          std::string::npos);
    // A ceiling never raises a level: a public client stays public.
    access_cases::Device open;
    Query admin;
    admin.ceiling = Access::Admin;
    CHECK_EQ(access_cases::request(open.api, Access::Public, Op::Get, "/users", nullptr, admin).status,
             Status::Unauthorized);
    // Asked of the API directly (a LocalClient), it applies as well.
    Query user;
    user.ceiling = Access::User;
    CHECK_EQ(access_cases::request(open.api, Access::Admin, Op::Get, "/users", nullptr, user).status,
             Status::Unauthorized);

    // A file envelope carries one too.
    CHECK(cases::roundTrip(d.api, "{\"id\":6,\"op\":\"download@user\",\"path\":\"/users/backup\"}")
              .find("\"status\":\"unauthorized\"") != std::string::npos);
    CHECK(cases::roundTrip(d.api, "{\"id\":7,\"op\":\"download\",\"path\":\"/users/backup\"}")
              .find("\"status\":\"ok\"") != std::string::npos);
}

TEST(ceiling_forwarded_requests) {
    Rig r;
    Client anon(r.gateway, Access::Public), user(r.gateway, Access::User), admin(r.gateway, Access::Admin);
    // The device trusts the gateway as an admin, but serves each client of
    // it at the client's level.
    CHECK_EQ(anon.call(r, Op::Get, "/dev").body, "{\"brightness\":10,\"net\":{\"port\":80,\"password\":null}}");
    CHECK_EQ(anon.call(r, Op::Get, "/dev/users/logins").status, Status::Unauthorized);
    Query schemaView;
    schemaView.view = tesser::View::Schema;
    Resp schema = anon.call(r, Op::Get, "/dev", "", schemaView);
    CHECK(schema.body.find("users") == std::string::npos);
    CHECK_EQ(anon.call(r, Op::Set, "/dev/net/port", "81").status, Status::Unauthorized);
    CHECK_EQ(user.call(r, Op::Set, "/dev/net/reboot", "null").status, Status::Unauthorized);
    CHECK(!r.device.rebooted);
    CHECK_EQ(r.device.port, 80);
    CHECK_EQ(user.call(r, Op::Set, "/dev/brightness", "5").status, Status::Ok);
    CHECK_EQ(r.device.brightness, 5);
    // Admins are unchanged.
    CHECK_EQ(admin.call(r, Op::Get, "/dev/users/logins").body, "0");
    CHECK_EQ(admin.call(r, Op::Set, "/dev/net/port", "81").status, Status::Ok);
    CHECK_EQ(r.device.port, 81);
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(ceiling_fan_out) {
    // '*' forwards each match with the client's level.
    Rig r;
    tesser::Object& peers = r.gateway.object("peers");
    peers.remote("a", r.gatewayEnd, addr(2));
    peers.remote("b", r.gatewayEnd, addr(2));
    Client anon(r.gateway, Access::Public), admin(r.gateway, Access::Admin);
    CHECK(anon.call(r, Op::Set, "/peers/*/net/port", "82").status != Status::Ok);
    CHECK_EQ(r.device.port, 80);
    CHECK_EQ(admin.call(r, Op::Set, "/peers/*/net/port", "82").status, Status::Ok);
    CHECK_EQ(r.device.port, 82);
}

TEST(ceiling_old_nodes_fail_closed) {
    // A node that predates ceilings would ignore a member; so the level
    // goes in the op, which it refuses. Simulated: the frames the gateway
    // sends with a ceiling are answered as such a node answers.
    Rig* rig = nullptr;
    Rig r([&rig](const tesser::PeerAddress& to, const std::string& m, tesser::Delivery d) {
        JsonDocument doc;
        deserializeJson(doc, m);
        std::string op = doc["op"] | "";
        if (op.find('@') != std::string::npos) {
            std::string reply = "{\"id\":" + std::to_string(doc["id"].as<uint32_t>()) +
                                ",\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/\","
                                "\"message\":\"op must be \\\"get\\\", \\\"set\\\", \\\"sub\\\" or \\\"unsub\\\"\"}}";
            rig->net.frames.push_back(datagram_cases::Net::Frame{to, addr(1), reply, tesser::Delivery::Reliable});
            return true;
        }
        rig->net.frames.push_back(datagram_cases::Net::Frame{addr(1), to, m, d});
        return true;
    });
    rig = &r;
    Client anon(r.gateway, Access::Public), admin(r.gateway, Access::Admin);
    CHECK_EQ(anon.call(r, Op::Get, "/dev/brightness").body,
             "{\"error\":\"unauthorized\",\"reason\":\"ceiling_unsupported\",\"path\":\"/dev\",\"message\":\"the remote node can't limit a client's "
             "access: only admins reach it through a gateway\"}");
    CHECK_EQ(anon.call(r, Op::Subscribe, "/dev").status, Status::Unauthorized);
    CHECK_EQ(r.device.api.subscriptionCount(), 0u);
    CHECK_EQ(admin.call(r, Op::Get, "/dev/brightness").body, "10");
}

TEST(ceiling_subscriptions) {
    Rig r;
    Client anon(r.gateway, Access::Public), admin(r.gateway, Access::Admin);
    // One upstream subscription per level, each filtered by the device.
    CHECK_EQ(anon.call(r, Op::Subscribe, "/dev").body, "{\"brightness\":10,\"net\":{\"port\":80,\"password\":null}}");
    CHECK(admin.call(r, Op::Subscribe, "/dev").body.find("\"users\":{\"admin\":null,\"logins\":0}") !=
          std::string::npos);
    CHECK_EQ(r.device.api.subscriptionCount(), 2u);
    CHECK_EQ(r.dev.upstreamCount(), 2u);
    std::string keys, exclude;
    int depth = 0;
    CHECK(r.dev.upstreamSpec("/dev", keys, exclude, depth, Access::Public));
    CHECK(r.dev.upstreamSpec("/dev", keys, exclude, depth, Access::Admin));
    CHECK(!r.dev.upstreamSpec("/dev", keys, exclude, depth, Access::User));

    // Changes: each client hears what it may read, once.
    r.device.logins = 3;
    r.device.api.changed("/users/logins");
    cases::set(r.device.api, "/brightness", "6");
    anon.drain(r);
    admin.c.process();
    CHECK_EQ(anon.notes.size(), 1u);
    CHECK_EQ(anon.last(), "{\"op\":\"change\",\"path\":\"/dev\",\"body\":{\"brightness\":6}}");
    CHECK_EQ(admin.notes.size(), 1u);
    CHECK_EQ(admin.last(), "{\"op\":\"change\",\"path\":\"/dev\",\"body\":{\"brightness\":6,\"users\":{\"logins\":3}}}");

    // Events likewise.
    r.device.login->emit(1);
    r.device.pressed.emit(2);
    anon.drain(r);
    admin.c.process();
    CHECK_EQ(anon.notes.size(), 2u);
    CHECK_EQ(anon.last(), "{\"op\":\"event\",\"path\":\"/dev/pressed\",\"body\":2}");
    CHECK_EQ(admin.notes.size(), 3u);
    if (admin.notes.size() == 3) {
        CHECK_EQ(admin.notes[1], "{\"op\":\"event\",\"path\":\"/dev/users/login\",\"body\":1}");
        CHECK_EQ(admin.notes[2], "{\"op\":\"event\",\"path\":\"/dev/pressed\",\"body\":2}");
    }

    // Unsubscribing one level leaves the other's upstream subscription.
    CHECK_EQ(anon.call(r, Op::Unsubscribe, "/dev").body, "1");
    r.settle();
    CHECK_EQ(r.dev.upstreamCount(), 1u);
    CHECK_EQ(r.device.api.subscriptionCount(), 1u);
    cases::set(r.device.api, "/brightness", "7");
    admin.drain(r);
    CHECK_EQ(admin.last(), "{\"op\":\"change\",\"path\":\"/dev\",\"body\":{\"brightness\":7}}");
}

TEST(ceiling_subscription_envelopes) {
    // On the device: a capped subscription's notifications say "as", and
    // only an unsub at its level ends it.
    access_cases::Device d;
    subscription_cases::FakeSubscriber s;
    Query user;
    user.ceiling = Access::User;
    CHECK_EQ(subscription_cases::subscribe(d.api, &s, "/", user).body,
             "{\"brightness\":10,\"net\":{\"port\":80,\"password\":null}}");
    CHECK_EQ(subscription_cases::subscribe(d.api, &s, "/").status, Status::Ok);  // the same client, uncapped
    cases::set(d.api, "/brightness", "3");
    d.api.poll(1000);
    CHECK_EQ(s.messages.size(), 2u);
    if (s.messages.size() == 2) {
        CHECK_EQ(s.messages[0], "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"brightness\":3},\"as\":\"user\"}");
        CHECK_EQ(s.messages[1], "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"brightness\":3}}");
    }
    d.login->emit(5);  // only the admin subscription reads it
    CHECK_EQ(s.last(), "{\"op\":\"event\",\"path\":\"/users/login\",\"body\":5}");
    CHECK_EQ(s.messages.size(), 3u);
    CHECK_EQ(subscription_cases::request(d.api, &s, Op::Unsubscribe, "/", user).body, "1");
    CHECK_EQ(d.api.subscriptionCount(), 1u);
    CHECK_EQ(subscription_cases::request(d.api, &s, Op::Unsubscribe, "/", user).body, "0");
    CHECK_EQ(subscription_cases::request(d.api, &s, Op::Unsubscribe, "/").body, "1");
}

TEST(ceiling_mirrors) {
    {
        // At the default level, public: the copy is what anyone may read.
        Rig r;
        r.dev.mirror(0, 30000);
        r.settle();
        CHECK(r.dev.hasCopy());
        Client anon(r.gateway, Access::Public), admin(r.gateway, Access::Admin);
        CHECK_EQ(anon.call(r, Op::Get, "/").body,
                 "{\"dev\":{\"brightness\":10,\"net\":{\"port\":80,\"password\":null}}}");
        CHECK_EQ(admin.call(r, Op::Get, "/").body, anon.call(r, Op::Get, "/").body);
    }
    {
        // At admin: the gateway's own view, for admins only. Others see
        // null, and their subscriptions to the node are forwarded.
        Rig r;
        r.dev.mirror(0, 30000, Access::Admin);
        r.settle();
        Client anon(r.gateway, Access::Public), admin(r.gateway, Access::Admin);
        CHECK_EQ(anon.call(r, Op::Get, "/").body, "{\"dev\":null}");
        CHECK(admin.call(r, Op::Get, "/").body.find("\"logins\":0") != std::string::npos);
        CHECK_EQ(anon.call(r, Op::Subscribe, "/").status, Status::Ok);
        CHECK_EQ(admin.call(r, Op::Subscribe, "/").status, Status::Ok);
        CHECK_EQ(anon.call(r, Op::Subscribe, "/dev").body,
                 "{\"brightness\":10,\"net\":{\"port\":80,\"password\":null}}");
        CHECK_EQ(r.dev.upstreamCount(), 1u);  // anon's, beside the mirror
        r.device.logins = 4;
        r.device.api.changed("/users/logins");
        r.device.login->emit(1);
        anon.drain(r);
        admin.c.process();
        for (const std::string& n : anon.notes) CHECK(n.find("login") == std::string::npos);
        bool heard = false;
        for (const std::string& n : admin.notes) heard = heard || n.find("/dev/users/login") != std::string::npos;
        CHECK(heard);
    }
}

TEST(ceiling_file_relays) {
    // A NowTP client of the gateway that the gateway doesn't trust (public)
    // relays a transfer to the device: at its own level.
    Rig r;
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, r.net.sender(addr(3)), r.net.clockFn());
    r.net.nodes.push_back({addr(3), &client});
    auto send = [&](const char* op, const char* extra) {
        datagram_cases::Result res;
        std::string env = std::string("{\"op\":\"") + op + "\",\"path\":\"/dev/users/backup\"" + extra + "}";
        JsonDocument doc;
        deserializeJson(doc, env);
        client.request(addr(1), doc.as<JsonObjectConst>(), datagram_cases::capture(res));
        r.net.run();
        return res;
    };
    datagram_cases::Result down = send("download", "");
    CHECK_EQ(down.status, Status::Unauthorized);
    std::string up = ",\"size\":3,\"data\":\"" + file_envelope_cases::b64("pwn") + "\"";
    CHECK_EQ(send("upload", up.c_str()).status, Status::Unauthorized);
    CHECK_EQ(r.device.backup, "secrets");
    // Trusted by the gateway (admin): relayed as before.
    r.gatewayEnd.trust([](const tesser::PeerAddress&) { return true; });
    CHECK_EQ(send("download", "").status, Status::Ok);
    CHECK_EQ(send("upload", up.c_str()).status, Status::Ok);
    CHECK_EQ(r.device.backup, "pwn");
}

// client → A → B → device, each node trusting the one before it as an
// admin. Endpoints are declared before the trees holding remote nodes.
struct TwoHops {
    Device device;
    datagram_cases::Net net;
    DatagramEndpoint deviceEnd, bEnd, aEnd;
    tesser::Api b, a;

    TwoHops()
        : deviceEnd(&device.api, tesser::TransportKind::NowTP, net.sender(addr(3)), net.clockFn()),
          bEnd(&b, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn()),
          aEnd(&a, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn()) {
        b.remote("dev", bEnd, addr(3));
        a.remote("b", aEnd, addr(2));
        deviceEnd.trust([](const tesser::PeerAddress& p) { return p == addr(2); });
        bEnd.trust([](const tesser::PeerAddress& p) { return p == addr(1); });
        net.nodes = {{addr(1), &aEnd}, {addr(2), &bEnd}, {addr(3), &deviceEnd}};
    }

    Resp call(Access level, Op op, const char* path, const char* body = "") {
        tesser::LocalClient c(a);
        c.setAccess(level);
        Resp out{Status::Internal, "<no reply>"};
        c.request(op, path, body, [&out](Status s, JsonVariantConst v) {
            out.status = s;
            out.body.clear();
            serializeJson(v, out.body);
        });
        net.run();
        c.process();
        return out;
    }
};

TEST(ceiling_two_hops) {
    TwoHops h;
    CHECK_EQ(h.call(Access::Public, Op::Get, "/b/dev/users/logins").status, Status::Unauthorized);
    CHECK_EQ(h.call(Access::User, Op::Set, "/b/dev/net/port", "90").status, Status::Unauthorized);
    CHECK_EQ(h.device.port, 80);
    CHECK_EQ(h.call(Access::Admin, Op::Get, "/b/dev/users/logins").body, "0");
    CHECK_EQ(h.call(Access::Admin, Op::Set, "/b/dev/net/port", "90").status, Status::Ok);
    CHECK_EQ(h.device.port, 90);
}

}  // namespace ceiling_cases

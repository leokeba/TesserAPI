// UdpLink over an in-memory packet network: the UDP transport's protocol
// (fragments, acknowledgements, broadcasts, discovery), tested without
// sockets.
#pragma once

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "core_cases.h"
#include "datagram_cases.h"

namespace udp_cases {

using tesser::PeerAddress;
using tesser::Status;
using tesser::UdpLink;

inline PeerAddress ip(uint8_t last, uint16_t port = 8484) { return tesser::udpAddress(192, 168, 1, last, port); }

// Datagrams in flight. An empty destination reaches every node, the sender
// included, as a real stack may loop a broadcast back; x.x.x.255 reaches
// every node on that port.
struct Net {
    struct Datagram {
        PeerAddress from, to;
        std::vector<uint8_t> data;
    };
    std::deque<Datagram> air;
    std::vector<std::pair<PeerAddress, UdpLink*>> nodes;
    uint32_t clock = 1000;
    // Returns true to lose a datagram.
    std::function<bool(const Datagram&)> lose;
    int lost = 0;

    UdpLink::PacketFn sender(PeerAddress self) {
        return [this, self](const PeerAddress& to, const uint8_t* d, size_t n) {
            air.push_back(Datagram{self, to, std::vector<uint8_t>(d, d + n)});
            return true;
        };
    }
    tesser::DatagramEndpoint::ClockFn clockFn() {
        return [this] { return clock; };
    }
    static bool broadcast(const PeerAddress& a) { return a.length == 0 || (a.length == 6 && a.bytes[3] == 255); }
    void deliver() {
        while (!air.empty()) {
            Datagram d = air.front();
            air.pop_front();
            if (lose && lose(d)) {
                lost++;
                continue;
            }
            for (auto& n : nodes) {
                bool to = d.to.length == 0 || n.first == d.to ||
                          (broadcast(d.to) && n.first.bytes[4] == d.to.bytes[4] && n.first.bytes[5] == d.to.bytes[5]);
                if (to) n.second->packet(d.from, d.data.data(), d.data.size());
            }
        }
    }
    // Delivers and ticks until quiet.
    void run() {
        for (int round = 0; round < 50; round++) {
            deliver();
            bool any = false;
            for (auto& n : nodes) any = n.second->tick() || any;
            if (air.empty() && !any) break;
        }
    }
    // Lets `ms` pass in 10 ms steps.
    void advance(uint32_t ms) {
        for (uint32_t t = 0; t < ms; t += 10) {
            clock += 10;
            run();
        }
    }
    void start() {
        uint32_t nonce = 0x1000;
        for (auto& n : nodes) n.second->start(nonce++);
        run();
    }
};

struct Result {
    int calls = 0;
    Status status = Status::Internal;
    std::string body;
};

inline tesser::DatagramEndpoint::ResponseHandler capture(Result& r) {
    return [&r](Status s, JsonVariantConst body) {
        r.calls++;
        r.status = s;
        r.body.clear();
        serializeJson(body, r.body);
    };
}

// A device serving its API, and a client without one.
struct Pair {
    cases::Device dev;
    Net net;
    UdpLink server;
    UdpLink client;

    explicit Pair(UdpLink::Options o = UdpLink::Options())
        : server(&dev.api, net.sender(ip(2)), net.clockFn(), o),
          client(nullptr, net.sender(ip(1)), net.clockFn(), o) {
        net.nodes = {{ip(1), &client}, {ip(2), &server}};
        net.start();
    }
    Result call(tesser::Op op, const char* path, const char* body = "", tesser::Query q = tesser::Query()) {
        Result r;
        client.endpoint().request(ip(2), op, path, body, capture(r), q);
        net.run();
        return r;
    }
};

TEST(udp_addresses) {
    PeerAddress a;
    CHECK(tesser::parseUdpAddress("192.168.1.59", 8484, a));
    CHECK(a == ip(59));
    CHECK_EQ(tesser::formatUdpAddress(a), "192.168.1.59:8484");
    CHECK(tesser::parseUdpAddress("10.0.0.255:9", 8484, a));
    CHECK_EQ(tesser::formatUdpAddress(a), "10.0.0.255:9");
    CHECK(!tesser::parseUdpAddress("10.0.0", 1, a));
    CHECK(!tesser::parseUdpAddress("10.0.0.256", 1, a));
    CHECK(!tesser::parseUdpAddress("10.0.0.1:", 1, a));
    CHECK(!tesser::parseUdpAddress("10.0.0.1:70000", 1, a));
    CHECK(!tesser::parseUdpAddress("10.0.0.1x", 1, a));
    CHECK_EQ(tesser::formatUdpAddress(PeerAddress()), "");
}

TEST(udp_request_response) {
    Pair p;
    Result r = p.call(tesser::Op::Get, "/lamp", "", cases::keys("on,brightness"));
    CHECK_EQ(r.calls, 1);
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "{\"on\":false,\"brightness\":128}");
    r = p.call(tesser::Op::Set, "/lamp/brightness", "7");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(p.dev.lamp.brightness, 7);
    CHECK_EQ(p.server.endpoint().stats().requests, 2u);
    CHECK_EQ(p.client.stats().failed, 0u);
}

TEST(udp_fragments) {
    // Small fragments: the schema takes many datagrams each way.
    UdpLink::Options o;
    o.fragmentSize = 64;
    Pair p(o);
    tesser::StringReply local;
    tesser::Request req;
    req.path = "/";
    req.query = cases::schema();
    p.dev.api.handle(req, local);
    uint32_t sent = p.server.stats().sent;
    Result r = p.call(tesser::Op::Get, "/", "", cases::schema());
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, local.body);
    CHECK(p.server.stats().sent - sent > local.body.size() / 64);
    // A body that spans fragments, in a request.
    std::string label = "\"abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz\"";
    r = p.call(tesser::Op::Set, "/config/owner", label.c_str());
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(p.dev.owner, label.substr(1, label.size() - 2));
}

TEST(udp_losses_and_repeats) {
    UdpLink::Options o;
    o.fragmentSize = 64;
    Pair p(o);
    // Every third datagram is lost, whatever it carries: fragments, acks,
    // whole small messages. Calls still complete, each applied once.
    int n = 0;
    p.net.lose = [&n](const Net::Datagram&) { return ++n % 3 == 0; };
    for (int i = 0; i < 5; i++) {
        Result r;
        p.client.endpoint().request(ip(2), tesser::Op::Set, "/lamp/toggle", "", capture(r));
        p.net.advance(1500);
        CHECK_EQ(r.calls, 1);
        CHECK_EQ(r.status, Status::Ok);
    }
    CHECK_EQ(p.dev.toggles, 5);
    Result r;
    p.client.endpoint().request(ip(2), tesser::Op::Get, "/", "", capture(r), cases::schema());
    p.net.advance(1500);
    CHECK_EQ(r.status, Status::Ok);
    CHECK(p.net.lost > 0);
    CHECK(p.client.stats().retransmits > 0);
    CHECK(p.server.stats().retransmits > 0);
    CHECK_EQ(p.client.stats().failed + p.server.stats().failed, 0u);

    // A lost acknowledgement: the request comes again and is acknowledged,
    // not applied twice.
    p.net.lose = [](const Net::Datagram& d) { return d.from == ip(2) && d.data[1] == 0x11 && d.data[3] == 0; };
    Result t;
    p.client.endpoint().request(ip(2), tesser::Op::Set, "/lamp/toggle", "", capture(t));
    p.net.run();
    p.net.lose = nullptr;
    p.net.advance(500);
    CHECK_EQ(t.status, Status::Ok);
    CHECK_EQ(p.dev.toggles, 6);
    CHECK(p.server.stats().duplicates > 0);
}

TEST(udp_unreachable_peer) {
    Pair p;
    // A peer that never answers: the call times out, the message fails and
    // the peer is forgotten.
    Result r;
    p.client.endpoint().request(ip(9), tesser::Op::Get, "/", "", capture(r));
    p.net.advance(3500);
    CHECK_EQ(r.calls, 1);
    CHECK_EQ(r.status, Status::Timeout);
    CHECK_EQ(p.client.stats().failed, 1u);
    // Too large to send: refused at once.
    UdpLink::Options o;
    o.maxMessage = 100;
    Pair small(o);
    std::string big = "\"" + std::string(200, 'x') + "\"";
    CHECK(!small.client.endpoint().request(ip(2), tesser::Op::Set, "/config/owner", big, capture(r)));
    // Stopped: nothing goes out.
    p.client.stop();
    CHECK(!p.client.endpoint().request(ip(2), tesser::Op::Get, "/", "", capture(r)));
}

TEST(udp_subscriptions) {
    Pair p;
    std::vector<std::string> notes;
    p.client.endpoint().onNotification([&](const PeerAddress& from, JsonObjectConst env) {
        CHECK(from == ip(2));
        std::string s;
        serializeJson(env, s);
        notes.push_back(s);
    });
    Result r = p.call(tesser::Op::Subscribe, "/lamp", "", cases::keys("brightness"));
    CHECK_EQ(r.body, "{\"brightness\":128}");
    p.dev.lamp.brightness = 3;
    p.dev.api.changed("/lamp/brightness");
    p.dev.api.poll(2000);
    p.net.run();
    CHECK_EQ(notes.size(), 1u);
    CHECK_EQ(notes.back(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"brightness\":3}}");
    // A client that went away: its notifications fail, and it is forgotten
    // with its subscriptions.
    p.net.nodes.pop_back();
    p.net.nodes.erase(p.net.nodes.begin());
    p.net.nodes.push_back({ip(2), &p.server});
    p.dev.lamp.brightness = 4;
    p.dev.api.changed("/lamp/brightness");
    p.dev.api.poll(2100);
    p.net.advance(3500);
    CHECK_EQ(p.server.stats().failed, 1u);
    p.dev.lamp.brightness = 5;
    p.dev.api.changed("/lamp/brightness");
    uint32_t sent = p.server.stats().sent;
    p.dev.api.poll(6000);
    p.net.run();
    CHECK_EQ(p.server.stats().sent, sent);
}

// Three devices and a sender, which serves an API too.
struct Fleet {
    cases::Device devs[3];
    cases::Device own;
    Net net;
    UdpLink a, b, c;
    UdpLink sender;

    Fleet()
        : a(&devs[0].api, net.sender(ip(10)), net.clockFn()),
          b(&devs[1].api, net.sender(ip(11)), net.clockFn()),
          c(&devs[2].api, net.sender(ip(12)), net.clockFn()),
          sender(&own.api, net.sender(ip(1)), net.clockFn()) {
        net.nodes = {{ip(10), &a}, {ip(11), &b}, {ip(12), &c}, {ip(1), &sender}};
        net.start();
    }
};

TEST(udp_broadcast_set) {
    Fleet f;
    // One datagram (twice) to every network: applied once by every node
    // but the sender, and answered by none.
    uint32_t sent = f.sender.stats().sent;
    CHECK(f.sender.broadcastSet(PeerAddress(), "/lamp/brightness", "77"));
    CHECK_EQ(f.sender.stats().sent - sent, 2u);
    f.net.run();
    for (auto& d : f.devs) CHECK_EQ(d.lamp.brightness, 77);
    CHECK_EQ(f.own.lamp.brightness, 128);
    CHECK_EQ(f.devs[0].api.subscriptionCount(), 0u);
    CHECK_EQ(f.a.endpoint().stats().requests, 1u);
    CHECK_EQ(f.a.stats().duplicates, 1u);
    CHECK_EQ(f.sender.endpoint().stats().responses, 0u);
    CHECK_EQ(f.a.stats().sent + f.b.stats().sent + f.c.stats().sent, 6u);  // their start queries only
    // A lost first copy: the second is applied, once.
    int toggles = f.devs[1].toggles;
    bool first = true;
    f.net.lose = [&](const Net::Datagram& d) {
        if (d.from == ip(1) && first) {
            first = false;
            return true;
        }
        return false;
    };
    CHECK(f.sender.broadcastSet(PeerAddress(), "/lamp/toggle", ""));
    f.net.run();
    f.net.lose = nullptr;
    for (auto& d : f.devs) CHECK_EQ(d.toggles, toggles + 1);
    // A directed broadcast address works the same.
    CHECK(f.sender.broadcastSet(ip(255), "/lamp", "{\"on\":true}"));
    f.net.run();
    for (auto& d : f.devs) CHECK(d.lamp.on);

    // Replies collected for a window, then the end.
    std::vector<std::string> replies;
    bool ended = false;
    CHECK(f.sender.endpoint().broadcast(
        PeerAddress(), tesser::Op::Set, "/lamp/brightness", "5",
        [&](const PeerAddress& from, Status s, JsonVariantConst body) {
            if (from.length == 0) {
                CHECK_EQ(s, Status::Timeout);
                ended = true;
                return;
            }
            std::string b;
            serializeJson(body, b);
            replies.push_back(tesser::formatUdpAddress(from) + " " + tesser::toString(s) + " " + b);
        },
        200));
    f.net.run();
    CHECK_EQ(replies.size(), 3u);
    CHECK(!ended);
    f.net.advance(250);
    CHECK(ended);
    CHECK_EQ(f.sender.endpoint().pendingCalls(), 0u);
    for (auto& d : f.devs) CHECK_EQ(d.lamp.brightness, 5);
    CHECK_EQ(f.sender.endpoint().stats().timeouts, 0u);

    // Errors aren't answered either, and only set is applied by broadcast.
    CHECK(f.sender.broadcastSet(PeerAddress(), "/lamp/brightness", "999"));
    CHECK(f.sender.endpoint().broadcast(PeerAddress(), tesser::Op::Subscribe, "/lamp", ""));
    f.net.run();
    CHECK_EQ(f.devs[0].lamp.brightness, 5);
    CHECK_EQ(f.sender.endpoint().stats().responses, 3u);
    CHECK_EQ(f.devs[0].api.subscriptionCount(), 0u);
}

TEST(udp_peer_access_and_tokens) {
    Pair p;
    p.dev.api.object("sys").readAccess(tesser::Access::Admin).value("secret", p.dev.level);
    p.dev.api.object("user").writeAccess(tesser::Access::User).value("x", p.dev.gain);
    p.dev.api.authenticate([](std::string_view token) {
        if (token == "root") return tesser::Access::Admin;
        return tesser::Access::Public;
    });
    auto auth = [&](const char* token) {
        Result r;
        std::string fields = std::string("\"op\":\"auth\",\"token\":\"") + token + "\"";
        p.client.endpoint().requestFields(ip(2), fields, capture(r));
        p.net.run();
        return r.body;
    };
    CHECK_EQ(p.call(tesser::Op::Set, "/user/x", "1").status, Status::Unauthorized);
    p.server.endpoint().setPeerAccess(tesser::Access::User);
    CHECK_EQ(p.call(tesser::Op::Set, "/user/x", "1").status, Status::Ok);
    CHECK_EQ(p.call(tesser::Op::Get, "/sys/secret").status, Status::Unauthorized);
    // A broadcast write is at the same level.
    CHECK(p.client.broadcastSet(PeerAddress(), "/sys/secret", "9"));
    p.net.run();
    CHECK_EQ(p.dev.level, 3);
    CHECK_EQ(auth("root"), "{\"access\":\"admin\"}");
    CHECK_EQ(p.call(tesser::Op::Get, "/sys/secret").body, "3");
    CHECK(p.client.broadcastSet(PeerAddress(), "/sys/secret", "9"));
    p.net.run();
    CHECK_EQ(p.dev.level, 9);
    // Subscriptions made as admin end when the level drops.
    CHECK_EQ(p.call(tesser::Op::Subscribe, "/sys").status, Status::Ok);
    CHECK_EQ(p.dev.api.subscriptionCount(), 1u);
    CHECK_EQ(auth("wrong"), "{\"access\":\"user\"}");
    CHECK_EQ(p.dev.api.subscriptionCount(), 0u);
    CHECK_EQ(p.call(tesser::Op::Get, "/sys/secret").status, Status::Unauthorized);
    CHECK_EQ(auth("root"), "{\"access\":\"admin\"}");
    CHECK_EQ(auth(""), "{\"access\":\"user\"}");
    // A restarted client starts over without its token.
    CHECK_EQ(auth("root"), "{\"access\":\"admin\"}");
    p.client.stop();
    p.client.start(0x9999);
    p.net.run();
    CHECK_EQ(p.call(tesser::Op::Get, "/sys/secret").status, Status::Unauthorized);
}

TEST(udp_discovery) {
    cases::Device lamp, fan;
    tesser::Api gateway;
    Net net;
    UdpLink lampLink(&lamp.api, net.sender(ip(20)), net.clockFn());
    UdpLink fanLink(&fan.api, net.sender(ip(21)), net.clockFn());
    UdpLink gw(&gateway, net.sender(ip(1)), net.clockFn());
    auto& peers = gateway.object("peers");
    net.nodes = {{ip(20), &lampLink}, {ip(21), &fanLink}, {ip(1), &gw}};
    lampLink.setName("lamp");
    uint8_t mac[6] = {0x10, 0x52, 0x1c, 0x75, 0xf3, 0x7c};
    lampLink.setDeviceId(mac);
    lampLink.advertise();
    fanLink.setName("lamp");  // the same name: a suffix tells them apart
    fanLink.advertise();
    net.start();
    gw.mountPeers(peers);
    net.run();
    CHECK_EQ(cases::get(gateway, "/peers", cases::depth(1)).body, "{\"lamp\":null,\"lamp-152124\":null}");
    tesser::Query q = cases::schema(1);
    q.keys = "lamp";
    char hash[9];
    snprintf(hash, sizeof(hash), "%08lx", static_cast<unsigned long>(lamp.api.schemaHash()));
    CHECK_EQ(cases::get(gateway, "/peers", q).body,
             std::string("{\"type\":\"object\",\"children\":{\"lamp\":{\"type\":\"remote\",\"schema\":\"") + hash +
                 "\"}}}");

    // Forwarded through the mounted node.
    tesser::LocalClient client(gateway);
    Result r;
    client.get("/peers/lamp/lamp/brightness", [&](Status s, JsonVariantConst body) {
        r.calls++;
        r.status = s;
        serializeJson(body, r.body);
    });
    for (int i = 0; i < 5; i++) {
        net.run();
        client.process();
    }
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "128");

    // Leaving says goodbye: offline at once.
    lampLink.stop();
    net.run();
    auto online = [&](const char* name) {
        auto* node = static_cast<const tesser::RemoteNode*>(peers.child(name));
        return node && node->online();
    };
    CHECK(!online("lamp"));
    CHECK(online("lamp-152124"));
    // Back with a new run: online again.
    lampLink.start(0x7777);
    net.run();
    CHECK(online("lamp"));
    // Silent for peerTimeoutMs: lost. Periodic advertisements keep the
    // others online.
    net.nodes.erase(net.nodes.begin());
    net.advance(14000);
    CHECK(online("lamp"));
    net.advance(2000);
    CHECK(!online("lamp"));
    CHECK(online("lamp-152124"));
    // A node that changes its tree advertises the new hash.
    fan.api.value("extra", fan.level);
    net.advance(1100);
    q.keys = "lamp-152124";
    snprintf(hash, sizeof(hash), "%08lx", static_cast<unsigned long>(fan.api.schemaHash()));
    CHECK(cases::get(gateway, "/peers", q).body.find(hash) != std::string::npos);
}

TEST(udp_device_mounted_once) {
    // A gateway reaching the same device over NowTP and UDP mounts it once,
    // whichever announces it first.
    cases::Device dev;
    tesser::Api gateway;
    auto& peers = gateway.object("peers");
    datagram_cases::Net radio;
    tesser::DatagramEndpoint now(&gateway, tesser::TransportKind::NowTP, radio.sender(datagram_cases::addr(1)),
                                 radio.clockFn());
    Net net;
    UdpLink devLink(&dev.api, net.sender(ip(20)), net.clockFn());
    UdpLink gw(&gateway, net.sender(ip(1)), net.clockFn());
    net.nodes = {{ip(20), &devLink}, {ip(1), &gw}};
    PeerAddress mac = datagram_cases::addr(0x7c);
    devLink.setName("lamp");
    devLink.setDeviceId(mac.bytes);
    devLink.advertise();
    now.mountPeers(peers);
    gw.mountPeers(peers);
    now.peerSeen(mac, "lamp", 1);
    now.process();
    net.start();
    CHECK_EQ(cases::get(gateway, "/peers", cases::depth(1)).body, "{\"lamp\":null}");
    CHECK(static_cast<const tesser::RemoteNode*>(peers.child("lamp"))->endpoint() == &now);

    // UDP first, then NowTP.
    tesser::Api gateway2;
    auto& peers2 = gateway2.object("peers");
    tesser::DatagramEndpoint now2(&gateway2, tesser::TransportKind::NowTP, radio.sender(datagram_cases::addr(2)),
                                  radio.clockFn());
    UdpLink gw2(&gateway2, net.sender(ip(2)), net.clockFn());
    net.nodes.push_back({ip(2), &gw2});
    gw2.start(0x4242);
    now2.mountPeers(peers2);
    gw2.mountPeers(peers2);
    net.run();
    now2.peerSeen(mac, "lamp", 1);
    now2.process();
    CHECK_EQ(cases::get(gateway2, "/peers", cases::depth(1)).body, "{\"lamp\":null}");
    CHECK(static_cast<const tesser::RemoteNode*>(peers2.child("lamp"))->endpoint() == &gw2.endpoint());
    // Another device of the same name over NowTP is a different one.
    now2.peerSeen(datagram_cases::addr(0x7d), "lamp", 1);
    now2.process();
    CHECK_EQ(cases::get(gateway2, "/peers", cases::depth(1)).body, "{\"lamp\":null,\"lamp-75f37d\":null}");
}

TEST(udp_restarted_peer) {
    Pair p;
    CHECK_EQ(p.call(tesser::Op::Subscribe, "/lamp").status, Status::Ok);
    CHECK_EQ(p.dev.api.subscriptionCount(), 1u);
    // The client comes back with a new nonce: its old subscriptions go.
    p.client.stop();
    p.client.start(0x5555);
    p.net.run();
    CHECK_EQ(p.dev.api.subscriptionCount(), 0u);
    CHECK_EQ(p.call(tesser::Op::Get, "/lamp/on").body, "false");
}

TEST(udp_malformed) {
    Pair p;
    uint32_t dropped = p.server.stats().dropped;
    uint8_t junk[40] = {0x54, 0x10, 0, 5, 1, 2, 3, 4, 1, 0, 0, 0, 2};  // index past count
    p.server.packet(ip(7), junk, sizeof(junk));
    uint8_t shortOne[5] = {0x54, 0x10};
    p.server.packet(ip(7), shortOne, sizeof(shortOne));
    uint8_t other[24] = {'G', 'E', 'T'};
    p.server.packet(ip(7), other, sizeof(other));
    p.net.run();
    CHECK_EQ(p.server.stats().dropped - dropped, 3u);
    CHECK_EQ(p.call(tesser::Op::Get, "/lamp/on").body, "false");
}

}  // namespace udp_cases

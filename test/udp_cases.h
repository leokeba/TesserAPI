// UdpLink over an in-memory packet network: the UDP transport's protocol
// (fragments, acknowledgements, broadcasts, discovery), tested without
// sockets.
#pragma once

#include <deque>
#include <functional>
#include <memory>
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
    // Changes a datagram on its way, before lose.
    std::function<void(Datagram&)> alter;
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
            if (alter) alter(d);
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

// Api::tokensChanged(): a peer's token is checked again; a trusted peer
// keeps its subscriptions.
TEST(udp_tokens_changed) {
    Pair p;
    p.dev.api.object("sys").readAccess(tesser::Access::Admin).value("secret", p.dev.level);
    bool valid = true;
    p.dev.api.authenticate([&](std::string_view token) {
        return valid && token == "root" ? tesser::Access::Admin : tesser::Access::Public;
    });
    Result r;
    p.client.endpoint().requestFields(ip(2), "\"op\":\"auth\",\"token\":\"root\"", capture(r));
    p.net.run();
    CHECK_EQ(r.body, "{\"access\":\"admin\"}");
    CHECK_EQ(p.call(tesser::Op::Subscribe, "/sys").status, Status::Ok);
    valid = false;
    p.dev.api.tokensChanged();
    CHECK_EQ(p.dev.api.subscriptionCount(), 0u);
    CHECK_EQ(p.call(tesser::Op::Get, "/sys/secret").status, Status::Unauthorized);
    p.server.endpoint().trust([](const PeerAddress&) { return true; });
    CHECK_EQ(p.call(tesser::Op::Subscribe, "/sys").status, Status::Ok);
    p.dev.api.tokensChanged();
    CHECK_EQ(p.dev.api.subscriptionCount(), 1u);
}

// The token check is given the peer: transport and address.
TEST(udp_token_check_sees_peer) {
    Pair p;
    p.dev.api.object("user").writeAccess(tesser::Access::User).value("x", p.dev.gain);
    tesser::Client last;
    p.dev.api.authenticate([&](std::string_view, const tesser::Client& client) {
        last = client;
        // Open to one address only.
        return client.addressLength == ip(1).length && memcmp(client.address, ip(1).bytes, ip(1).length) == 0
                   ? tesser::Access::User
                   : tesser::Access::Public;
    });
    CHECK_EQ(p.call(tesser::Op::Set, "/user/x", "1").status, Status::Ok);
    CHECK(last.transport == tesser::TransportKind::Udp);
    CHECK(!last.authenticated && last.access == tesser::Access::Public);  // not decided yet
}

TEST(udp_discovery) {
    cases::Device lamp, fan;
    tesser::Api gateway;
    Net net;
    UdpLink::Options small;
    small.maxMessage = 4000;  // the lamp takes less than the gateway
    UdpLink lampLink(&lamp.api, net.sender(ip(20)), net.clockFn(), small);
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
    // Each advertises the largest message it takes (docs/DESIGN.md section 14.1).
    CHECK_EQ(gw.endpoint().peerLimit(ip(20)), 4000u);
    CHECK_EQ(gw.endpoint().peerLimit(ip(21)), 16384u);
    tesser::Query q = cases::schema(1);
    q.keys = "lamp";
    char hash[9];
    snprintf(hash, sizeof(hash), "%08lx", static_cast<unsigned long>(lamp.api.schemaHash()));
    CHECK_EQ(cases::get(gateway, "/peers", q).body,
             std::string("{\"type\":\"object\",\"children\":{\"lamp\":{\"type\":\"remote\",\"schema\":\"") + hash +
                 "\",\"id\":\"10521c75f37c\",\"protocol\":2}}}");

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

namespace udp_cases {

// A peer mirroring this node subscribes (again, when it comes back): this
// node's snapshot goes in fragments. Returns the most of its full-size
// fragments in flight at once, and whether the peer got its copy.
inline size_t mirrorSnapshotInFlight(size_t limitAbove, size_t floor, bool& mirrored) {
    cases::Device v;
    std::string blob(3000, 'x');
    v.api.value("blob", blob);
    tesser::Api r;
    Net net;
    UdpLink vLink(&v.api, net.sender(ip(20)), net.clockFn());
    UdpLink rLink(&r, net.sender(ip(21)), net.clockFn());
    net.nodes = {{ip(20), &vLink}, {ip(21), &rLink}};
    vLink.setName("v");
    rLink.setName("r");
    vLink.advertise();
    rLink.advertise();
    net.start();
    rLink.mountPeers(r.object("peers"), 1000);
    size_t most = 0;
    size_t floorBefore = tesser::heapFloor();
    for (int round = 0; round < 200; round++) {
        net.deliver();
        rLink.tick();
        {
            // This node's free heap is what its limit leaves; the air's
            // datagrams count too, as the stack's copies do on target.
            tesser::testing::HeapLimit limit(limitAbove);
            // Only the probe's reading matters here: what goes past the
            // limit (the air's copies) isn't an abort.
            tesser::testing::onExceeded(+[](size_t, size_t, size_t) {});
            if (limitAbove) {
                tesser::setFreeHeapProbe(&tesser::testing::heapLeft);
                tesser::setHeapFloor(floor);
            }
            vLink.tick();
            tesser::testing::onExceeded(nullptr);
            tesser::setFreeHeapProbe(nullptr);
            tesser::setHeapFloor(floorBefore);
        }
        size_t inFlight = 0;
        for (const Net::Datagram& d : net.air) inFlight += d.from == ip(20) && d.data.size() > 1000;
        if (inFlight > most) most = inFlight;
        net.clock += 20;
    }
    auto* node = static_cast<tesser::RemoteNode*>(r.object("peers").child("v"));
    mirrored = node && node->hasCopy();
    return most;
}

TEST(udp_fragments_paced_on_a_tight_heap) {
    // A reliable message's fragments go only as far as the heap holds the
    // stack's copies of them above the floor; the rest follow the
    // acknowledgements. On target, a mirror snapshot sent all at once took
    // a gateway 2 KB below its floor.
    bool mirrored = false;
    CHECK_EQ(mirrorSnapshotInFlight(0, 0, mirrored), 2u);  // all at once with room (the third is short)
    CHECK(mirrored);
    mirrored = false;
    CHECK_EQ(mirrorSnapshotInFlight(11000, 4000, mirrored), 1u);
    CHECK(mirrored);
}

// The message id of a datagram (header bytes 8-11).
inline uint32_t datagramId(const std::vector<uint8_t>& d) {
    return uint32_t(d[8]) | uint32_t(d[9]) << 8 | uint32_t(d[10]) << 16 | uint32_t(d[11]) << 24;
}

TEST(udp_window_gap_after_broadcasts) {
    // A reliable message to the server is lost; more than 64 broadcasts go
    // by (that the server doesn't get, as to another subnet); a later message
    // completes first. The first one's retransmission is still delivered:
    // the server keeps broadcasts in their own window, and the client numbers
    // its messages to the server on their own.
    Pair p;
    bool dropFirst = true;
    std::vector<uint32_t> ids;
    p.net.lose = [&](const Net::Datagram& d) {
        if (d.to == ip(2)) {
            if ((d.data[1] & 0x0f) == 0) ids.push_back(datagramId(d.data));  // data
            return dropFirst;
        }
        return d.to.length == 0 || d.to.bytes[3] == 255;
    };
    Result r1, r2;
    CHECK(p.client.endpoint().request(ip(2), tesser::Op::Set, "/lamp/brightness", "77", capture(r1)));
    p.net.deliver();
    dropFirst = false;
    for (int i = 0; i < 70; i++) {
        p.client.endpoint().broadcast(tesser::udpAddress(10, 0, 0, 255, 8484), tesser::Op::Set, "/x", "1");
    }
    CHECK(p.client.endpoint().request(ip(2), tesser::Op::Get, "/lamp/on", "", capture(r2)));
    p.net.run();
    CHECK_EQ(r2.calls, 1);
    CHECK_EQ(r2.status, Status::Ok);
    p.net.advance(500);  // the set goes again
    CHECK_EQ(r1.calls, 1);
    CHECK_EQ(r1.status, Status::Ok);
    CHECK_EQ(p.dev.lamp.brightness, 77);
    CHECK_EQ(p.server.stats().duplicates, 0u);
    CHECK(ids.size() >= 2);
    if (ids.size() >= 2) CHECK_EQ(ids[1], ids[0] + 1);  // the broadcasts' ids aren't in between
}

TEST(udp_ids_for_an_older_peer) {
    // A peer that doesn't say it keeps broadcasts apart (an older node) has
    // one window for everything: messages to it take the common count, above
    // the broadcasts' ids, or it would take them for repeats.
    Pair p;
    UdpLink older(&p.dev.api, p.net.sender(ip(3)), p.net.clockFn());
    p.net.nodes.push_back({ip(3), &older});
    std::vector<uint32_t> ids;
    p.net.alter = [&](Net::Datagram& d) {
        if (d.from == ip(3)) d.data[2] &= 0x7f;  // its flag doesn't arrive
        if (d.to == ip(3) && (d.data[1] & 0x0f) == 0) ids.push_back(datagramId(d.data));  // data
    };
    older.start(0x3000);
    p.net.run();
    Result r1, r2;
    p.client.endpoint().request(ip(3), tesser::Op::Get, "/lamp/on", "", capture(r1));
    p.net.run();
    for (int i = 0; i < 70; i++) p.client.endpoint().broadcast(PeerAddress(), tesser::Op::Set, "/x", "1");
    p.net.run();
    p.client.endpoint().request(ip(3), tesser::Op::Get, "/lamp/on", "", capture(r2));
    p.net.run();
    CHECK_EQ(r1.status, Status::Ok);
    CHECK_EQ(r2.status, Status::Ok);
    CHECK(ids.size() >= 2);
    if (ids.size() >= 2) CHECK(ids.back() > ids.front() + 70);
}

TEST(udp_peer_table_keeps_windows) {
    // More peers than maxPeers talk to the server while the client's
    // acknowledgement is lost: the client's entry, whose window the
    // retransmission needs, stays, and the set runs once. A newcomer that
    // finds no room is dropped, as if lost.
    cases::Device dev;
    Net net;
    UdpLink::Options o;
    o.maxPeers = 4;
    UdpLink client(nullptr, net.sender(ip(1)), net.clockFn(), o);
    UdpLink server(&dev.api, net.sender(ip(2)), net.clockFn(), o);
    std::vector<std::unique_ptr<UdpLink>> others;
    net.nodes = {{ip(1), &client}, {ip(2), &server}};
    for (uint8_t i = 0; i < 5; i++) {
        others.emplace_back(new UdpLink(nullptr, net.sender(ip(uint8_t(10 + i))), net.clockFn(), o));
        net.nodes.push_back({ip(uint8_t(10 + i)), others.back().get()});
    }
    net.start();
    net.advance(4000);  // the start's queries are old news
    bool dropAcks = true;
    net.lose = [&](const Net::Datagram& d) { return dropAcks && d.from == ip(2) && d.to == ip(1); };
    Result r;
    uint32_t before = server.endpoint().stats().requests;
    CHECK(client.endpoint().request(ip(2), tesser::Op::Set, "/lamp/brightness", "77", capture(r)));
    net.run();
    CHECK_EQ(server.endpoint().stats().requests, before + 1);
    std::vector<Result> theirs(others.size());
    for (size_t i = 0; i < others.size(); i++) {
        others[i]->endpoint().request(ip(2), tesser::Op::Get, "/lamp/on", "", capture(theirs[i]));
    }
    net.run();
    dropAcks = false;
    net.advance(200);  // the client's retransmission
    CHECK_EQ(r.calls, 1);
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(server.stats().duplicates, 1u);
    // Three newcomers found room; two found the table full for as long as
    // they tried, and failed. Later, there is room again.
    net.advance(4000);
    size_t ok = 0;
    for (const Result& t : theirs) ok += t.status == Status::Ok;
    CHECK_EQ(ok, 3u);
    CHECK_EQ(server.endpoint().stats().requests, before + 4);
    Result again;
    others.back()->endpoint().request(ip(2), tesser::Op::Get, "/lamp/on", "", capture(again));
    net.advance(200);
    CHECK_EQ(again.status, Status::Ok);
}

TEST(udp_forgotten_advertised_peer_reported) {
    // An advertised peer the table forgets to make room is reported silent:
    // its restart would otherwise go unseen.
    tesser::Api gateway;
    Net net;
    UdpLink::Options o;
    o.maxPeers = 2;
    UdpLink gw(&gateway, net.sender(ip(1)), net.clockFn(), o);
    std::vector<std::unique_ptr<cases::Device>> devs;
    std::vector<std::unique_ptr<UdpLink>> links;
    net.nodes = {{ip(1), &gw}};
    for (uint8_t i = 0; i < 3; i++) {
        devs.emplace_back(new cases::Device);
        links.emplace_back(new UdpLink(&devs.back()->api, net.sender(ip(uint8_t(20 + i))), net.clockFn()));
        links.back()->setName(std::string("d") + char('0' + i));
        net.nodes.push_back({ip(uint8_t(20 + i)), links.back().get()});
    }
    std::vector<std::pair<PeerAddress, tesser::PeerLoss>> lost;
    gw.endpoint().onPeerLost([&](const PeerAddress& peer, const PeerAddress&, tesser::PeerLoss why) {
        lost.emplace_back(peer, why);
    });
    gw.start(0x100);
    links[0]->start(0x200);
    links[1]->start(0x201);
    links[0]->advertise();
    links[1]->advertise();
    net.run();
    net.advance(4000);
    CHECK(lost.empty());
    links[2]->start(0x202);
    links[2]->advertise();
    net.run();
    CHECK_EQ(lost.size(), 1u);
    if (!lost.empty()) CHECK(lost[0].second == tesser::PeerLoss::Silent);
}

}  // namespace udp_cases

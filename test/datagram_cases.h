// DatagramEndpoint over an in-memory link: the logic behind the NowTP
// transport, tested without radios.
#pragma once

#include <deque>
#include <string>

#include "core_cases.h"

namespace datagram_cases {

using tesser::DatagramEndpoint;
using tesser::Delivery;
using tesser::PeerAddress;
using tesser::Status;

inline PeerAddress addr(uint8_t last) {
    uint8_t mac[6] = {0x10, 0x52, 0x1c, 0x75, 0xf3, last};
    return PeerAddress::fromBytes(mac, 6);
}

// Messages in flight between endpoints; deliver() hands them over.
struct Net {
    struct Frame {
        PeerAddress from, to;
        std::string text;
        Delivery delivery;
    };
    std::deque<Frame> frames;
    std::vector<std::pair<PeerAddress, DatagramEndpoint*>> nodes;
    uint32_t clock = 1000;
    bool dropAll = false;

    DatagramEndpoint::SendFn sender(PeerAddress self) {
        return [this, self](const PeerAddress& to, const std::string& m, Delivery d) {
            tesser::testing::HeapExempt link;  // the link's buffers (heap_cases.h)
            if (!dropAll) frames.push_back(Frame{self, to, m, d});
            return true;
        };
    }
    DatagramEndpoint::ClockFn clockFn() {
        return [this] { return clock; };
    }
    // Delivers and processes until quiet.
    void run() {
        for (int round = 0; round < 20; round++) {
            bool any = false;
            while (!frames.empty()) {
                Frame f;
                {
                    tesser::testing::HeapExempt link;
                    f = frames.front();
                    frames.pop_front();
                }
                for (auto& n : nodes) {
                    if (n.first == f.to) n.second->receive(f.from, f.text.data(), f.text.size(), f.delivery == Delivery::Reliable);
                }
                any = true;
            }
            for (auto& n : nodes) any = n.second->process() || any;
            if (!any) break;
        }
    }
};

struct Result {
    bool called = false;
    Status status = Status::Internal;
    std::string body;
};

inline DatagramEndpoint::ResponseHandler capture(Result& r) {
    return [&r](Status s, JsonVariantConst body) {
        r.called = true;
        r.status = s;
        r.body.clear();
        serializeJson(body, r.body);
    };
}

TEST(datagram_request_response) {
    cases::Device dev;
    Net net;
    DatagramEndpoint server(&dev.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    net.nodes = {{addr(1), &client}, {addr(2), &server}};

    Result r;
    tesser::Query q;
    q.keys = "on,brightness";
    CHECK(client.request(addr(2), tesser::Op::Get, "/lamp", std::string_view(), capture(r), q));
    net.run();
    CHECK(r.called);
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "{\"on\":false,\"brightness\":128}");

    Result w;
    CHECK(client.request(addr(2), tesser::Op::Set, "/lamp/brightness", std::string_view("77"), capture(w)));
    net.run();
    CHECK_EQ(w.status, Status::Ok);
    CHECK_EQ(dev.lamp.brightness, 77);

    Result e;
    CHECK(client.request(addr(2), tesser::Op::Set, "/lamp/brightness", std::string_view("777"), capture(e)));
    net.run();
    CHECK_EQ(e.status, Status::InvalidValue);
    CHECK(e.body.find("out of range") != std::string::npos);
    CHECK_EQ(client.pendingCalls(), 0u);
    CHECK_EQ(server.stats().requests, 3u);
}

TEST(datagram_unreliable_set_rejected) {
    cases::Device dev;
    Net net;
    DatagramEndpoint server(&dev.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    const char* req = "{\"id\":4,\"op\":\"set\",\"path\":\"/lamp/on\",\"body\":true}";
    server.receive(addr(1), req, strlen(req), false);
    server.process();
    CHECK_EQ(net.frames.size(), 1u);
    if (!net.frames.empty()) {
        CHECK(net.frames[0].text.find("\"status\":\"not_allowed\"") != std::string::npos);
        CHECK(net.frames[0].to == addr(1));
    }
    CHECK(!dev.lamp.on);

    // A broadcast get is fine.
    net.frames.clear();
    const char* get = "{\"id\":5,\"op\":\"get\",\"path\":\"/lamp/on\"}";
    server.receive(addr(1), get, strlen(get), false);
    server.process();
    CHECK_EQ(net.frames.size(), 1u);
    if (!net.frames.empty()) CHECK_EQ(net.frames[0].text, "{\"id\":5,\"status\":\"ok\",\"body\":false}");
}

// Request bodies nest no deeper than over HTTP, though the endpoint parses
// replies of any depth a node writes.
TEST(datagram_request_nesting) {
    tesser::Api api;
    int x = 0;
    api.object("a").object("b").object("c").value("x", x);
    api.config().maxDepth = 3;
    Net net;
    DatagramEndpoint server(&api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    const char* ok = "{\"id\":1,\"op\":\"set\",\"path\":\"/\",\"body\":{\"a\":{\"b\":{\"c\":{\"x\":1}}}}}";
    server.receive(addr(1), ok, strlen(ok), true);
    server.process();
    CHECK_EQ(x, 1);
    const char* deep = "{\"id\":2,\"op\":\"set\",\"path\":\"/\",\"body\":{\"a\":{\"b\":{\"c\":{\"x\":[2]}}}}}";
    server.receive(addr(1), deep, strlen(deep), true);
    server.process();
    CHECK_EQ(net.frames.size(), 2u);
    if (net.frames.size() == 2) CHECK(net.frames[1].text.find("envelope nested too deeply") != std::string::npos);
    CHECK_EQ(x, 1);
}

TEST(datagram_timeouts_and_forget) {
    Net net;
    net.dropAll = true;
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    Result r1, r2;
    CHECK(client.request(addr(9), tesser::Op::Get, "/", std::string_view(), capture(r1), tesser::Query(), 500));
    CHECK(client.request(addr(8), tesser::Op::Get, "/", std::string_view(), capture(r2), tesser::Query(), 5000));
    net.clock += 499;
    client.process();
    CHECK(!r1.called);
    net.clock += 1;
    client.process();
    CHECK(r1.called);
    CHECK_EQ(r1.status, Status::Timeout);
    CHECK(!r2.called);
    client.forgetPeer(addr(8));
    CHECK(!r2.called);  // takes effect in process(), never inside a link callback
    client.process();
    CHECK(r2.called);
    CHECK_EQ(r2.status, Status::Timeout);
    CHECK_EQ(client.stats().timeouts, 2u);
}

TEST(datagram_limits) {
    cases::Device dev;
    Net net;
    DatagramEndpoint::Options opts;
    opts.queueLimit = 2;
    opts.maxCalls = 1;
    DatagramEndpoint server(&dev.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn(), opts);
    const char* req = "{\"id\":1,\"op\":\"get\",\"path\":\"/lamp/on\"}";
    for (int i = 0; i < 3; i++) server.receive(addr(1), req, strlen(req), true);
    // The third was answered "busy" immediately, from receive().
    CHECK_EQ(net.frames.size(), 1u);
    if (!net.frames.empty()) CHECK(net.frames[0].text.find("\"status\":\"busy\"") != std::string::npos);
    server.process();
    CHECK_EQ(net.frames.size(), 3u);

    // Garbage and stray responses are counted, not answered.
    net.frames.clear();
    server.receive(addr(1), "nope", 4, true);
    const char* stray = "{\"id\":99,\"status\":\"ok\",\"body\":1}";
    server.receive(addr(1), stray, strlen(stray), true);
    server.process();
    CHECK_EQ(net.frames.size(), 0u);
    CHECK_EQ(server.stats().unmatched, 1u);

    Result r;
    CHECK(server.request(addr(3), tesser::Op::Get, "/", std::string_view(), capture(r)));
    CHECK(!server.request(addr(3), tesser::Op::Get, "/", std::string_view(), capture(r)));  // maxCalls
}

TEST(datagram_deferred) {
    tesser::Api api;
    tesser::Pending held;
    api.action("slow", [&](tesser::Call& call) { held = call.defer(); });
    Net net;
    DatagramEndpoint server(&api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    net.nodes = {{addr(1), &client}, {addr(2), &server}};
    Result r;
    CHECK(client.request(addr(2), tesser::Op::Set, "/slow", std::string_view(), capture(r)));
    net.run();
    CHECK(!r.called);
    held.replyWith([](tesser::JsonWriter& w) {
        w.beginObject();
        w.key("done");
        w.boolean(true);
        w.endObject();
    });
    net.run();
    CHECK(r.called);
    CHECK_EQ(r.body, "{\"done\":true}");
}

TEST(datagram_envelope_builder) {
    tesser::Query q;
    q.depth = 2;
    q.keys = "a,b";
    q.view = tesser::View::Schema;
    CHECK_EQ(tesser::buildRequestEnvelope(7, tesser::Op::Get, "/x", q, std::string_view()),
             "{\"id\":7,\"op\":\"get\",\"path\":\"/x\",\"depth\":2,\"keys\":\"a,b\",\"view\":\"schema\",\"v\":2}");
    CHECK_EQ(tesser::buildRequestEnvelope(8, tesser::Op::Set, "", tesser::Query(), "{\"a\":1}"),
             "{\"id\":8,\"op\":\"set\",\"path\":\"/\",\"body\":{\"a\":1}}");
}

TEST(datagram_queued_subscribe_keeps_subscriber) {
    // In queued mode a peer's first sub waits for poll() with the peer's
    // subscriber: it isn't deleted for having no subscription yet. A queued
    // sub that is refused leaves nothing behind once process() looks again.
    cases::Device dev;
    dev.api.config().queued = true;
    Net net;
    DatagramEndpoint server(&dev.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    net.nodes = {{addr(1), &client}, {addr(2), &server}};
    Result r;
    CHECK(client.request(addr(2), tesser::Op::Subscribe, "/lamp", std::string_view(), capture(r)));
    net.run();
    CHECK_EQ(dev.api.queuedRequests(), 1u);
    CHECK_EQ(server.peerSubscribers(), 1u);
    dev.api.poll();
    net.run();
    CHECK(r.called);
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(dev.api.subscriptionCount(), 1u);
    {
        tesser::Lock lock(dev.api);
        dev.lamp.brightness = 9;
    }
    dev.api.changed("/lamp/brightness");
    dev.api.poll();
    net.run();
    CHECK_EQ(server.peerSubscribers(), 1u);

    Result refused;
    DatagramEndpoint other(nullptr, tesser::TransportKind::NowTP, net.sender(addr(3)), net.clockFn());
    net.nodes.push_back({addr(3), &other});
    CHECK(other.request(addr(2), tesser::Op::Subscribe, "/nothing", std::string_view(), capture(refused)));
    net.run();
    CHECK_EQ(server.peerSubscribers(), 2u);
    dev.api.poll();
    net.run();
    CHECK_EQ(refused.status, Status::NotFound);
    CHECK_EQ(server.peerSubscribers(), 1u);
}

TEST(datagram_dropped_forward_answers) {
    // A forwarded request in flight when its remote node's endpoint is
    // destroyed: the call goes without its handlers running, yet the client
    // hears, and the pending slot comes back.
    cases::Device gw;
    Net net;
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    DatagramEndpoint server(&gw.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    auto* far = new DatagramEndpoint(
        nullptr, tesser::TransportKind::NowTP,
        [](const tesser::PeerAddress&, std::string, tesser::Delivery) { return true; }, net.clockFn());
    net.nodes = {{addr(1), &client}, {addr(2), &server}};
    {
        tesser::Lock lock(gw.api);
        gw.api.object("peers").remote("far", *far, addr(9));
    }
    Result r;
    CHECK(client.request(addr(2), tesser::Op::Get, "/peers/far/x", std::string_view(), capture(r)));
    net.run();
    CHECK_EQ(gw.api.pending(), 1);
    delete far;
    CHECK_EQ(gw.api.pending(), 0);
    net.run();
    CHECK(r.called);
    CHECK_EQ(r.status, Status::Internal);
}

TEST(datagram_forget_all) {
    // A link that stops forgets every peer it knows: heard from, called,
    // holding a subscription. Calls fail and each lost peer is reported.
    cases::Device dev;
    Net net;
    DatagramEndpoint server(&dev.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    net.nodes = {{addr(1), &client}, {addr(2), &server}};
    Result sub;
    CHECK(client.request(addr(2), tesser::Op::Subscribe, "/lamp", std::string_view(), capture(sub)));
    net.run();
    CHECK_EQ(dev.api.subscriptionCount(), 1u);
    std::vector<tesser::PeerLoss> lost;
    client.onPeerLost([&](const tesser::PeerAddress&, const tesser::PeerAddress&, tesser::PeerLoss why) { lost.push_back(why); });
    Result call;
    net.dropAll = true;
    CHECK(client.request(addr(2), tesser::Op::Get, "/lamp", std::string_view(), capture(call)));
    client.forgetAll(tesser::PeerLoss::Stopped);
    server.forgetAll(tesser::PeerLoss::Stopped);
    client.process();
    server.process();
    CHECK(call.called);
    CHECK(call.status != Status::Ok);
    CHECK_EQ(lost.size(), 1u);
    if (!lost.empty()) CHECK(lost[0] == tesser::PeerLoss::Stopped);
    CHECK_EQ(dev.api.subscriptionCount(), 0u);
    CHECK_EQ(server.peerSubscribers(), 0u);
}

TEST(peer_table_parts_last_apart) {
    using Table = tesser::detail::PeerTable;
    Table t(tesser::TransportKind::Udp);
    PeerAddress a = addr(1);
    // One record, whatever it was first known by.
    t.setAdvertised(a, 0x11, 2);
    CHECK_EQ(t.peers().size(), 1u);
    CHECK(t.known(a) == nullptr);
    t.learn(a, 100).maxMessage = 512;
    CHECK_EQ(t.peers().size(), 1u);
    CHECK(t.known(a) != nullptr && t.known(a)->online);
    CHECK_EQ(t.find(a)->schemaHash, 0x11u);
    CHECK_EQ(t.find(a)->device.length, 0u);  // only NowTP's addresses are devices
    t.present(a).token = "out";
    t.holdToken(a, "in", 8);
    std::string token;
    CHECK(t.heldToken(a, token));
    CHECK_EQ(token, "in");
    CHECK_EQ(t.presented(a)->token, "out");

    // Forgotten as heard, it is still what it advertised and its tokens.
    for (uint8_t i = 2; i < 34; i++) t.learn(addr(i), 200 + i);
    CHECK(t.known(a) == nullptr);
    CHECK(t.find(a) != nullptr);
    CHECK(t.find(a)->advertised);
    CHECK_EQ(t.find(a)->device.length, 0u);
    CHECK_EQ(t.find(a)->maxMessage, 0u);
    CHECK(t.presented(a) != nullptr);
    CHECK(t.heldToken(a, token));
    // The last part gone, the record goes.
    t.dropHeldToken(a);
    CHECK(!t.heldToken(a, token));
    CHECK(t.presented(a) != nullptr);
    t.stopPresenting(a);
    CHECK(t.presented(a) == nullptr);
    CHECK(t.find(a) != nullptr);  // advertised
    for (uint8_t i = 40; i < 72; i++) t.setAdvertised(addr(i), i, 2);
    CHECK(t.find(a) == nullptr);
    CHECK_EQ(t.peers().size(), 64u);  // 32 heard, 32 advertised

    // On NowTP, an address is the device.
    Table now(tesser::TransportKind::NowTP);
    CHECK(now.learn(a, 1).device == a);
}

TEST(peer_table_evicts_as_each_part_says) {
    using Table = tesser::detail::PeerTable;
    // Heard: an offline peer goes first, then the one silent longest.
    Table heard(tesser::TransportKind::Udp);
    for (uint8_t i = 0; i < 32; i++) heard.learn(addr(uint8_t(i + 1)), 1000u + i);
    heard.known(addr(9))->online = false;
    heard.learn(addr(100), 5000);
    CHECK(heard.known(addr(9)) == nullptr);
    CHECK(heard.known(addr(1)) != nullptr);
    heard.learn(addr(101), 5001);
    CHECK(heard.known(addr(1)) == nullptr);
    CHECK(heard.known(addr(2)) != nullptr);
    // Heard again: back online, what was deferred or reported cleared.
    heard.known(addr(2))->online = false;
    heard.known(addr(2))->lossPending = true;
    heard.known(addr(2))->authFailed = true;
    heard.learn(addr(2), 6000);
    CHECK(heard.known(addr(2))->online && !heard.known(addr(2))->lossPending && !heard.known(addr(2))->authFailed);
    CHECK_EQ(heard.known(addr(2))->heardMs, 6000u);

    // Advertised: the first to advertise goes, an update doesn't move it.
    Table adverts(tesser::TransportKind::Udp);
    for (uint8_t i = 0; i < 32; i++) adverts.setAdvertised(addr(uint8_t(i + 1)), i, 2);
    adverts.setAdvertised(addr(1), 77, 3);
    CHECK_EQ(adverts.find(addr(1))->schemaHash, 77u);
    adverts.setAdvertised(addr(100), 1, 2);
    CHECK(adverts.find(addr(1)) == nullptr);
    CHECK(adverts.find(addr(2)) != nullptr);

    // Tokens: the oldest goes; a new token from a peer makes it the newest.
    Table tokens(tesser::TransportKind::Udp);
    std::string t;
    tokens.holdToken(addr(1), "a", 2);
    tokens.holdToken(addr(2), "b", 2);
    tokens.holdToken(addr(1), "c", 2);
    tokens.holdToken(addr(3), "d", 2);
    CHECK(!tokens.heldToken(addr(2), t));
    CHECK(tokens.heldToken(addr(1), t));
    CHECK_EQ(t, "c");
    CHECK(tokens.heldToken(addr(3), t));
    // None kept with no room, and an empty one signs out.
    tokens.holdToken(addr(4), "e", 0);
    CHECK(!tokens.heldToken(addr(4), t));
    tokens.holdToken(addr(1), "", 2);
    CHECK(!tokens.heldToken(addr(1), t));
    CHECK(tokens.find(addr(1)) == nullptr);
}

}  // namespace datagram_cases

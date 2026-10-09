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
                Frame f = frames.front();
                frames.pop_front();
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
             "{\"id\":7,\"op\":\"get\",\"path\":\"/x\",\"depth\":2,\"keys\":\"a,b\",\"view\":\"schema\"}");
    CHECK_EQ(tesser::buildRequestEnvelope(8, tesser::Op::Set, "", tesser::Query(), "{\"a\":1}"),
             "{\"id\":8,\"op\":\"set\",\"path\":\"/\",\"body\":{\"a\":1}}");
}

}  // namespace datagram_cases

// A fragmented heap without PSRAM: large replies and notifications are
// refused, or put off, instead of aborting the device.
#pragma once

#include <string>

#include "core_cases.h"
#include "remote_cases.h"
#include "subscription_cases.h"
#include "udp_cases.h"

namespace memory_cases {

using tesser::Status;

inline size_t g_largest = SIZE_MAX;
inline size_t probe() { return g_largest; }
inline size_t g_free = SIZE_MAX;
inline size_t freeProbe() { return g_free; }

// A heap floor and the free heap the library sees; restored after.
struct Floor {
    size_t before = tesser::heapFloor();
    Floor(size_t floor, size_t free) {
        g_free = free;
        tesser::setFreeHeapProbe(&freeProbe);
        tesser::setHeapFloor(floor);
    }
    ~Floor() {
        tesser::setFreeHeapProbe(nullptr);
        tesser::setHeapFloor(before);  // the device's own, on target
    }
    void free(size_t bytes) { g_free = bytes; }
};

// Sets the largest free block the library sees, and restores the platform's.
struct Heap {
    explicit Heap(size_t largest) {
        g_largest = largest;
        tesser::setLargestFreeBlockProbe(&probe);
    }
    ~Heap() { tesser::setLargestFreeBlockProbe(nullptr); }
    void set(size_t largest) { g_largest = largest; }
};

TEST(memory_reply_refused_when_heap_is_short) {
    udp_cases::Pair p;
    std::string big(3000, 'x');
    p.dev.api.value("big", big);
    Heap heap(10000);
    // Room for the reply (grown by half again at a time), a copy and
    // headroom: it goes.
    udp_cases::Result r = p.call(tesser::Op::Get, "/big");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body.size(), 3002u);
    // Not enough: "busy", not an abort, and small replies still go.
    heap.set(5000);
    r = p.call(tesser::Op::Get, "/big");
    CHECK_EQ(r.status, Status::Busy);
    CHECK_EQ(r.body, "{\"error\":\"busy\",\"reason\":\"low_memory\",\"path\":\"/big\",\"message\":\"not enough memory for the response\"}");
    CHECK_EQ(p.call(tesser::Op::Get, "/lamp/on").body, "false");
    // Replies under 1 KB aren't checked at all.
    heap.set(0);
    CHECK_EQ(p.call(tesser::Op::Get, "/lamp/brightness").body, "128");
}

TEST(memory_notification_waits_for_room) {
    cases::Device d;
    std::string big(3000, 'x');
    auto& bigNode = d.api.value("big", big);
    subscription_cases::FakeSubscriber sub;
    CHECK_EQ(subscription_cases::subscribe(d.api, &sub, "/", cases::keys("big,lamp")).status, Status::Ok);
    Heap heap(5000);
    big.assign(3000, 'y');
    bigNode.changed();
    d.api.poll(100);
    CHECK(sub.messages.empty());  // put off
    d.lamp.brightness = 7;
    d.api.changed("/lamp/brightness");
    d.api.poll(200);
    CHECK(sub.messages.empty());
    // Room again: one notification with both changes.
    heap.set(SIZE_MAX);
    d.api.poll(300);
    CHECK_EQ(sub.messages.size(), 1u);
    CHECK(sub.last().find("yyyy") != std::string::npos);
    CHECK(sub.last().find("\"overflow\"") == std::string::npos);
}

TEST(memory_notification_backs_off) {
    // A notification the heap can't hold isn't built again at every poll()
    // (every 10 ms on target) while it can't go: the wait doubles, from
    // 50 ms to 2 s. Measured on DEVKIT-R1 before this: a 5-6 KB change
    // notification rebuilt and refused every 10 ms.
    cases::Device d;
    std::string big(3000, 'x');
    auto& bigNode = d.api.value("big", big);
    subscription_cases::FakeSubscriber sub;
    CHECK_EQ(subscription_cases::subscribe(d.api, &sub, "/", cases::keys("big")).status, Status::Ok);
    uint32_t tries = 0;
    {
        Floor floor(8000, 9000);
        big.assign(3000, 'y');
        bigNode.changed();
        uint32_t before = tesser::memoryBudget().refused();
        for (uint32_t t = 1000; t <= 11000; t += 10) d.api.poll(t);
        tries = tesser::memoryBudget().refused() - before;
        CHECK(sub.messages.empty());
    }
    // 1000, 1050, 1150, 1350, 1750, 2550, then every 2 s: 10, not 1001.
    CHECK(tries >= 8 && tries <= 12);
    // Room again: it goes within the longest wait.
    for (uint32_t t = 11010; t <= 13010 && sub.messages.empty(); t += 10) d.api.poll(t);
    CHECK_EQ(sub.messages.size(), 1u);
    CHECK(sub.last().find("yyyy") != std::string::npos);
    // Sent, the next change goes at once.
    big.assign(3000, 'z');
    bigNode.changed();
    d.api.poll(13020);
    CHECK_EQ(sub.messages.size(), 2u);
}

TEST(memory_json_member) {
    std::string_view out;
    CHECK(tesser::jsonMember("{\"id\":3,\"status\":\"ok\",\"body\":{\"a\":[1,{\"b\":\"}\"}]}}", "body", out));
    CHECK_EQ(std::string(out), "{\"a\":[1,{\"b\":\"}\"}]}");
    // Spaced out (another implementation), the body not last, escapes.
    CHECK(tesser::jsonMember("{ \"body\" : \"x\\\"y,}\" , \"id\" : 1 }", "body", out));
    CHECK_EQ(std::string(out), "\"x\\\"y,}\"");
    CHECK(tesser::jsonMember("{\"status\":\"ok\",\"body\":-12.5e3}", "body", out));
    CHECK_EQ(std::string(out), "-12.5e3");
    CHECK(tesser::jsonMember("{\"body\":null}", "body", out));
    CHECK_EQ(std::string(out), "null");
    CHECK(!tesser::jsonMember("{\"id\":1,\"status\":\"ok\"}", "body", out));
    CHECK(!tesser::jsonMember("{}", "body", out));
    CHECK(!tesser::jsonMember("[1]", "body", out));
    // A key that only looks like it, inside a value, doesn't count.
    CHECK(tesser::jsonMember("{\"x\":{\"body\":1},\"body\":2}", "body", out));
    CHECK_EQ(std::string(out), "2");
}

TEST(memory_gateway_relays_bodies_raw) {
    // An "ok" body goes from the remote to the client as the remote wrote
    // it; errors still map their paths.
    remote_cases::Rig r;
    std::string big = "q\"uo\\te\u00e9 " + std::string(2000, 'z');
    r.device.api.value("big", big);
    std::string reply = r.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/kitchen/big\"}");
    std::string direct = cases::get(r.device.api, "/big").body;
    CHECK_EQ(reply, "{\"id\":1,\"status\":\"ok\",\"body\":" + direct + "}");
    CHECK(r.ask("{\"id\":2,\"op\":\"get\",\"path\":\"/kitchen/nope\"}").find("\"path\":\"/kitchen/nope\"") !=
          std::string::npos);
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(memory_gateway_relay_refused_when_heap_is_short) {
    // The device answers; then the gateway's heap is short: its client
    // gets "busy" for the relayed reply, and the gateway lives on.
    remote_cases::Rig r;
    std::string big(3000, 'x');
    r.device.api.value("big", big);
    auto deliver = [&] {
        while (!r.net.frames.empty()) {
            auto f = r.net.frames.front();
            r.net.frames.pop_front();
            for (auto& n : r.net.nodes) {
                if (n.first == f.to) {
                    n.second->receive(f.from, f.text.data(), f.text.size(), f.delivery == tesser::Delivery::Reliable);
                }
            }
        }
    };
    r.lines.out.clear();
    std::string line = "{\"id\":5,\"op\":\"get\",\"path\":\"/kitchen/big\"}\n";
    r.line.feed(line.data(), line.size());  // forwarded
    deliver();
    r.deviceEnd.process();  // the device answers
    Heap heap(5000);
    deliver();
    r.gatewayEnd.process();  // the relay
    CHECK_EQ(r.lines.out.size(), 1u);
    if (!r.lines.out.empty()) {
        CHECK_EQ(r.lines.out[0], "{\"id\":5,\"status\":\"busy\",\"body\":{\"error\":\"busy\",\"reason\":\"low_memory\",\"path\":\"/kitchen\","
                                 "\"message\":\"not enough memory for the response\"}}");
    }
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(memory_heap_floor) {
    // Under the floor, a reply past 256 bytes is refused even though some
    // block could hold it: the heap as a whole is short. Errors still go.
    udp_cases::Pair p;
    std::string mid(600, 'm');
    p.dev.api.value("mid", mid);
    Floor floor(8000, 8500);
    udp_cases::Result r = p.call(tesser::Op::Get, "/mid");
    CHECK_EQ(r.status, Status::Busy);
    CHECK(r.body.find("not enough memory") != std::string::npos);
    CHECK_EQ(p.call(tesser::Op::Get, "/nope").status, Status::NotFound);
    CHECK_EQ(p.call(tesser::Op::Get, "/lamp/on").body, "false");
    floor.free(20000);
    CHECK_EQ(p.call(tesser::Op::Get, "/mid").status, Status::Ok);
}

TEST(memory_remote_subscription_admission) {
    // A forwarded subscription that needs its own copy of the remote's
    // state wants twice the floor free; one the shared copy covers doesn't.
    remote_cases::Rig r;
    Floor floor(8000, 12000);
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"sub\",\"path\":\"/kitchen/lamp\"}"),
             "{\"id\":1,\"status\":\"busy\",\"body\":{\"error\":\"busy\",\"reason\":\"low_memory\",\"path\":\"/kitchen/lamp\","
             "\"message\":\"not enough memory for another remote subscription\"}}");
    CHECK_EQ(r.device.api.subscriptionCount(), 0u);
    floor.free(20000);
    CHECK(r.ask("{\"id\":2,\"op\":\"sub\",\"path\":\"/kitchen/lamp\"}").find("\"status\":\"ok\"") != std::string::npos);
    floor.free(12000);
    subscription_cases::FakeSubscriber covered;
    CHECK_EQ(subscription_cases::subscribe(r.gateway, &covered, "/kitchen/lamp", cases::keys("on")).body,
             "{\"on\":false}");
    r.gateway.dropSubscriber(&covered);
}

TEST(memory_large_receive_dropped_under_floor) {
    // A link's lent buffer (NowTP) isn't copied when the heap can't spare
    // it above the floor: a request is answered busy at once, anything else
    // is lost, as datagrams can be. The reason says it was the heap, not
    // the endpoint's queue.
    cases::Device dev;
    datagram_cases::Net net;
    tesser::DatagramEndpoint ep(&dev.api, tesser::TransportKind::NowTP, net.sender(datagram_cases::addr(2)),
                                net.clockFn());
    std::string big = "{\"id\":1,\"op\":\"get\",\"path\":\"/lamp\",\"pad\":\"" + std::string(600, 'p') + "\"}";
    Floor floor(8000, 8500);
    ep.receive(datagram_cases::addr(1), big.data(), big.size(), true);
    CHECK_EQ(ep.stats().dropped, 1u);
    CHECK_EQ(net.frames.size(), 1u);
    if (!net.frames.empty()) CHECK(net.frames[0].text.find("\"reason\":\"low_memory\"") != std::string::npos);
    net.frames.clear();
    std::string small = "{\"id\":2,\"op\":\"get\",\"path\":\"/lamp/on\"}";
    ep.receive(datagram_cases::addr(1), small.data(), small.size(), true);
    ep.process();
    CHECK_EQ(ep.stats().requests, 1u);
    floor.free(20000);
    ep.receive(datagram_cases::addr(1), big.data(), big.size(), true);
    ep.process();
    CHECK_EQ(ep.stats().requests, 2u);
}

TEST(memory_receive_refused_to_a_link_that_can) {
    // A link that can refuse a message (NowTP's Message::refused) is told to,
    // for a reliable one the endpoint can't take: nothing answers it, and
    // the sender sends it again. A request answered busy, or a reply failing
    // its call, would lose what a retry gets.
    cases::Device dev;
    datagram_cases::Net net;
    tesser::DatagramEndpoint ep(&dev.api, tesser::TransportKind::NowTP, net.sender(datagram_cases::addr(2)),
                                net.clockFn());
    ep.linkRefuses(true);
    std::string big = "{\"id\":1,\"op\":\"get\",\"path\":\"/lamp\",\"pad\":\"" + std::string(600, 'p') + "\"}";
    {
        Floor floor(8000, 8500);
        CHECK(!ep.receive(datagram_cases::addr(1), big.data(), big.size(), true));
        CHECK(net.frames.empty());
        CHECK_EQ(ep.stats().dropped, 1u);
        // Unreliable (or broadcast): the link can't refuse it; answered as before.
        CHECK(ep.receive(datagram_cases::addr(1), big.data(), big.size(), false));
        CHECK_EQ(net.frames.size(), 1u);
        net.frames.clear();
        // A buffer handed over costs nothing more: taken under the floor.
        char* owned = static_cast<char*>(malloc(big.size()));
        memcpy(owned, big.data(), big.size());
        CHECK(ep.receiveOwned(datagram_cases::addr(1), owned, big.size(), true));
    }
    // Sent again with room: taken.
    CHECK(ep.receive(datagram_cases::addr(1), big.data(), big.size(), true));
    ep.process();
    CHECK_EQ(ep.stats().requests, 2u);
    // A full queue is refused the same way.
    tesser::DatagramEndpoint::Options small;
    small.queueLimit = 1;
    tesser::DatagramEndpoint narrow(&dev.api, tesser::TransportKind::NowTP, net.sender(datagram_cases::addr(2)),
                                    net.clockFn(), small);
    narrow.linkRefuses(true);
    std::string get = "{\"id\":2,\"op\":\"get\",\"path\":\"/lamp/on\"}";
    CHECK(narrow.receive(datagram_cases::addr(1), get.data(), get.size(), true));
    CHECK(!narrow.receive(datagram_cases::addr(1), get.data(), get.size(), true));
    net.frames.clear();
    narrow.process();
    CHECK_EQ(narrow.stats().requests, 1u);
}

TEST(memory_remote_state_needs_room) {
    // Admitted while there was room, the remote's state then finds none:
    // the waiting subscriber hears "busy", and nothing is copied.
    remote_cases::Rig r;
    auto deliver = [&] {
        while (!r.net.frames.empty()) {
            auto f = r.net.frames.front();
            r.net.frames.pop_front();
            for (auto& n : r.net.nodes) {
                if (n.first == f.to) {
                    n.second->receive(f.from, f.text.data(), f.text.size(), f.delivery == tesser::Delivery::Reliable);
                }
            }
        }
    };
    Floor floor(8000, 20000);
    r.lines.out.clear();
    std::string line = "{\"id\":7,\"op\":\"sub\",\"path\":\"/kitchen/lamp\"}\n";
    r.line.feed(line.data(), line.size());
    deliver();
    r.deviceEnd.process();
    floor.free(8050);
    deliver();
    r.gatewayEnd.process();
    CHECK_EQ(r.lines.out.size(), 1u);
    if (!r.lines.out.empty()) {
        CHECK(r.lines.out[0].find("\"status\":\"busy\"") != std::string::npos);
        CHECK(r.lines.out[0].find("not enough memory for the remote's state") != std::string::npos);
    }
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(memory_receive_owned_buffer) {
    // A link hands its malloc'd reassembly buffer over: answered like any
    // message, freed by the endpoint (ASan checks), also when it is dropped.
    cases::Device dev;
    datagram_cases::Net net;
    tesser::DatagramEndpoint::Options o;
    o.queueLimit = 1;
    tesser::DatagramEndpoint server(&dev.api, tesser::TransportKind::NowTP, net.sender(datagram_cases::addr(2)),
                                    net.clockFn(), o);
    tesser::DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(datagram_cases::addr(1)),
                                    net.clockFn());
    net.nodes = {{datagram_cases::addr(1), &client}, {datagram_cases::addr(2), &server}};
    datagram_cases::Result r;
    client.request(datagram_cases::addr(2), tesser::Op::Get, "/lamp/brightness", "", datagram_cases::capture(r));
    // Taken off the network by hand, as a link would, and handed over.
    CHECK_EQ(net.frames.size(), 1u);
    std::string text = net.frames.front().text;
    net.frames.pop_front();
    auto handOver = [&](const std::string& t) {
        char* buffer = static_cast<char*>(malloc(t.size()));
        memcpy(buffer, t.data(), t.size());
        server.receiveOwned(datagram_cases::addr(1), buffer, t.size(), true);
    };
    handOver(text);
    handOver("{\"id\":99,\"op\":\"get\",\"path\":\"/lamp/on\"}");  // the queue is full: dropped, and freed
    CHECK_EQ(server.stats().dropped, 1u);
    net.run();
    CHECK(r.called);
    CHECK_EQ(r.body, "128");
}

}  // namespace memory_cases

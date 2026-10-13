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
    CHECK_EQ(r.body, "{\"error\":\"busy\",\"path\":\"/big\",\"message\":\"not enough memory for the response\"}");
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
        CHECK_EQ(r.lines.out[0], "{\"id\":5,\"status\":\"busy\",\"body\":{\"error\":\"busy\",\"path\":\"/kitchen\","
                                 "\"message\":\"not enough memory for the response\"}}");
    }
    CHECK_EQ(r.gateway.pending(), 0);
}

}  // namespace memory_cases

// A fragmented heap without PSRAM: large replies and notifications are
// refused, or put off, instead of aborting the device.
#pragma once

#include <string>

#include "core_cases.h"
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

}  // namespace memory_cases

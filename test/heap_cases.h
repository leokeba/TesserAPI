// Client-reachable paths on a heap with a size (tesser/testing/heap_limit.h):
// each scenario runs with the heap limited to every size from what it holds
// to 24 KB more, the library's free-heap probes reading what is left, and a
// floor of 4 KB (half the target's, for what the network stack takes, which
// isn't modeled). A C++ allocation past the limit aborts, as on ESP-IDF; a
// path that checks its room answers busy instead. So these tests pass when
// nothing a client can send makes the library allocate unchecked past what
// the floor leaves.
#pragma once

#include <stdio.h>
#include <stdlib.h>

#include <functional>
#include <string>

#include "relay_cases.h"
#include "remote_cases.h"
#include "subscription_cases.h"
#include "tesser/testing/heap_limit.h"

#if defined(__APPLE__) || defined(__GLIBC__)
#include <execinfo.h>
#define TESSER_HEAP_BACKTRACE 1
#endif

namespace heap_cases {

using tesser::Status;
using tesser::testing::HeapExempt;
using tesser::testing::HeapLimit;

constexpr size_t kFloor = 2048;

// TESSER_HEAP_DISCOVER=1 prints where each allocation past the limit came
// from, and lets it go, instead of aborting at the first.
inline size_t g_above = 0;  // the sweep's step, for reports
inline void report(size_t bytes, size_t used, size_t limit) {
    fprintf(stderr, "heap_limit: new(%zu) past the limit: %zu held of %zu (at %zu above; free %zu, floor %zu, unsettled %zu)\n",
            bytes, used, limit, g_above, tesser::freeHeap(), tesser::heapFloor(), tesser::unsettledBytes());
#if defined(TESSER_HEAP_BACKTRACE)
    void* frames[24];
    int n = backtrace(frames, 24);
    backtrace_symbols_fd(frames, n, 2);
#endif
}

// The heap limited to `above` more than is held, as the library sees it.
struct Tight {
    size_t floorBefore = tesser::heapFloor();
    HeapLimit limit;
    Tight(size_t above, size_t floor) : limit(above) {
        tesser::setFreeHeapProbe(&tesser::testing::heapLeft);
        tesser::setLargestFreeBlockProbe(&tesser::testing::heapLeft);
        tesser::setHeapFloor(floor);
        // A checked allocation over 256 bytes leaves the floor free; give or
        // take what the allocator rounds up (libc++ asks 264 bytes for a
        // 250-byte string), as the target's heap has block headers too.
        tesser::testing::setHeapFloorRule(floor - 64, 320);
        const char* d = getenv("TESSER_HEAP_DISCOVER");
        if (d && *d == '1') tesser::testing::onExceeded(&report);
    }
    ~Tight() {
        tesser::testing::onExceeded(nullptr);
        tesser::testing::setHeapFloorRule(0);
        tesser::setFreeHeapProbe(nullptr);
        tesser::setLargestFreeBlockProbe(nullptr);
        tesser::setHeapFloor(floorBefore);
    }
};

// Runs `scenario` with a fresh `T` (built before the limit) at every heap
// size from the floor to 24 KB more than what it holds: a device starts with
// at least its floor free, which is what small allocations, never refused,
// live on. `scenario` says whether everything went through: not at the
// smallest heap (so the refusals were exercised), and at the largest.
template <typename T>
void sweep(const std::function<bool(T&)>& scenario, size_t floor = kFloor, size_t step = 128) {
    bool first = true, last = false;
    size_t start = floor;
    for (size_t above = start; above <= floor + 24 * 1024; above += step) {
        T t;
        g_above = above;
        bool done;
        {
            Tight tight(above, floor);
            done = scenario(t);
        }
        if (above == start) first = done;
        last = done;
    }
    CHECK(!first);
    CHECK(last);
}

TEST(heap_limit_counts_and_limits) {
    size_t before = tesser::testing::heapUsed();
    {
        std::string s(5000, 'x');
        CHECK(tesser::testing::heapUsed() >= before + 5000);
    }
    CHECK_EQ(tesser::testing::heapUsed(), before);
    static size_t seen = 0;
    tesser::testing::onExceeded([](size_t bytes, size_t, size_t) { seen = bytes; });
    {
        HeapLimit limit(1000);
        std::string s(5000, 'x');
    }
    tesser::testing::onExceeded(nullptr);
    CHECK(seen >= 5000);
}

// Uploads and downloads through a gateway, relayed to its device, over a
// serial line, as a client would. The gateway and the device share this
// heap, so both nodes' bookkeeping during a relay (call slots, callbacks,
// the transfer's state: each under 256 bytes, never refused) lives on the
// one floor: twice the floor of one node.
TEST(heap_relayed_transfers) {
    sweep<relay_cases::Rig>([](relay_cases::Rig& r) {
        r.device.api.config().maxResponse = 4096;
        r.device.api.config().maxRequestBody = 4096;
        bool up = r.uploadAll("/dev/big", r.big, 2880).find("\"status\":\"ok\"") != std::string::npos;
        bool down = r.downloadAll("/dev/big", 100000) == r.big;
        bool config = r.downloadAll("/dev/sys/config", 100000) == "{\"level\":3}";
        tesser::testing::HeapExempt test;
        return up && r.got == r.big && down && config;
    }, 2 * kFloor);
}

// Change notifications and snapshots of a large value.
struct Subs {
    cases::Device d;
    std::string big = std::string(6000, 'x');
    tesser::ValueNode* node = nullptr;
    subscription_cases::FakeSubscriber sub;
    Subs() { node = &d.api.value("big", big); }
};

TEST(heap_subscriptions) {
    sweep<Subs>([](Subs& s) {
        bool snapshot = subscription_cases::subscribe(s.d.api, &s.sub, "/").status == Status::Ok;
        for (uint32_t t = 0; t < 5; t++) {
            s.big[t] = 'y';
            s.node->changed();
            s.d.api.poll(1000 + t * 3000);
        }
        return snapshot && s.sub.messages.size() == 5;
    });
}

// Events and change notifications on a heap taken by something else (the
// network stack in a burst): nothing left above the floor, or almost
// nothing at all. They wait rather than abort.
TEST(heap_notifications_on_an_exhausted_heap) {
    for (size_t above : {size_t(0), size_t(64), size_t(512), kFloor - 1}) {
        cases::Device d;
        tesser::EventNode& pressed = d.api.object("button").event("pressed");
        pressed.emit();  // finds its path (a string), once, as at boot
        subscription_cases::FakeSubscriber sub;
        subscription_cases::subscribe(d.api, &sub, "/");
        d.lamp.brightness = 7;
        d.api.changed("/lamp/brightness");
        {
            Tight tight(above, kFloor);
            pressed.emit();
            d.api.poll(1000);
        }
        CHECK(sub.messages.empty());
        CHECK(sub.missed.load());
        d.api.poll(2000);
        CHECK_EQ(sub.messages.size(), 1u);
    }
}

// A client of a gateway subscribed below a remote node: the gateway
// forwards the subscription and relays the device's changes, holding a
// copy of its state. Two nodes on this heap, as for the relay.
struct Gateway : remote_cases::Rig {
    std::string big = std::string(3000, 'x');
    tesser::ValueNode* node = nullptr;
    Gateway() { node = &device.api.value("big", big); }
};

TEST(heap_forwarded_subscriptions) {
    sweep<Gateway>([](Gateway& g) {
        bool sub = g.ask("{\"id\":1,\"op\":\"sub\",\"path\":\"/kitchen\",\"keys\":\"big,lamp\"}").find("\"ok\"") !=
                   std::string::npos;
        size_t changes = 0;
        for (uint32_t t = 0; t < 5; t++) {
            g.big[t] = 'y';
            g.node->changed();
            {
                HeapExempt test;
                g.lines.out.clear();
            }
            g.device.api.poll(1000 + t * 3000);
            g.net.run();
            g.gateway.poll(1000 + t * 3000);
            HeapExempt test;
            for (const std::string& l : g.lines.out) changes += l.find("\"op\":\"change\"") != std::string::npos;
        }
        return sub && changes == 5;
    }, 2 * kFloor);
}

// Envelopes over a serial line: reads of the whole tree, its schema and a
// 6 KB value, and a 3.5 KB write.
struct Lines {
    cases::Device d;
    int ok = 0;
    std::string big = std::string(6000, 'x');
    cases::Lines out;
    tesser::LineTransport line{d.api, [this](const char* p, size_t n) { out.add(p, n); }};
    Lines() { d.api.value("big", big); }
    void ask(const std::string& envelope) {
        std::string l;
        {
            HeapExempt client;
            l = envelope + "\n";
        }
        size_t before = out.out.size();
        line.feed(l.data(), l.size());
        HeapExempt client;
        if (out.out.size() > before && out.out.back().find("\"status\":\"ok\"") != std::string::npos) ok++;
    }
};

TEST(heap_envelopes) {
    sweep<Lines>([](Lines& l) {
        std::string set;
        {
            HeapExempt client;
            set = "{\"id\":3,\"op\":\"set\",\"path\":\"/big\",\"body\":\"" + std::string(3500, 'z') + "\"}";
        }
        l.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/\"}");
        l.ask("{\"id\":2,\"op\":\"get\",\"path\":\"/\",\"view\":\"schema\"}");
        l.ask(set);
        l.ask("{\"id\":4,\"op\":\"get\",\"path\":\"/big\"}");
        return l.ok == 4;
    });
}

}  // namespace heap_cases

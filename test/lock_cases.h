// The API lock and I/O (docs/DESIGN.md section 9): replies are sent without
// it, slow work runs off it, and lock ranks are checked.
#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "core_cases.h"
#include "subscription_cases.h"

namespace lock_cases {

using tesser::Api;
using tesser::Status;

inline std::atomic<int> g_violations{0};
inline void countViolation(const char*, const char*) { g_violations++; }

TEST(lock_ranks_checked) {
#if defined(TESSER_LOCK_CHECK)
    tesser::setLockOrderHandler(&countViolation);
    g_violations = 0;
    tesser::Mutex api(tesser::kRankApi, "test api");
    tesser::Mutex remote(tesser::kRankRemote, "test remote");
    tesser::Mutex plain;  // unranked: never checked
    {
        tesser::MutexGuard a(api);
        tesser::MutexGuard r(remote);
        tesser::MutexGuard again(api);  // recursive: fine
        tesser::MutexGuard p(plain);
    }
    CHECK_EQ(g_violations.load(), 0);
    {
        tesser::MutexGuard r(remote);
        tesser::MutexGuard a(api);  // the API lock after a remote node's
    }
    CHECK_EQ(g_violations.load(), 1);
    tesser::setLockOrderHandler(nullptr);
#endif
    // Released only when held once: an outer holder still counts on it.
    tesser::Mutex m(tesser::kRankApi, "test release");
    m.lock();
    {
        tesser::MutexRelease r(m);
        CHECK(r.released());
        CHECK(!m.heldByMe());
    }
    CHECK_EQ(m.depth(), 1);
    m.lock();
    {
        tesser::MutexRelease r(m);
        CHECK(!r.released());
        CHECK_EQ(m.depth(), 2);
    }
    m.unlock();
    m.unlock();
    CHECK_EQ(m.depth(), 0);
}

// A tree of many objects: a reply long enough to go out in pieces.
struct Wide {
    Api api;
    std::vector<int> values = std::vector<int>(120, 7);
    Wide() {
        for (size_t i = 0; i < values.size(); i++) {
            names.push_back("node" + std::to_string(i));
        }
        for (size_t i = 0; i < values.size(); i++) {
            api.object(names[i].c_str()).value("value", values[i]).doc("a value to make the reply long");
        }
        api.value("x", x);
    }
    std::vector<std::string> names;
    int x = 0;
};

TEST(lock_released_while_sending) {
    // A line's reply goes out between an object's children without the API
    // lock: another task isn't held up by a slow line.
    Wide w;
    std::vector<bool> lockedDuring;
    std::string out;
    tesser::LineTransport line(w.api, [&](const char* d, size_t n) {
        lockedDuring.push_back(w.api.mutex().heldByMe());
        out.append(d, n);
    });
    std::string req = "{\"id\":1,\"op\":\"get\",\"path\":\"/\",\"view\":\"schema\"}\n";
    line.feed(req.data(), req.size());
    CHECK(out.size() > 8000);
    CHECK(lockedDuring.size() > 3);
    size_t unlocked = 0;
    for (bool b : lockedDuring) unlocked += !b;
    CHECK_EQ(unlocked, lockedDuring.size());
    JsonDocument doc;
    CHECK(!deserializeJson(doc, out));
}

TEST(lock_notification_waits_for_the_line) {
    // A notification that comes while a reply streams lands after it, never
    // inside the line.
    Wide w;
    std::string out;
    bool once = true;
    std::atomic<bool> blocked{false};
    tesser::LineTransport line(w.api, [&](const char* d, size_t n) {
        out.append(d, n);
        if (once && out.size() > 2000) {
            once = false;
            // Another task, meanwhile: a change and its notification. (It
            // gives up after a second rather than hang if the line held the
            // API lock.)
            std::thread([&] {
                // 10 ms: shorter sleeps busy-wait on ESP-IDF.
                for (int i = 0; i < 100 && !w.api.mutex().tryLock(); i++) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (!w.api.mutex().heldByMe()) {
                    blocked = true;
                    return;
                }
                w.x = 5;
                w.api.changed("/x");
                w.api.poll(1000);
                w.api.mutex().unlock();
            }).join();
        }
    });
    std::string sub = "{\"id\":1,\"op\":\"sub\",\"path\":\"/x\"}\n";
    line.feed(sub.data(), sub.size());
    out.clear();
    std::string req = "{\"id\":2,\"op\":\"get\",\"path\":\"/\",\"view\":\"schema\"}\n";
    line.feed(req.data(), req.size());
    CHECK(!blocked.load());
    size_t nl = out.find('\n');
    CHECK(nl != std::string::npos);
    if (nl != std::string::npos) {
        JsonDocument doc;
        CHECK(!deserializeJson(doc, out.substr(0, nl)));
        CHECK_EQ(out.substr(nl + 1), "{\"op\":\"change\",\"path\":\"/x\",\"body\":5}\n");
    }
}

TEST(lock_events_of_two_apis) {
    // An event's first emit() finds its API without taking another's lock:
    // each thread holds its own API and emits its own event, at once.
#if defined(TESSER_LOCK_CHECK)
    tesser::setLockOrderHandler(&countViolation);
    g_violations = 0;
#endif
    Api a, b;
    tesser::EventNode& ea = a.object("x").event("ping");
    tesser::EventNode& eb = b.object("y").event("pong");
    subscription_cases::FakeSubscriber sa, sb;
    subscription_cases::subscribe(a, &sa, "/");
    subscription_cases::subscribe(b, &sb, "/");
    std::atomic<int> ready{0};
    auto run = [&ready](Api& api, tesser::EventNode& ev) {
        tesser::Lock lock(api);
        ready++;
        // Sleeps, not yields: a yielding thread above the test task's
        // priority would starve it on target before it starts the other.
        while (ready < 2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ev.emit(1);
    };
    std::thread ta(run, std::ref(a), std::ref(ea));
    std::thread tb(run, std::ref(b), std::ref(eb));
    ta.join();
    tb.join();
    CHECK_EQ(sa.last(), "{\"op\":\"event\",\"path\":\"/x/ping\",\"body\":1}");
    CHECK_EQ(sb.last(), "{\"op\":\"event\",\"path\":\"/y/pong\",\"body\":1}");
#if defined(TESSER_LOCK_CHECK)
    CHECK_EQ(g_violations.load(), 0);
    tesser::setLockOrderHandler(nullptr);
#endif
    a.dropSubscriber(&sa);
    b.dropSubscriber(&sb);
}

TEST(lock_background_work) {
    // A slow action defers, and its work runs on the background task without
    // the API lock: requests go on meanwhile.
    Api api;
    int x = 1;
    api.value("x", x);
    std::atomic<bool> release{false};
    std::atomic<bool> lockFree{false};
    api.action("slow", [&](tesser::Call& call) {
        tesser::Pending p = call.defer();
        CHECK(tesser::background([&api, &release, &lockFree, p]() mutable {
            lockFree = !api.mutex().heldByMe();
            while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            p.reply(42);
        }));
    });
    tesser::LocalClient c(api);
    std::string got;
    c.set("/slow", "null", [&](Status s, JsonVariantConst v) {
        got = std::string(tesser::toString(s)) + v.as<std::string>();
    });
    CHECK_EQ(cases::get(api, "/x").body, "1");  // the API isn't held
    release = true;
    for (int i = 0; i < 300 && got.empty(); i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        c.process();
    }
    CHECK_EQ(got, "ok42");
    CHECK(lockFree.load());
    CHECK(!tesser::background(nullptr));
}

}  // namespace lock_cases

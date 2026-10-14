// Change tracking, subscriptions and events.
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"
#include "datagram_cases.h"

namespace subscription_cases {

using cases::Resp;
using tesser::Api;
using tesser::Op;
using tesser::Query;
using tesser::Status;

struct FakeSubscriber : tesser::Subscriber {
    std::vector<std::string> messages;
    int failNext = 0;
    bool notify(const std::string& m, tesser::Delivery) override {
        if (failNext > 0) {
            failNext--;
            return false;
        }
        messages.push_back(m);
        return true;
    }
    std::string last() const { return messages.empty() ? "" : messages.back(); }
};

// As an admin client, unless `level` says otherwise.
inline Resp request(Api& api, tesser::Subscriber* sub, Op op, const char* path, Query q = Query(),
                    tesser::Access level = tesser::Access::Admin) {
    tesser::Request req;
    req.op = op;
    req.path = path;
    req.query = q;
    req.subscriber = sub;
    req.client.access = level;
    req.client.authenticated = level != tesser::Access::Public;
    tesser::StringReply reply;
    api.handle(req, reply);
    return {reply.status, reply.body};
}

inline Resp subscribe(Api& api, tesser::Subscriber* sub, const char* path, Query q = Query(),
                      tesser::Access level = tesser::Access::Admin) {
    return request(api, sub, Op::Subscribe, path, q, level);
}

TEST(sub_without_snapshot) {
    cases::Device d;
    FakeSubscriber s;
    Query q;
    q.snapshot = false;
    Resp r = subscribe(d.api, &s, "/lamp", q);
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "null");
    cases::set(d.api, "/lamp/on", "true");
    d.api.poll(1000);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"on\":true}}");
    q.interval = 5;
    CHECK_EQ(tesser::buildRequestEnvelope(3, Op::Subscribe, "/lamp", q, std::string_view()),
             "{\"id\":3,\"op\":\"sub\",\"path\":\"/lamp\",\"interval\":5,\"snapshot\":false}");
}

TEST(sub_snapshot_and_changes) {
    cases::Device d;
    FakeSubscriber s;
    Resp r = subscribe(d.api, &s, "/lamp");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "{\"on\":false,\"brightness\":128,\"label\":\"desk\"}");
    CHECK_EQ(d.api.subscriptionCount(), 1u);

    d.api.poll(1000);
    CHECK_EQ(s.messages.size(), 0u);  // nothing changed yet

    CHECK_EQ(cases::set(d.api, "/lamp/brightness", "200").status, Status::Ok);
    d.api.poll(1010);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"brightness\":200}}");
    d.api.poll(1020);
    CHECK_EQ(s.messages.size(), 1u);  // delivered once

    // A patch touching two values gives one notification with both.
    cases::set(d.api, "/lamp", "{\"on\":true,\"label\":\"x\"}");
    d.api.poll(1030);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"on\":true,\"label\":\"x\"}}");
    // Changes elsewhere don't concern this subscription.
    cases::set(d.api, "/config/level", "9");
    d.api.poll(1040);
    CHECK_EQ(s.messages.size(), 2u);
}

TEST(sub_nested_sparse_patch) {
    cases::Device d;
    FakeSubscriber s;
    subscribe(d.api, &s, "/");
    cases::set(d.api, "/config/numbers/huge", "5");
    cases::set(d.api, "/lamp/on", "true");
    d.api.poll(1000);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"lamp\":{\"on\":true},\"config\":{\"numbers\":{\"huge\":5}}}}");
}

TEST(sub_filters_and_leaf) {
    cases::Device d;
    FakeSubscriber keyed, shallow, leaf;
    Query q;
    q.keys = "lamp";
    subscribe(d.api, &keyed, "/", q);
    subscribe(d.api, &shallow, "/config", cases::depth(1));
    subscribe(d.api, &leaf, "/lamp/on");

    cases::set(d.api, "/config/numbers/big", "1");
    cases::set(d.api, "/config/level", "4");
    d.api.poll(1000);
    CHECK_EQ(keyed.messages.size(), 0u);
    CHECK_EQ(shallow.last(), "{\"op\":\"change\",\"path\":\"/config\",\"body\":{\"level\":4}}");
    CHECK_EQ(leaf.messages.size(), 0u);

    cases::set(d.api, "/lamp/on", "true");
    d.api.poll(1010);
    CHECK_EQ(keyed.last(), "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"lamp\":{\"on\":true}}}");
    CHECK_EQ(leaf.last(), "{\"op\":\"change\",\"path\":\"/lamp/on\",\"body\":true}");
}

TEST(sub_interval_coalesces) {
    cases::Device d;
    FakeSubscriber s;
    Query q;
    q.interval = 100;
    subscribe(d.api, &s, "/lamp", q);
    cases::set(d.api, "/lamp/brightness", "1");
    d.api.poll(1000);
    CHECK_EQ(s.messages.size(), 1u);
    cases::set(d.api, "/lamp/brightness", "2");
    d.api.poll(1050);
    cases::set(d.api, "/lamp/brightness", "3");
    d.api.poll(1090);
    CHECK_EQ(s.messages.size(), 1u);  // inside the interval
    d.api.poll(1100);
    CHECK_EQ(s.messages.size(), 2u);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"brightness\":3}}");
}

TEST(sub_failed_notify_retries_with_overflow) {
    cases::Device d;
    FakeSubscriber s;
    subscribe(d.api, &s, "/lamp");
    s.failNext = 1;
    cases::set(d.api, "/lamp/brightness", "7");
    d.api.poll(1000);
    CHECK_EQ(s.messages.size(), 0u);
    CHECK(s.missed.load());
    cases::set(d.api, "/lamp/on", "true");
    d.api.poll(1010);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"on\":true,\"brightness\":7},\"overflow\":true}");
    CHECK(!s.missed.load());
}

TEST(sub_events) {
    Api api;
    auto& btn = api.object("button");
    tesser::EventNode& pressed = btn.event("pressed");
    tesser::EventNode& held = btn.event("held").doc("long press");
    auto& other = api.object("other").event("ping");
    int presses = 0;
    btn.value("presses", presses);

    FakeSubscriber all, button, quiet, unrelated;
    CHECK_EQ(subscribe(api, &all, "/").status, Status::Ok);
    subscribe(api, &button, "/button");
    subscribe(api, &button, "/button/pressed");  // overlapping: still delivered once
    Query noEvents;
    noEvents.events = false;
    subscribe(api, &quiet, "/button", noEvents);
    CHECK_EQ(subscribe(api, &unrelated, "/other/ping").body, "null");

    pressed.emit(3);
    CHECK_EQ(all.last(), "{\"op\":\"event\",\"path\":\"/button/pressed\",\"body\":3}");
    CHECK_EQ(button.messages.size(), 1u);
    CHECK_EQ(quiet.messages.size(), 0u);
    CHECK_EQ(unrelated.messages.size(), 0u);

    held.emit();
    CHECK_EQ(button.last(), "{\"op\":\"event\",\"path\":\"/button/held\",\"body\":null}");
    other.emitWith([](tesser::JsonWriter& w) {
        w.beginObject();
        w.key("n");
        w.integer(1);
        w.endObject();
    });
    CHECK_EQ(unrelated.last(), "{\"op\":\"event\",\"path\":\"/other/ping\",\"body\":{\"n\":1}}");
    CHECK_EQ(all.messages.size(), 3u);
    CHECK_EQ(cases::get(api, "/button", cases::schema()).body,
             "{\"type\":\"object\",\"children\":{\"pressed\":{\"type\":\"event\"},"
             "\"held\":{\"type\":\"event\",\"description\":\"long press\"},\"presses\":{\"type\":\"integer\",\"writable\":true}}}");
    CHECK_EQ(cases::get(api, "/button").body, "{\"presses\":0}");
    CHECK_EQ(cases::set(api, "/button/pressed", "1").status, Status::NotAllowed);
}

// "changes": false: events only, from anywhere under the path.
TEST(sub_events_only) {
    Api api;
    tesser::EventNode& joined = api.object("sys").object("wifi").event("joined");
    int level = 0;
    api.value("level", level);
    cases::Lines lines;
    tesser::LineTransport t(api, [&](const char* data, size_t n) { lines.add(data, n); });
    const char* sub = "{\"id\":1,\"op\":\"sub\",\"path\":\"/\",\"changes\":false,\"snapshot\":false,\"interval\":60000}\n";
    t.feed(sub, strlen(sub));
    CHECK_EQ(lines.out.size(), 1u);
    if (!lines.out.empty()) CHECK_EQ(lines.out[0], "{\"id\":1,\"status\":\"ok\",\"body\":null}");
    CHECK_EQ(api.subscriptionCount(), 1u);

    cases::set(api, "/level", "3");
    api.poll(1000);
    api.poll(70000);
    CHECK_EQ(lines.out.size(), 1u);  // no change notifications, ever
    joined.emit(7);
    CHECK_EQ(lines.out.size(), 2u);
    if (lines.out.size() == 2) CHECK_EQ(lines.out[1], "{\"op\":\"event\",\"path\":\"/sys/wifi/joined\",\"body\":7}");

    const char* bad = "{\"id\":2,\"op\":\"sub\",\"path\":\"/\",\"changes\":0}\n";
    t.feed(bad, strlen(bad));
    CHECK(!lines.out.empty() && lines.out.back().find("changes must be a boolean") != std::string::npos);

    Query q;
    q.changes = false;
    q.snapshot = false;
    CHECK_EQ(tesser::buildRequestEnvelope(3, Op::Subscribe, "/", q, std::string_view()),
             "{\"id\":3,\"op\":\"sub\",\"path\":\"/\",\"changes\":false,\"snapshot\":false}");
}

TEST(sub_watch_and_changed) {
    Api api;
    int counter = 0, untracked = 0, explicitly = 0;
    api.value("counter", counter).watch();
    api.value("untracked", untracked);
    auto& e = api.value("explicit", explicitly);
    FakeSubscriber s;
    subscribe(api, &s, "/");
    api.poll(1000);  // first sample: no change
    CHECK_EQ(s.messages.size(), 0u);

    counter = 5;
    untracked = 5;
    api.poll(1100);  // before the watch interval
    CHECK_EQ(s.messages.size(), 0u);
    api.poll(1200);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"counter\":5}}");

    explicitly = 9;
    e.changed();
    api.poll(1210);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"explicit\":9}}");

    // Api::changed(path), and changed() on an object marks everything below.
    CHECK(api.changed("/untracked"));
    CHECK(!api.changed("/nope"));
    api.poll(1220);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"untracked\":5}}");
    api.changed("/");
    api.poll(1230);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"counter\":5,\"untracked\":5,\"explicit\":9}}");
}

// The subscription's snapshot is the watch baseline: a change before the
// first sample is still notified.
TEST(sub_watch_baseline_at_subscribe) {
    Api api;
    int counter = 1;
    api.value("counter", counter).watch();
    FakeSubscriber s;
    CHECK_EQ(subscribe(api, &s, "/").body, "{\"counter\":1}");
    counter = 2;
    api.poll(1000);  // first sample
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"counter\":2}}");

    // A change while nobody was subscribed is in the next snapshot, and not
    // notified again.
    api.dropSubscriber(&s);
    counter = 3;
    FakeSubscriber t;
    CHECK_EQ(subscribe(api, &t, "/counter").body, "3");
    api.poll(2000);
    CHECK_EQ(t.messages.size(), 0u);
}

TEST(sub_limits_unsub_and_drop) {
    cases::Device d;
    FakeSubscriber s, t;
    for (int i = 0; i < 4; i++) CHECK_EQ(subscribe(d.api, &s, "/lamp").status, Status::Ok);
    CHECK_EQ(subscribe(d.api, &s, "/lamp").status, Status::Busy);
    CHECK_EQ(subscribe(d.api, &t, "/config").status, Status::Ok);

    CHECK_EQ(request(d.api, &s, Op::Unsubscribe, "/lamp").body, "4");
    CHECK_EQ(request(d.api, &s, Op::Unsubscribe, "/lamp").body, "0");
    CHECK_EQ(d.api.subscriptionCount(), 1u);
    d.api.dropSubscriber(&t);
    CHECK_EQ(d.api.subscriptionCount(), 0u);

    CHECK_EQ(subscribe(d.api, nullptr, "/lamp").status, Status::NotAllowed);
    CHECK_EQ(subscribe(d.api, &s, "/lamp/toggle").status, Status::NotAllowed);
    CHECK_EQ(subscribe(d.api, &s, "/nope").status, Status::NotFound);
    CHECK_EQ(subscribe(d.api, &s, "/lamp", cases::keys("nope")).status, Status::NotFound);
    CHECK_EQ(d.api.subscriptionCount(), 0u);  // failed snapshots don't subscribe
}

TEST(sub_over_line_transport) {
    cases::Device d;
    cases::Lines lines;
    {
        tesser::LineTransport t(d.api, [&](const char* data, size_t n) { lines.add(data, n); });
        const char* sub = "{\"id\":1,\"op\":\"sub\",\"path\":\"/lamp\",\"keys\":\"on\",\"interval\":0}\n";
        t.feed(sub, strlen(sub));
        CHECK_EQ(lines.out.size(), 1u);
        if (!lines.out.empty()) CHECK_EQ(lines.out[0], "{\"id\":1,\"status\":\"ok\",\"body\":{\"on\":false}}");
        cases::set(d.api, "/lamp/on", "true");
        cases::set(d.api, "/lamp/brightness", "3");  // filtered out by keys
        d.api.poll(1000);
        CHECK_EQ(lines.out.size(), 2u);
        if (lines.out.size() == 2) CHECK_EQ(lines.out[1], "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"on\":true}}");
        CHECK_EQ(d.api.subscriptionCount(), 1u);
    }
    // Destroying the transport drops its subscriptions.
    CHECK_EQ(d.api.subscriptionCount(), 0u);
}

TEST(sub_over_datagram) {
    using datagram_cases::addr;
    cases::Device d;
    datagram_cases::Net net;
    tesser::DatagramEndpoint server(&d.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    const char* sub = "{\"id\":1,\"op\":\"sub\",\"path\":\"/lamp/brightness\"}";
    server.receive(addr(1), sub, strlen(sub), true);
    server.process();
    CHECK_EQ(net.frames.size(), 1u);
    CHECK_EQ(d.api.subscriptionCount(), 1u);
    net.frames.clear();

    cases::set(d.api, "/lamp/brightness", "42");
    d.api.poll(1000);
    CHECK_EQ(net.frames.size(), 1u);
    if (!net.frames.empty()) {
        CHECK_EQ(net.frames[0].text, "{\"op\":\"change\",\"path\":\"/lamp/brightness\",\"body\":42}");
        CHECK(net.frames[0].to == addr(1));
    }

    // An unreliable sub (broadcast) is refused; a lost peer is dropped.
    server.receive(addr(3), sub, strlen(sub), false);
    server.process();
    CHECK_EQ(d.api.subscriptionCount(), 1u);
    server.forgetPeer(addr(1));
    CHECK_EQ(d.api.subscriptionCount(), 1u);
    server.process();
    CHECK_EQ(d.api.subscriptionCount(), 0u);
}

TEST(sub_client_receives_notifications) {
    using datagram_cases::addr;
    cases::Device d;
    datagram_cases::Net net;
    tesser::DatagramEndpoint server(&d.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    tesser::DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    net.nodes = {{addr(1), &client}, {addr(2), &server}};
    std::vector<std::string> got;
    client.onNotification([&](const tesser::PeerAddress& from, JsonObjectConst env) {
        CHECK(from == addr(2));
        std::string s;
        serializeJson(env, s);
        got.push_back(s);
    });
    datagram_cases::Result r;
    tesser::Query q;
    q.keys = "on";
    q.interval = 5;
    CHECK(client.request(addr(2), Op::Subscribe, "/lamp", std::string_view(), datagram_cases::capture(r), q));
    net.run();
    CHECK_EQ(r.body, "{\"on\":false}");
    cases::set(d.api, "/lamp/on", "true");
    d.api.poll(5000);
    net.run();
    CHECK_EQ(got.size(), 1u);
    if (!got.empty()) CHECK_EQ(got[0], "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"on\":true}}");
    CHECK_EQ(server.stats().dropped, 0u);
    CHECK_EQ(client.stats().dropped, 0u);
}

TEST(queued_mode) {
    cases::Device d;
    d.api.config().queued = true;
    d.api.config().maxQueued = 2;
    cases::Lines lines;
    tesser::LineTransport t(d.api, [&](const char* data, size_t n) { lines.add(data, n); });
    const char* set = "{\"id\":1,\"op\":\"set\",\"path\":\"/lamp/brightness\",\"body\":5}\n";
    t.feed(set, strlen(set));
    CHECK_EQ(lines.out.size(), 0u);  // waits for poll()
    CHECK_EQ(d.api.queuedRequests(), 1u);
    CHECK_EQ(d.lamp.brightness, 128);
    t.feed(set, strlen(set));
    t.feed(set, strlen(set));  // queue full
    CHECK_EQ(lines.out.size(), 1u);
    if (!lines.out.empty()) CHECK(lines.out[0].find("\"status\":\"busy\"") != std::string::npos);
    d.api.poll(1000);
    CHECK_EQ(lines.out.size(), 3u);
    if (lines.out.size() == 3) CHECK_EQ(lines.out[1], "{\"id\":1,\"status\":\"ok\",\"body\":5}");
    CHECK_EQ(d.lamp.brightness, 5);

    // Replies that can't wait are handled inline.
    CHECK_EQ(cases::set(d.api, "/lamp/brightness", "6").body, "6");

    // Deferred actions still work from the queue.
    tesser::Pending held;
    d.api.action("slow", [&](tesser::Call& call) { held = call.defer(); });
    lines.out.clear();
    const char* slow = "{\"id\":2,\"op\":\"set\",\"path\":\"/slow\"}\n";
    t.feed(slow, strlen(slow));
    d.api.poll(1010);
    CHECK_EQ(lines.out.size(), 0u);
    held.reply(1);
    CHECK_EQ(lines.out.size(), 1u);
    if (!lines.out.empty()) CHECK_EQ(lines.out[0], "{\"id\":2,\"status\":\"ok\",\"body\":1}");
}

TEST(queued_requests_die_with_their_client) {
    cases::Device d;
    d.api.config().queued = true;
    {
        cases::Lines lines;
        tesser::LineTransport t(d.api, [&](const char* data, size_t n) { lines.add(data, n); });
        const char* get = "{\"id\":1,\"op\":\"get\",\"path\":\"/lamp\"}\n";
        t.feed(get, strlen(get));
        CHECK_EQ(d.api.queuedRequests(), 1u);
    }
    CHECK_EQ(d.api.queuedRequests(), 0u);
    d.api.poll(1000);  // nothing left to run
}

// The generation counter wraps (docs/DESIGN.md section 11): a value changed
// once is notified once, however many changes elsewhere follow, and changes
// across the wrap are all seen.
TEST(sub_generation_wraparound) {
    tesser::detail::setGeneration(0x1000);  // these stamps stay clear of the wrap
    cases::Device d;
    FakeSubscriber s;
    subscribe(d.api, &s, "/lamp");
    cases::set(d.api, "/lamp/on", "true");
    d.api.poll(1000);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"on\":true}}");
    // Many changes elsewhere, past half of what 16 bits held.
    for (int i = 0; i < 40000; i++) d.api.changed("/config/level");
    cases::set(d.api, "/lamp/brightness", "1");
    d.api.poll(1010);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"brightness\":1}}");
    // Across the wrap of the 32-bit counter, and half of it away; polled
    // along the way, as a live device is.
    auto advance = [&](uint32_t to) {
        while (to - tesser::currentGeneration() > tesser::kGenerationHorizon / 2) {
            tesser::detail::setGeneration(tesser::currentGeneration() + tesser::kGenerationHorizon / 2);
            d.api.poll(1000);
        }
        tesser::detail::setGeneration(to);
    };
    advance(0xfffffffeu);
    cases::set(d.api, "/lamp/brightness", "2");
    d.api.poll(1020);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"brightness\":2}}");
    d.api.changed("/config/level");
    d.api.changed("/config/level");
    cases::set(d.api, "/lamp/label", "\"x\"");
    d.api.poll(1030);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"label\":\"x\"}}");
    advance(tesser::currentGeneration() + 0x80000000u);
    cases::set(d.api, "/lamp/on", "false");
    d.api.poll(1040);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"on\":false}}");
    // Jumped over, the first changes' stamps may each be notified once
    // more (a whole lap old): never again after that.
    size_t sent = s.messages.size();
    d.api.poll(1050);
    CHECK_EQ(s.messages.size(), sent);
    // A subscription left undelivered for longer than the horizon loses
    // what is older (the next notification says "overflow": re-read), and
    // stays in step afterwards.
    s.failNext = 1;
    cases::set(d.api, "/lamp/brightness", "3");
    d.api.poll(1060);
    CHECK_EQ(s.messages.size(), sent);
    tesser::detail::setGeneration(tesser::currentGeneration() + tesser::kGenerationHorizon + 10);
    cases::set(d.api, "/lamp/label", "\"y\"");
    d.api.poll(1070);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"label\":\"y\"},\"overflow\":true}");
    d.api.poll(1080);
    CHECK_EQ(s.messages.size(), sent + 1);
}

}  // namespace subscription_cases

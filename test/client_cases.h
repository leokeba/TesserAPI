// ApiClient: LocalClient against an API in the same process, PeerClient over
// the in-memory datagram link.
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"
#include "datagram_cases.h"

namespace client_cases {

using datagram_cases::addr;
using tesser::Status;

struct Log {
    std::vector<std::string> lines;
    tesser::ApiClient::ResponseHandler handler() {
        return [this](Status s, JsonVariantConst body) {
            std::string b;
            serializeJson(body, b);
            lines.push_back(std::string(tesser::toString(s)) + " " + b);
        };
    }
    void watch(tesser::ApiClient& c) {
        c.onNotification([this](JsonObjectConst env) {
            std::string e;
            serializeJson(env, e);
            lines.push_back(e);
        });
    }
    std::string last() const { return lines.empty() ? "" : lines.back(); }
};

TEST(client_local) {
    cases::Device d;
    tesser::LocalClient c(d.api);
    Log log;
    log.watch(c);
    tesser::Query q;
    q.keys = "on";
    CHECK(c.get("/lamp", log.handler(), q));
    CHECK(c.set("/lamp/brightness", "300", log.handler()));
    CHECK(!c.set("/lamp/brightness", "{nope", log.handler()));  // not JSON: not sent
    // Nothing runs before process(), in the caller's task.
    CHECK_EQ(log.lines.size(), 0u);
    CHECK_EQ(c.queued(), 2u);
    CHECK_EQ(c.process(), 2u);
    CHECK_EQ(log.lines.size(), 2u);
    if (log.lines.size() == 2) {
        CHECK_EQ(log.lines[0], "ok {\"on\":false}");
        CHECK(log.lines[1].find("invalid_value {\"error\":\"invalid_value\"") == 0);
    }

    // Subscriptions: the snapshot as a response, then change notifications.
    CHECK(c.subscribe("/lamp", log.handler()));
    c.process();
    CHECK_EQ(log.last(), "ok {\"on\":false,\"brightness\":128,\"label\":\"desk\"}");
    cases::set(d.api, "/lamp/on", "true");
    d.api.poll(1000);
    CHECK_EQ(c.process(), 1u);
    CHECK_EQ(log.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"on\":true}}");

    // A slow client loses notifications beyond its queue, and is told.
    tesser::LocalClient small(d.api, 1);
    Log slow;
    slow.watch(small);
    small.subscribe("/lamp", nullptr);
    small.process();
    cases::set(d.api, "/lamp/brightness", "1");
    d.api.poll(1100);
    cases::set(d.api, "/lamp/brightness", "2");
    d.api.poll(1200);
    cases::set(d.api, "/lamp/brightness", "3");
    d.api.poll(1300);
    small.process();
    CHECK_EQ(slow.lines.size(), 1u);
    cases::set(d.api, "/lamp/brightness", "4");
    d.api.poll(1400);
    small.process();
    CHECK(slow.last().find("\"overflow\":true") != std::string::npos);

    // Destroying a client drops its subscriptions.
    size_t before = d.api.subscriptionCount();
    { tesser::LocalClient gone(d.api); gone.subscribe("/sensors", nullptr); }
    CHECK_EQ(d.api.subscriptionCount(), before);
}

TEST(client_local_deferred_and_auth) {
    tesser::Api api;
    tesser::Pending held;
    int secret = 1;
    api.action("slow", [&](tesser::Call& call) { held = call.defer(); });
    api.value("secret", secret);
    api.authorize(tesser::authorizers::readOnlyUnlessAuthenticated());
    tesser::LocalClient c(api);
    Log log;
    c.set("/slow", "", log.handler());
    CHECK_EQ(c.process(), 0u);
    held.reply(42);
    CHECK_EQ(c.process(), 1u);
    CHECK_EQ(log.last(), "ok 42");

    c.set("/secret", "2", log.handler());
    c.setAuthenticated(false);
    c.set("/secret", "3", log.handler());
    c.process();
    CHECK_EQ(secret, 2);
    CHECK(log.last().find("unauthorized") == 0);

    // A reply that arrives after the client is gone is dropped.
    {
        tesser::LocalClient brief(api);
        brief.set("/slow", "", log.handler());
    }
    held.reply(1);
}

TEST(client_peer) {
    cases::Device d;
    datagram_cases::Net net;
    tesser::DatagramEndpoint server(&d.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    tesser::DatagramEndpoint ep(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    net.nodes = {{addr(1), &ep}, {addr(2), &server}};
    Log log;
    {
        tesser::PeerClient c(ep, addr(2));
        log.watch(c);
        CHECK(c.get("/lamp/on", log.handler()));
        CHECK(c.subscribe("/lamp", log.handler()));
        net.run();
        CHECK_EQ(log.lines.size(), 0u);  // queued until process()
        CHECK_EQ(c.process(), 2u);
        CHECK_EQ(log.lines.size(), 2u);
        if (log.lines.size() == 2) CHECK_EQ(log.lines[0], "ok false");
        CHECK_EQ(d.api.subscriptionCount(), 1u);

        cases::set(d.api, "/lamp/brightness", "9");
        d.api.poll(1000);
        net.run();
        c.process();
        CHECK_EQ(log.last(), "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"brightness\":9}}");

        // A peer that doesn't answer: the call times out.
        net.dropAll = true;
        CHECK(c.get("/lamp", log.handler()));
        net.clock += 4000;
        ep.process();
        c.process();
        CHECK_EQ(log.last(), "timeout null");
        net.dropAll = false;
    }
    // The client's subscriptions are undone when it goes.
    net.run();
    CHECK_EQ(d.api.subscriptionCount(), 0u);
    CHECK_EQ(ep.pendingCalls(), 0u);
}

}  // namespace client_cases

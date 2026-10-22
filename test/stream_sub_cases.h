// StreamSubscriber: a subscription delivered as lines on one long response
// (HTTP streamed subscriptions, docs/DESIGN.md section 10.2).
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"

namespace stream_sub_cases {

using tesser::Status;

struct Fixture {
    cases::Device device;
    tesser::EventNode& pressed;
    std::vector<std::string> pieces;
    bool fail = false;
    tesser::StreamSubscriber stream;

    Fixture()
        : pressed(device.api.object("button").event("pressed")),
          stream(device.api, [this](const std::string& p) {
              if (fail) return false;
              pieces.push_back(p);
              return true;
          }) {}

    Status open(const char* path, tesser::Query q, tesser::StringReply& error) {
        tesser::Request req;
        req.op = tesser::Op::Get;  // replaced
        req.path = path;
        req.query = q;
        req.client.authenticated = true;
        return stream.open(req, error);
    }
};

inline tesser::Query eventsOnly() {
    tesser::Query q;
    q.changes = false;
    q.snapshot = false;
    q.interval = 60000;
    return q;
}

TEST(stream_events_only) {
    Fixture f;
    tesser::StringReply error;
    CHECK_EQ(f.open("/", eventsOnly(), error), Status::Ok);
    CHECK(!error.begun);
    CHECK(f.pieces.empty());  // no snapshot
    CHECK(f.stream.isOpen());
    CHECK_EQ(f.device.api.subscriptionCount(), 1u);

    f.pressed.emit(3);
    cases::set(f.device.api, "/lamp/brightness", "7");
    f.device.api.poll(100000);
    CHECK_EQ(f.pieces.size(), 1u);
    if (!f.pieces.empty()) CHECK_EQ(f.pieces[0], "{\"op\":\"event\",\"path\":\"/button/pressed\",\"body\":3}\n");

    // Keep-alive: once nothing has gone out for the idle time.
    f.pieces.clear();
    CHECK(f.stream.keepAlive(1000, 15000));  // the event went out just now
    CHECK(f.stream.keepAlive(11000, 15000));
    CHECK(f.pieces.empty());
    CHECK(f.stream.keepAlive(16000, 15000));
    CHECK_EQ(f.pieces.size(), 1u);
    if (!f.pieces.empty()) CHECK_EQ(f.pieces[0], "\n");
    f.pressed.emit(5);
    CHECK(f.stream.keepAlive(31000, 15000));  // sent since: restarts the period
    CHECK_EQ(f.pieces.size(), 2u);
    f.fail = true;
    CHECK(f.stream.keepAlive(45000, 15000));
    CHECK(!f.stream.keepAlive(46000, 15000));

    // One subscription per stream; closing ends it.
    tesser::StringReply again;
    CHECK_EQ(f.open("/", eventsOnly(), again), Status::NotAllowed);
    f.stream.close();
    CHECK(!f.stream.isOpen());
    CHECK_EQ(f.device.api.subscriptionCount(), 0u);
    f.fail = false;
    f.pieces.clear();
    f.pressed.emit(4);
    CHECK(f.stream.keepAlive(100000, 15000));
    CHECK(f.pieces.empty());
}

TEST(stream_snapshot_and_changes) {
    Fixture f;
    tesser::StringReply error;
    tesser::Query q;
    q.events = false;
    CHECK_EQ(f.open("/lamp/", q, error), Status::Ok);
    CHECK_EQ(f.pieces.size(), 1u);
    if (!f.pieces.empty()) {
        CHECK_EQ(f.pieces[0],
                 "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"on\":false,\"brightness\":128,\"label\":\"desk\"}}\n");
    }
    cases::set(f.device.api, "/lamp/brightness", "9");
    f.device.api.poll(100000);
    CHECK_EQ(f.pieces.size(), 2u);
    if (f.pieces.size() == 2) CHECK_EQ(f.pieces[1], "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"brightness\":9}}\n");
    // A failed send is a lost notification: the next one says so.
    f.fail = true;
    cases::set(f.device.api, "/lamp/brightness", "10");
    f.device.api.poll(200000);
    f.fail = false;
    cases::set(f.device.api, "/lamp/brightness", "11");
    f.device.api.poll(300000);
    CHECK_EQ(f.pieces.size(), 3u);
    if (f.pieces.size() == 3) {
        CHECK_EQ(f.pieces[2], "{\"op\":\"change\",\"path\":\"/lamp\",\"body\":{\"brightness\":11},\"overflow\":true}\n");
    }
}

TEST(stream_refusals) {
    {
        Fixture f;
        tesser::StringReply error;
        CHECK_EQ(f.open("/nope", eventsOnly(), error), Status::NotFound);
        CHECK_EQ(error.body, "{\"error\":\"not_found\",\"path\":\"/nope\",\"message\":\"no such node\"}");
        CHECK(!f.stream.isOpen());
        CHECK(f.pieces.empty());
    }
    {
        // The subscription budget applies.
        Fixture f;
        f.device.api.config().maxSubscriptions = 0;
        tesser::StringReply error;
        CHECK_EQ(f.open("/", eventsOnly(), error), Status::Busy);
        CHECK(!f.stream.isOpen());
    }
    {
        // A snapshot too large for maxResponse: refused, nothing subscribed.
        Fixture f;
        f.device.api.config().maxResponse = 16;
        tesser::StringReply error;
        CHECK_EQ(f.open("/", tesser::Query(), error), Status::TooLarge);
        CHECK_EQ(f.device.api.subscriptionCount(), 0u);
        CHECK(f.pieces.empty());
    }
    {
        // Access is fixed at open: a public stream doesn't see what it can't read.
        Fixture f;
        auto& secret = f.device.api.object("admin").readAccess(tesser::Access::Admin).event("alarm");
        tesser::Request req;
        req.path = "/";
        req.query = eventsOnly();
        tesser::StringReply error;
        CHECK_EQ(f.stream.open(req, error), Status::Ok);
        secret.emit();
        f.pressed.emit();
        CHECK_EQ(f.pieces.size(), 1u);
        if (!f.pieces.empty()) CHECK_EQ(f.pieces[0], "{\"op\":\"event\",\"path\":\"/button/pressed\",\"body\":null}\n");
    }
}

// A stream whose token was revoked (Api::tokensChanged()) loses its
// subscription when it was made above the new level, and is closed.
TEST(stream_token_revoked) {
    Fixture f;
    f.device.api.object("sys").readAccess(tesser::Access::Admin).value("level", f.device.lamp.brightness);
    bool valid = true;
    f.device.api.authenticate([&](std::string_view token, const tesser::Client& client) {
        return valid && token == "t" && client.addressLength == 4 ? tesser::Access::Admin : tesser::Access::Public;
    });
    tesser::Request req;
    req.path = "/sys";
    req.client.addressLength = 4;
    req.client.access = tesser::Access::Admin;
    f.stream.presented("t", false);
    tesser::StringReply error;
    CHECK_EQ(f.stream.open(req, error), Status::Ok);
    f.device.api.tokensChanged();
    CHECK_EQ(f.device.api.subscriptionCount(), 1u);
    CHECK(!f.stream.takeRevoked());
    valid = false;
    f.device.api.tokensChanged();
    CHECK_EQ(f.device.api.subscriptionCount(), 0u);
    CHECK(!f.stream.isOpen());
    CHECK(f.stream.takeRevoked());
    CHECK(!f.stream.takeRevoked());  // once

    // The server's own token: kept.
    Fixture g;
    g.device.api.authenticate([](std::string_view) { return tesser::Access::Public; });
    g.stream.presented("own", true);
    tesser::StringReply error2;
    CHECK_EQ(g.open("/", eventsOnly(), error2), Status::Ok);
    g.device.api.tokensChanged();
    CHECK_EQ(g.device.api.subscriptionCount(), 1u);
}

}  // namespace stream_sub_cases

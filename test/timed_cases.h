// Timed sets (docs/DESIGN.md sections 10.1.3 and 14.3): a set carrying the
// network time to apply it, checked when it arrives, held by the node's
// scheduler and applied at its time; streamed ones never merged, kept in
// order, several in flight.
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"
#include "route_cases.h"

namespace timed_cases {

using cases::Resp;
using tesser::Op;
using tesser::Query;
using tesser::Status;
using tesser::TimedWrite;

inline Query at(uint64_t us) {
    Query q;
    q.at = us;
    return q;
}

// A device whose scheduler keeps what it is given, and answers `answer`.
struct Scheduled {
    cases::Device d;
    std::vector<TimedWrite> writes;
    Status answer = Status::Ok;
    Scheduled() {
        d.api.setScheduler([this](TimedWrite&& w) {
            if (answer == Status::Ok) writes.push_back(std::move(w));
            return answer;
        });
    }
};

TEST(timed_set_on_the_wire) {
    // Strict: a node that predates "at" refuses the set rather than apply
    // it on arrival.
    CHECK_EQ(tesser::buildRequestEnvelope(1, Op::Set, "/a", at(1234567890123ULL), "1"),
             "{\"id\":1,\"op\":\"set\",\"path\":\"/a\",\"at\":1234567890123,\"v\":2,\"body\":1}");
    CHECK_EQ(tesser::buildRequestEnvelope(2, Op::Get, "/a", at(5), ""), "{\"id\":2,\"op\":\"get\",\"path\":\"/a\"}");

    cases::Device d;
    // No scheduler: refused.
    CHECK(cases::roundTrip(d.api, "{\"id\":3,\"op\":\"set\",\"path\":\"/lamp/on\",\"body\":true,\"at\":5,\"v\":2}")
              .find("\"error\":\"not_allowed\"") != std::string::npos);
    CHECK(!d.lamp.on);
    CHECK(cases::roundTrip(d.api, "{\"id\":4,\"op\":\"set\",\"path\":\"/lamp/on\",\"body\":true,\"at\":0}")
              .find("\"error\":\"bad_request\"") != std::string::npos);
    CHECK(cases::roundTrip(d.api, "{\"id\":5,\"op\":\"set\",\"path\":\"/lamp/on\",\"body\":true,\"at\":\"5\"}")
              .find("\"error\":\"bad_request\"") != std::string::npos);
    CHECK(cases::roundTrip(d.api, "{\"id\":6,\"op\":\"get\",\"path\":\"/lamp/on\",\"at\":5}")
              .find("\"message\":\"at is for set\"") != std::string::npos);
    // Over HTTP a GET's ?at= reaches the handler: refused there too.
    CHECK_EQ(cases::request(d.api, Op::Get, "/lamp/on", nullptr, at(5)).status, Status::BadRequest);
    CHECK(cases::roundTrip(d.api, "{\"op\":\"get\",\"path\":\"/\",\"view\":\"info\"}").find("\"at\"") !=
          std::string::npos);
}

TEST(timed_set_checked_then_scheduled) {
    Scheduled s;
    tesser::Api& api = s.d.api;
    Resp r = cases::request(api, Op::Set, "/lamp/on", "true", at(1000));
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "null");
    CHECK(!s.d.lamp.on);  // not yet
    CHECK_EQ(s.writes.size(), 1u);
    if (s.writes.size() == 1) {
        CHECK_EQ(s.writes[0].path, "/lamp/on");
        CHECK_EQ(s.writes[0].body, "true");
        CHECK_EQ(s.writes[0].at, 1000u);
        CHECK(s.writes[0].persist);
        CHECK_EQ(api.applyTimed(s.writes[0]), Status::Ok);
        CHECK(s.d.lamp.on);
    }

    // Checked on arrival, as a set would be: nothing scheduled.
    CHECK_EQ(cases::request(api, Op::Set, "/lamp/brightness", "999", at(1)).status, Status::InvalidValue);
    CHECK_EQ(cases::request(api, Op::Set, "/sensors/serial", "1", at(1)).status, Status::ReadOnly);
    CHECK_EQ(cases::request(api, Op::Set, "/lamp/nothing", "1", at(1)).status, Status::NotFound);
    CHECK_EQ(cases::request(api, Op::Set, "/lamp", "{\"on\":1}", at(1)).status, Status::InvalidValue);
    CHECK_EQ(cases::request(api, Op::Set, "/lamp/*", "true", at(1)).status, Status::BadRequest);
    api.authorize(tesser::authorizers::readOnlyUnlessAuthenticated());
    CHECK_EQ(cases::request(api, Op::Set, "/lamp/on", "false", at(1)).status, Status::Unauthorized);
    api.authorize(nullptr);
    CHECK_EQ(s.writes.size(), 1u);

    // A patch and an action, applied later.
    Query q = at(2000);
    q.persist = false;
    CHECK_EQ(cases::request(api, Op::Set, "/lamp", "{\"on\":false,\"brightness\":10}", q).status, Status::Ok);
    CHECK_EQ(cases::request(api, Op::Set, "/lamp/toggle", "null", at(3000)).status, Status::Ok);
    CHECK_EQ(s.writes.size(), 3u);
    if (s.writes.size() == 3) {
        CHECK(!s.writes[1].persist);
        CHECK_EQ(api.applyTimed(s.writes[1]), Status::Ok);
        CHECK(!s.d.lamp.on);
        CHECK_EQ(s.d.lamp.brightness, 10);
        CHECK_EQ(s.d.toggles, 0);
        CHECK_EQ(api.applyTimed(s.writes[2]), Status::Ok);
        CHECK_EQ(s.d.toggles, 1);
        // Checked again when applied: the tree may have changed since.
        TimedWrite bad = s.writes[1];
        bad.body = "{\"brightness\":999}";
        CHECK_EQ(api.applyTimed(bad), Status::InvalidValue);
    }

    // What the scheduler answers.
    s.answer = Status::Timeout;
    r = cases::request(api, Op::Set, "/lamp/on", "true", at(4000));
    CHECK_EQ(r.status, Status::Timeout);
    CHECK(r.body.find("\"reason\":\"late\"") != std::string::npos);
    s.answer = Status::Busy;
    CHECK_EQ(cases::request(api, Op::Set, "/lamp/on", "true", at(4000)).status, Status::Busy);
    s.answer = Status::NotAllowed;
    CHECK_EQ(cases::request(api, Op::Set, "/lamp/on", "true", at(4000)).status, Status::NotAllowed);
    CHECK_EQ(s.writes.size(), 3u);
    CHECK(api.hasScheduler());
    api.setScheduler(nullptr);
    CHECK(!api.hasScheduler());
}

TEST(timed_stream_in_order) {
    // A master streams timed sets to a device: each one reaches the
    // scheduler, none merged, their times strictly increasing; several in
    // flight at once; applied as the master (writer tracking).
    route_cases::Defaults d;
    route_cases::Rig r;
    std::vector<TimedWrite> writes;
    Status answer = Status::Ok;
    r.dev.setScheduler([&](TimedWrite&& w) {
        if (answer == Status::Ok) writes.push_back(std::move(w));
        return answer;
    });
    std::vector<std::string> released;
    tesser::Object& out = *static_cast<tesser::Object*>(r.dev.child("out"));
    r.dev.trackWriter(out, [&](tesser::Node&, tesser::PeerLoss) { released.push_back("out"); });
    tesser::RemoteNode::setDefaultRouteOrder({tesser::TransportKind::Udp});
    tesser::RemoteNode& n = r.node();

    Query q = at(1000);
    q.persist = false;
    for (int i = 1; i <= 6; i++) CHECK_EQ(n.stream("/out/level", std::to_string(i), q), Status::Ok);
    CHECK_EQ(n.streamStats().sent, uint32_t(tesser::detail::SetStream::kTimedWindow));  // the rest wait
    r.advance(200);
    CHECK_EQ(n.streamStats().sent, 6u);
    CHECK_EQ(n.streamStats().merged, 0u);
    CHECK_EQ(n.streaming(), 0u);
    CHECK_EQ(writes.size(), 6u);
    uint64_t last = 0;
    bool ordered = true;
    for (size_t i = 0; i < writes.size(); i++) {
        ordered = ordered && writes[i].at > last && writes[i].body == std::to_string(i + 1);
        last = writes[i].at;
    }
    CHECK(ordered);
    if (writes.size() == 6) {
        CHECK_EQ(writes[0].at, 1000u);
        CHECK_EQ(writes[5].at, 1005u);  // one µs apart
        CHECK(!writes[0].persist);
        CHECK_EQ(writes[0].path, "/out/level");
        CHECK_EQ(r.level, 0);  // nothing applied on arrival
        for (const TimedWrite& w : writes) CHECK_EQ(r.dev.applyTimed(w), Status::Ok);
        CHECK_EQ(r.level, 6);
        CHECK((r.applied == std::vector<int>{1, 2, 3, 4, 5, 6}));
        CHECK_EQ(r.writer, "10521c75f301");  // the master's device
        tesser::PeerAddress writer;
        CHECK(r.dev.writerOf(out, writer));
    }

    // A later time keeps its own; an untimed set still merges as before.
    CHECK_EQ(n.stream("/out/level", "7", at(5000)), Status::Ok);
    r.advance(100);
    CHECK_EQ(writes.size(), 7u);
    if (writes.size() == 7) CHECK_EQ(writes[6].at, 5000u);

    // Late on the device: counted, reported, not sent again.
    std::vector<std::string> errors;
    n.onStreamError([&](const std::string& path, Status s, JsonVariantConst body) {
        errors.push_back(path + " " + tesser::toString(s) + " " + (body["reason"] | ""));
    });
    answer = Status::Timeout;
    CHECK_EQ(n.stream("/out/level", "8", at(6000)), Status::Ok);
    r.advance(200);
    CHECK_EQ(n.streamStats().late, 1u);
    CHECK_EQ(n.streamStats().failed, 1u);
    CHECK((errors == std::vector<std::string>{"/out/level timeout late"}));
    CHECK_EQ(n.streaming(), 0u);
    CHECK_EQ(writes.size(), 7u);
}

TEST(timed_set_through_a_gateway) {
    // A client's timed set to a remote node goes on with its time.
    route_cases::Defaults d;
    route_cases::Rig r;
    std::vector<TimedWrite> writes;
    r.dev.setScheduler([&](TimedWrite&& w) {
        writes.push_back(std::move(w));
        return Status::Ok;
    });
    tesser::LocalClient local(r.master);
    Status got = Status::Internal;
    CHECK(local.request(Op::Set, "/peers/lamp/out/level", "4",
                        [&](Status s, JsonVariantConst) { got = s; }, at(777)));
    r.advance(200);
    local.process();
    CHECK_EQ(got, Status::Ok);
    CHECK_EQ(writes.size(), 1u);
    if (writes.size() == 1) {
        CHECK_EQ(writes[0].at, 777u);
        CHECK_EQ(writes[0].path, "/out/level");
    }
    CHECK_EQ(r.level, 0);
}

}  // namespace timed_cases

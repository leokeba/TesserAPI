// Typed object arguments and typed deferred actions (docs/DESIGN.md
// section 4.2).
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"

namespace action_cases {

using cases::Resp;
using tesser::Api;
using tesser::Status;

// Described by a free function, found by argument-dependent lookup.
struct Target {
    std::string name = "New target";
    float az = 180;
    float el = 30;
};

inline void describe(tesser::Object& o, Target& t) {
    o.value("name", t.name);
    o.value("az", t.az).range(0, 360).unit("°");
    o.value("el", t.el).range(-90, 90);
}

// Described by a member, with a nested object.
struct Broadcast {
    std::string msg;
    int retries = 3;
    struct Opts {
        bool reliable = true;
    } opts;
    void describe(tesser::Object& o) {
        o.value("msg", msg);
        o.value("retries", retries).range(0, 10);
        o.object("options").value("reliable", opts.reliable);
    }
};

struct Fixture {
    Api api;
    std::vector<Target> added;
    std::vector<Broadcast> sent;
    int position = 0;
    tesser::Pending pending;
    Target pendingArg;
    double moveArg = 0;

    Fixture() {
        auto& h = api.object("helio");
        h.value("position", position);
        h.action("add", [this](const Target& t) { added.push_back(t); });
        h.action("broadcast", [this](Broadcast b) {
            sent.push_back(b);
            return static_cast<int>(sent.size());
        });
        h.action("addLater", [this](const Target& t, tesser::Call& call) {
            pendingArg = t;
            pending = call.defer();
        });
        h.action("moveLater", [this](double deg, tesser::Call& call) {
            moveArg = deg;
            pending = call.defer();
        }).range(0, 360);
    }
};

TEST(object_arg_schema) {
    Fixture f;
    CHECK_EQ(cases::get(f.api, "/helio/add", cases::schema()).body,
             "{\"type\":\"action\",\"arg\":\"object\",\"params\":{\"type\":\"object\",\"children\":{"
             "\"name\":{\"type\":\"string\",\"writable\":true},"
             "\"az\":{\"type\":\"number\",\"writable\":true,\"min\":0,\"max\":360,\"unit\":\"°\"},"
             "\"el\":{\"type\":\"number\",\"writable\":true,\"min\":-90,\"max\":90}}}}");
    CHECK_EQ(cases::get(f.api, "/helio/broadcast", cases::schema()).body,
             "{\"type\":\"action\",\"arg\":\"object\",\"params\":{\"type\":\"object\",\"children\":{"
             "\"msg\":{\"type\":\"string\",\"writable\":true},"
             "\"retries\":{\"type\":\"integer\",\"writable\":true,\"min\":0,\"max\":10},"
             "\"options\":{\"type\":\"object\",\"children\":{\"reliable\":{\"type\":\"boolean\",\"writable\":true}}}}},"
             "\"returns\":\"integer\"}");
    // Typed deferred actions report their argument.
    std::string later = cases::get(f.api, "/helio/addLater", cases::schema(0)).body;
    CHECK(later.find("\"arg\":\"object\",\"params\":") != std::string::npos);
    CHECK(later.find("\"deferred\":true") != std::string::npos);
    CHECK_EQ(cases::get(f.api, "/helio/moveLater", cases::schema()).body,
             "{\"type\":\"action\",\"arg\":\"number\",\"min\":0,\"max\":360,\"deferred\":true}");
}

TEST(object_arg_call) {
    Fixture f;
    Resp r = cases::set(f.api, "/helio/add", "{\"name\":\"Sun\",\"az\":120.5}");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "null");
    CHECK_EQ(f.added.size(), 1u);
    if (f.added.size() == 1) {
        CHECK_EQ(f.added[0].name, "Sun");
        CHECK_EQ(f.added[0].az, 120.5f);
        CHECK_EQ(f.added[0].el, 30.0f);  // omitted: the default
    }
    // null, or no body: every field keeps its default.
    CHECK_EQ(cases::set(f.api, "/helio/add", "null").status, Status::Ok);
    CHECK_EQ(cases::set(f.api, "/helio/add", nullptr).status, Status::Ok);
    CHECK_EQ(f.added.size(), 3u);
    if (f.added.size() == 3) CHECK_EQ(f.added[2].name, "New target");
    // Nested objects and a return value.
    r = cases::set(f.api, "/helio/broadcast", "{\"msg\":\"hi\",\"options\":{\"reliable\":false}}");
    CHECK_EQ(r.body, "1");
    CHECK(f.sent.size() == 1 && f.sent[0].msg == "hi" && !f.sent[0].opts.reliable && f.sent[0].retries == 3);
}

TEST(object_arg_validation) {
    Fixture f;
    // Each field is validated like a patch; errors name it.
    Resp r = cases::set(f.api, "/helio/add", "{\"name\":\"Sun\",\"az\":400}");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK_EQ(r.body, "{\"error\":\"invalid_value\",\"path\":\"/helio/add/az\",\"message\":\"out of range\"}");
    r = cases::set(f.api, "/helio/add", "{\"nope\":1}");
    CHECK_EQ(r.status, Status::NotFound);
    CHECK(r.body.find("\"path\":\"/helio/add/nope\"") != std::string::npos);
    r = cases::set(f.api, "/helio/broadcast", "{\"options\":{\"reliable\":1}}");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("\"path\":\"/helio/broadcast/options/reliable\"") != std::string::npos);
    CHECK_EQ(cases::set(f.api, "/helio/add", "5").status, Status::InvalidValue);
    CHECK_EQ(cases::set(f.api, "/helio/add", "[]").status, Status::InvalidValue);
    CHECK(f.added.empty());
}

TEST(object_arg_in_patch) {
    Fixture f;
    // Values first, then the action with its argument.
    Resp r = cases::set(f.api, "/helio", "{\"add\":{\"name\":\"Moon\"},\"position\":7}");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "{\"add\":null,\"position\":7}");
    CHECK(f.added.size() == 1 && f.added[0].name == "Moon");
    // A bad argument fails validation: nothing is applied.
    r = cases::set(f.api, "/helio", "{\"position\":9,\"add\":{\"el\":-100}}");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("\"path\":\"/helio/add/el\"") != std::string::npos);
    CHECK_EQ(f.position, 7);
    CHECK_EQ(f.added.size(), 1u);
}

TEST(typed_deferred_actions) {
    Fixture f;
    cases::Lines lines;
    tesser::LineTransport t(f.api, [&](const char* d, size_t n) { lines.add(d, n); });
    const char* req = "{\"id\":1,\"op\":\"set\",\"path\":\"/helio/addLater\",\"body\":{\"name\":\"Venus\",\"el\":10}}\n";
    t.feed(req, strlen(req));
    CHECK(lines.out.empty());
    CHECK_EQ(f.pendingArg.name, "Venus");
    CHECK_EQ(f.pendingArg.el, 10.0f);
    f.pending.reply("added");
    CHECK_EQ(lines.out.size(), 1u);
    if (!lines.out.empty()) CHECK_EQ(lines.out[0], "{\"id\":1,\"status\":\"ok\",\"body\":\"added\"}");

    // The argument is checked before the action runs.
    lines.out.clear();
    const char* bad = "{\"id\":2,\"op\":\"set\",\"path\":\"/helio/addLater\",\"body\":{\"az\":-1}}\n";
    t.feed(bad, strlen(bad));
    CHECK_EQ(lines.out.size(), 1u);
    if (!lines.out.empty()) CHECK(lines.out[0].find("\"path\":\"/helio/addLater/az\"") != std::string::npos);
    const char* move = "{\"id\":3,\"op\":\"set\",\"path\":\"/helio/moveLater\",\"body\":400}\n";
    t.feed(move, strlen(move));
    CHECK_EQ(lines.out.size(), 2u);
    if (lines.out.size() == 2) CHECK(lines.out[1].find("invalid_value") != std::string::npos);
    const char* moveOk = "{\"id\":4,\"op\":\"set\",\"path\":\"/helio/moveLater\",\"body\":90}\n";
    t.feed(moveOk, strlen(moveOk));
    CHECK_EQ(f.moveArg, 90.0);
    f.pending.fail(Status::Busy, "motor busy");
    CHECK_EQ(lines.out.size(), 3u);
    if (lines.out.size() == 3) CHECK(lines.out[2].find("\"status\":\"busy\"") != std::string::npos);

    // Deferred actions can't be part of a patch.
    CHECK_EQ(cases::set(f.api, "/helio", "{\"moveLater\":1}").status, Status::BadRequest);
}

}  // namespace action_cases

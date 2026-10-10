// Value validators: the application's own checks, run in pass 1 with the
// type and range checks (docs/DESIGN.md sections 4.1 and 6.3).
#pragma once

#include <string.h>

#include <string>
#include <vector>

#include "core_cases.h"

namespace validate_cases {

using tesser::Api;
using tesser::Check;
using tesser::Status;

struct Timer {
    std::string name;
    std::string path;
};

inline Check validPath(const std::string& p) {
    return p == "/led/on" || p == "/led/level" ? Check::ok() : Check::fail(Status::NotFound, "no such node");
}

struct Fixture {
    Api api;
    std::string time = "07:30";
    int level = 1;
    std::string hashed = "h(secret1)";
    int setterCalls = 0;
    std::vector<Timer> timers = {{"keep", "/led/on"}};
    std::vector<Timer> added;

    Fixture() {
        auto& c = api.object("cfg");
        c.value("level", level).range(0, 9).persist();
        c.value("time", time).persist().validate([](const std::string& t) {
            return t.size() == 5 && t[2] == ':';  // a bool: "rejected" when false
        });
        // A setter only for its side effect; the check comes first.
        c.value(
             "password", [] { return std::string(); },
             [this](const std::string& p) {
                 setterCalls++;
                 hashed = "h(" + p + ")";
             })
            .secret()
            .validate([](JsonVariantConst v) {
                size_t n = v.as<JsonString>().size();
                return n == 0 || (n >= 8 && n <= 63) ? Check::ok() : Check::fail(Status::InvalidValue, "8 to 63 characters");
            });
        api.list("timers", timers, [](tesser::Object& o, Timer& t) {
               o.value("name", t.name);
               o.value("path", t.path).validate([&t](const std::string& p) {
                   (void)t;  // may capture the element: it exists while the validator runs
                   return validPath(p);
               });
           }).key("name").persist();
        api.action("add", [this](const Timer& t) { added.push_back(t); });
    }
};

// Object arguments describe a Timer the same way.
inline void describe(tesser::Object& o, Timer& t) {
    o.value("name", t.name);
    o.value("path", t.path).validate(validPath);
}

TEST(validate_patch_is_atomic) {
    Fixture f;
    cases::Resp r = cases::set(f.api, "/cfg", "{\"level\":5,\"time\":\"7h30\"}");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("\"path\":\"/cfg/time\"") != std::string::npos);
    CHECK(r.body.find("partial") == std::string::npos);
    CHECK_EQ(f.level, 1);  // nothing applied
    CHECK_EQ(cases::set(f.api, "/cfg", "{\"level\":5,\"time\":\"08:15\"}").status, Status::Ok);
    CHECK_EQ(f.level, 5);
    CHECK_EQ(f.time, "08:15");

    // Before the setter: a refused password is never hashed.
    r = cases::set(f.api, "/cfg/password", "\"short\"");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("8 to 63 characters") != std::string::npos);
    CHECK_EQ(f.setterCalls, 0);
    CHECK_EQ(cases::set(f.api, "/cfg/password", "\"long enough\"").status, Status::Ok);
    CHECK_EQ(f.hashed, "h(long enough)");
}

TEST(validate_list_replacement_is_atomic) {
    Fixture f;
    cases::Resp r = cases::set(f.api, "/timers", "[{\"name\":\"bad\",\"path\":\"/led/nope\"}]");
    CHECK_EQ(r.status, Status::NotFound);
    CHECK(r.body.find("\"path\":\"/timers/bad/path\"") != std::string::npos);
    CHECK_EQ(f.timers.size(), 1u);
    if (!f.timers.empty()) CHECK_EQ(f.timers[0].name, "keep");
    // Existing elements are checked too, through their own nodes.
    CHECK_EQ(cases::set(f.api, "/timers/keep/path", "\"/led/gone\"").status, Status::NotFound);
    CHECK_EQ(cases::set(f.api, "/timers", "[{\"name\":\"keep\"},{\"name\":\"new\",\"path\":\"/led/level\"}]").status,
             Status::Ok);
    CHECK_EQ(f.timers.size(), 2u);
}

TEST(validate_object_argument) {
    Fixture f;
    CHECK_EQ(cases::set(f.api, "/add", "{\"name\":\"t\",\"path\":\"/x\"}").status, Status::NotFound);
    CHECK_EQ(f.added.size(), 0u);
    CHECK_EQ(cases::set(f.api, "/add", "{\"name\":\"t\",\"path\":\"/led/on\"}").status, Status::Ok);
    CHECK_EQ(f.added.size(), 1u);
}

TEST(validate_load_and_restore) {
    tesser::MemoryStorage storage;
    storage.records["cfg"] = "{\"level\":3,\"time\":\"late\"}";
    storage.records["timers"] = "[{\"name\":\"a\",\"path\":\"/led/level\"},{\"name\":\"b\",\"path\":\"/nope\"}]";
    Fixture f;
    f.api.persistence(storage);
    CHECK(f.api.load());
    CHECK_EQ(f.level, 3);
    CHECK_EQ(f.time, "07:30");  // skipped, like any invalid value
    std::vector<std::string> skipped;
    CHECK_EQ(f.api.restore("{\"cfg\":{\"time\":\"9\"}}", &skipped), Status::Ok);
    CHECK_EQ(skipped.size(), 1u);
    if (!skipped.empty()) CHECK_EQ(skipped[0], "/cfg/time");
    CHECK_EQ(f.time, "07:30");
}

TEST(validate_kind_mismatch) {
    Api api;
    int x = 0;
    int before = tesser::declarationErrors();
    api.value("x", x).validate([](const std::string&) { return false; });
    CHECK_EQ(tesser::declarationErrors(), before + 1);
    CHECK_EQ(cases::set(api, "/x", "4").status, Status::Ok);  // not installed

    // Any type of the node's kind will do.
    std::string s = "a";
    api.value("s", s).validate([](const char* v) { return strlen(v) < 3; });
    CHECK_EQ(tesser::declarationErrors(), before + 1);
    CHECK_EQ(cases::set(api, "/s", "\"abc\"").status, Status::InvalidValue);
    CHECK_EQ(cases::set(api, "/s", "\"ab\"").status, Status::Ok);
}

}  // namespace validate_cases

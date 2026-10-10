// Scalar arrays (docs/DESIGN.md section 4.5).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "core_cases.h"
#include "subscription_cases.h"

namespace array_cases {

using cases::Resp;
using tesser::Api;
using tesser::Status;

struct Fixture {
    Api api;
    float offsets[4] = {0, 0.5f, 1, 1.5f};
    std::vector<int> days = {1, 2};
    std::array<uint8_t, 3> rgb = {{10, 20, 30}};
    std::vector<std::string> tags = {"a"};
    const std::vector<int> fixed = {7, 8};

    Fixture() {
        auto& c = api.object("cal");
        c.array("offsets", offsets).range(-5, 5).persist();
        c.array("days", days).range(0, 6).maxSize(7);
        c.array("rgb", rgb);
        c.array("tags", tags);
        c.array("fixed", fixed);
    }
};

TEST(array_read) {
    Fixture f;
    CHECK_EQ(cases::get(f.api, "/cal").body,
             "{\"offsets\":[0,0.5,1,1.5],\"days\":[1,2],\"rgb\":[10,20,30],\"tags\":[\"a\"],\"fixed\":[7,8]}");
    CHECK_EQ(cases::get(f.api, "/cal/offsets/1").body, "0.5");
    CHECK_EQ(cases::get(f.api, "/cal/offsets/4").status, Status::NotFound);
    CHECK_EQ(cases::get(f.api, "/cal/offsets/01").status, Status::NotFound);
    CHECK_EQ(cases::get(f.api, "/cal/offsets", cases::schema()).body,
             "{\"type\":\"array\",\"writable\":true,\"maxSize\":4,\"fixed\":true,"
             "\"items\":{\"type\":\"number\",\"min\":-5,\"max\":5},\"persist\":true}");
    CHECK_EQ(cases::get(f.api, "/cal/days", cases::schema()).body,
             "{\"type\":\"array\",\"writable\":true,\"maxSize\":7,\"items\":{\"type\":\"integer\",\"min\":0,\"max\":6}}");
    CHECK_EQ(cases::get(f.api, "/cal/fixed", cases::schema()).body,
             "{\"type\":\"array\",\"maxSize\":64,\"items\":{\"type\":\"integer\"}}");
    // An element's schema is a value's.
    CHECK_EQ(cases::get(f.api, "/cal/days/0", cases::schema()).body,
             "{\"type\":\"integer\",\"writable\":true,\"min\":0,\"max\":6}");
}

TEST(array_write) {
    Fixture f;
    // Whole: a resizable array takes any length up to maxSize.
    Resp r = cases::set(f.api, "/cal/days", "[0,3,6]");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "[0,3,6]");
    CHECK_EQ(f.days.size(), 3u);
    CHECK_EQ(cases::set(f.api, "/cal/days", "[]").body, "[]");
    CHECK_EQ(cases::set(f.api, "/cal/days", "[0,1,2,3,4,5,6,0]").status, Status::InvalidValue);
    // A fixed one only its own length.
    CHECK_EQ(cases::set(f.api, "/cal/offsets", "[1,2,3]").status, Status::InvalidValue);
    CHECK_EQ(cases::set(f.api, "/cal/offsets", "[1,2,3,4]").body, "[1,2,3,4]");
    CHECK_EQ(f.offsets[3], 4.0f);
    // Each element is checked; the error names it, and nothing changes.
    r = cases::set(f.api, "/cal/offsets", "[0,0,9,0]");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK_EQ(r.body, "{\"error\":\"invalid_value\",\"path\":\"/cal/offsets/2\",\"message\":\"out of range\"}");
    CHECK_EQ(f.offsets[0], 1.0f);
    CHECK_EQ(cases::set(f.api, "/cal/rgb", "[1,2,300]").status, Status::InvalidValue);
    CHECK_EQ(cases::set(f.api, "/cal/tags", "[\"x\",1]").status, Status::InvalidValue);
    CHECK_EQ(cases::set(f.api, "/cal/days", "{\"0\":1}").status, Status::InvalidValue);
    // One element.
    CHECK_EQ(cases::set(f.api, "/cal/offsets/1", "-2.5").body, "-2.5");
    CHECK_EQ(f.offsets[1], -2.5f);
    CHECK_EQ(cases::set(f.api, "/cal/offsets/1", "-6").status, Status::InvalidValue);
    // In a patch.
    r = cases::set(f.api, "/cal", "{\"rgb\":[1,2,3],\"tags\":[\"p\",\"q\"]}");
    CHECK_EQ(r.body, "{\"rgb\":[1,2,3],\"tags\":[\"p\",\"q\"]}");
    CHECK_EQ(f.tags.size(), 2u);
    // Read-only.
    CHECK_EQ(cases::set(f.api, "/cal/fixed", "[1]").status, Status::ReadOnly);
    CHECK_EQ(cases::set(f.api, "/cal/fixed/0", "1").status, Status::ReadOnly);
}

TEST(array_changes_and_persistence) {
    Fixture f;
    subscription_cases::FakeSubscriber s;
    CHECK_EQ(subscription_cases::subscribe(f.api, &s, "/cal", cases::keys("offsets")).status, Status::Ok);
    CHECK_EQ(subscription_cases::subscribe(f.api, &s, "/cal/offsets/1").status, Status::NotAllowed);
    // An element write marks the array: notifications carry it whole.
    cases::set(f.api, "/cal/offsets/2", "3");
    f.api.poll(1000);
    CHECK_EQ(s.last(), "{\"op\":\"change\",\"path\":\"/cal\",\"body\":{\"offsets\":[0,0.5,3,1.5]}}");

    tesser::MemoryStorage storage;
    f.api.persistence(storage);
    CHECK(f.api.save());
    CHECK_EQ(storage.records["cal"], "{\"offsets\":[0,0.5,3,1.5]}");
    Fixture g;
    g.api.persistence(storage);
    CHECK(g.api.load());
    CHECK_EQ(g.offsets[2], 3.0f);
    // A stored array that no longer fits keeps the defaults.
    storage.records["cal"] = "{\"offsets\":[1,2]}";
    Fixture h;
    h.api.persistence(storage);
    h.api.load();
    CHECK_EQ(h.offsets[1], 0.5f);
}

}  // namespace array_cases

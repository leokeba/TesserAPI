// Lists of described objects.
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"
#include "subscription_cases.h"

namespace list_cases {

using cases::get;
using cases::set;
using tesser::Api;
using tesser::Status;

struct Remote {
    std::string host;
    int port = 80;
    bool enabled = true;
};

inline void describe(tesser::Object& o, Remote& r) {
    o.value("host", r.host);
    o.value("port", r.port).range(1, 65535);
    o.value("enabled", r.enabled);
    o.action("disable", [&r] { r.enabled = false; });
}

struct Fixture {
    std::vector<Remote> remotes{{"a", 80, true}, {"b", 81, false}};
    Api api;
    tesser::ListNode* list;
    Fixture() { list = &api.list("remotes", remotes).maxSize(3); }
};

TEST(list_get) {
    Fixture f;
    CHECK_EQ(get(f.api, "/remotes").body,
             "[{\"host\":\"a\",\"port\":80,\"enabled\":true},{\"host\":\"b\",\"port\":81,\"enabled\":false}]");
    CHECK_EQ(get(f.api, "/remotes/1/port").body, "81");
    CHECK_EQ(get(f.api, "/remotes/1").body, "{\"host\":\"b\",\"port\":81,\"enabled\":false}");
    CHECK_EQ(get(f.api, "/remotes/1", cases::keys("host")).body, "{\"host\":\"b\"}");
    CHECK_EQ(get(f.api, "/remotes/2").status, Status::NotFound);
    CHECK_EQ(get(f.api, "/remotes/01").status, Status::NotFound);
    CHECK_EQ(get(f.api, "/remotes/x").status, Status::NotFound);
    // A list is one value: whole at any depth, its elements' objects included.
    CHECK_EQ(get(f.api, "/", cases::depth(1)).body,
             "{\"remotes\":[{\"host\":\"a\",\"port\":80,\"enabled\":true},{\"host\":\"b\",\"port\":81,\"enabled\":false}]}");
    CHECK_EQ(get(f.api, "/", cases::depth(0)).body, "{}");
    // Elements' nested objects too, and in a subscription's snapshot, as in
    // its change notifications.
    cases::DeepTree t;
    const char* whole = "{\"networks\":[{\"ssid\":\"home\",\"static\":{\"dhcp\":true,\"addr\":{\"ip\":0}}}]}";
    CHECK_EQ(get(t.api, "/a/b/c/d/e/f/g/h", cases::depth(1)).body, whole);
    subscription_cases::FakeSubscriber s;
    CHECK_EQ(subscription_cases::subscribe(t.api, &s, "/a/b/c/d/e/f/g/h", cases::depth(1)).body, whole);
    // TesserUI reads a list's schema on its own path: the element schema
    // comes in full, nested objects included.
    CHECK_EQ(get(t.api, "/a/b/c/d/e/f/g/h/networks", cases::schema(16)).body,
             "{\"type\":\"list\",\"maxSize\":32,\"key\":\"ssid\",\"items\":{\"type\":\"object\",\"children\":{"
             "\"ssid\":{\"type\":\"string\",\"writable\":true},\"static\":{\"type\":\"object\",\"children\":{"
             "\"dhcp\":{\"type\":\"boolean\",\"writable\":true},\"addr\":{\"type\":\"object\",\"children\":{"
             "\"ip\":{\"type\":\"integer\",\"writable\":true}}}}}}}}");
    CHECK_EQ(get(f.api, "/remotes", cases::keys("host")).status, Status::BadRequest);
    CHECK_EQ(get(f.api, "/remotes", cases::schema()).body,
             "{\"type\":\"list\",\"maxSize\":3,\"items\":{\"type\":\"object\",\"children\":{"
             "\"host\":{\"type\":\"string\",\"writable\":true},"
             "\"port\":{\"type\":\"integer\",\"writable\":true,\"min\":1,\"max\":65535},"
             "\"enabled\":{\"type\":\"boolean\",\"writable\":true},"
             "\"disable\":{\"type\":\"action\"}}}}");
}

TEST(list_set_elements) {
    Fixture f;
    CHECK_EQ(set(f.api, "/remotes/0/port", "8080").body, "8080");
    CHECK_EQ(f.remotes[0].port, 8080);
    CHECK_EQ(set(f.api, "/remotes/0/port", "0").status, Status::InvalidValue);
    CHECK_EQ(set(f.api, "/remotes/1", "{\"enabled\":true,\"host\":\"c\"}").body, "{\"enabled\":true,\"host\":\"c\"}");
    CHECK_EQ(f.remotes[1].host, "c");
    CHECK_EQ(set(f.api, "/remotes/1/disable", nullptr).status, Status::Ok);
    CHECK(!f.remotes[1].enabled);
}

TEST(list_replace) {
    Fixture f;
    // Shorter: truncates; listed elements are patched in place.
    cases::Resp r = set(f.api, "/remotes", "[{\"host\":\"z\"}]");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "[{\"host\":\"z\",\"port\":80,\"enabled\":true}]");
    CHECK_EQ(f.remotes.size(), 1u);
    // Longer: new elements start from defaults.
    r = set(f.api, "/remotes", "[{},{\"port\":9}]");
    CHECK_EQ(r.body, "[{\"host\":\"z\",\"port\":80,\"enabled\":true},{\"host\":\"\",\"port\":9,\"enabled\":true}]");
    // Validated first: a bad element changes nothing.
    r = set(f.api, "/remotes", "[{\"port\":1},{\"port\":70000},{}]");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("\"path\":\"/remotes/1/port\"") != std::string::npos);
    CHECK_EQ(f.remotes.size(), 2u);
    CHECK_EQ(f.remotes[0].port, 80);
    CHECK_EQ(set(f.api, "/remotes", "[{},{},{},{}]").status, Status::InvalidValue);  // maxSize 3
    CHECK_EQ(set(f.api, "/remotes", "[1]").status, Status::InvalidValue);
    CHECK_EQ(set(f.api, "/remotes", "{}").status, Status::InvalidValue);
    CHECK_EQ(set(f.api, "/remotes", "[{\"nope\":1}]").status, Status::NotFound);
    // Inside a patch of the parent.
    r = set(f.api, "/", "{\"remotes\":[{\"port\":5}]}");
    CHECK_EQ(r.body, "{\"remotes\":[{\"host\":\"z\",\"port\":5,\"enabled\":true}]}");
    CHECK_EQ(set(f.api, "/", "{\"remotes\":[]}").body, "{\"remotes\":[]}");
    CHECK(f.remotes.empty());
}

TEST(list_subscriptions_and_persistence) {
    Fixture f;
    subscription_cases::FakeSubscriber s;
    CHECK_EQ(subscription_cases::subscribe(f.api, &s, "/").status, Status::Ok);
    CHECK_EQ(subscription_cases::subscribe(f.api, &s, "/remotes/0").status, Status::NotAllowed);
    set(f.api, "/remotes/1/port", "99");
    f.api.poll(1000);
    CHECK_EQ(s.last(),
             "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"remotes\":[{\"host\":\"a\",\"port\":80,\"enabled\":true},"
             "{\"host\":\"b\",\"port\":99,\"enabled\":false}]}}");

    tesser::MemoryStorage storage;
    f.list->persist();
    f.api.persistence(storage);
    CHECK(f.api.save());
    CHECK_EQ(storage.records["remotes"],
             "[{\"host\":\"a\",\"port\":80,\"enabled\":true},{\"host\":\"b\",\"port\":99,\"enabled\":false}]");

    Fixture g;
    g.remotes.clear();
    g.list->persist();
    storage.records["remotes"] = "[{\"host\":\"x\",\"port\":0},{\"host\":\"y\"},{},{},{}]";
    g.api.persistence(storage);
    CHECK(g.api.load());
    CHECK_EQ(g.remotes.size(), 3u);  // capped at maxSize
    if (g.remotes.size() == 3) {
        CHECK_EQ(g.remotes[0].host, "x");
        CHECK_EQ(g.remotes[0].port, 80);  // 0 is out of range: default kept
        CHECK_EQ(g.remotes[1].host, "y");
    }
}

TEST(list_with_lambda_describe) {
    Api api;
    std::vector<int> levels{1, 2};
    // Any element type works through a lambda, here a plain int.
    api.list("levels", levels, [](tesser::Object& o, int& v) { o.value("v", v).range(0, 10); });
    CHECK_EQ(get(api, "/levels").body, "[{\"v\":1},{\"v\":2}]");
    CHECK_EQ(set(api, "/levels/1/v", "7").body, "7");
    CHECK_EQ(levels[1], 7);
}

}  // namespace list_cases

// Keyed lists (docs/DESIGN.md section 4.4).
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"
#include "subscription_cases.h"

namespace keyed_cases {

using cases::Resp;
using tesser::Api;
using tesser::Status;

struct Source {
    std::string name;
    float az = 180;
    float el = 30;
};

struct Network {
    std::string ssid;
    std::string password;
    int priority = 0;
};

struct Fixture {
    Api api;
    std::vector<Source> sources = {{"Sun", 120, 45}, {"Moon", 200, 20}};
    std::vector<Network> networks = {{"home", "secret1", 1}, {"work", "secret2", 2}};
    tesser::ListNode* sourcesNode;

    Fixture() {
        sourcesNode = &api.list("sources", sources, [](tesser::Object& o, Source& s) {
                              o.value("name", s.name);
                              o.value("az", s.az).range(0, 360);
                              o.value("el", s.el);
                          }).key("name").maxSize(4);
        api.list("networks", networks, [](tesser::Object& o, Network& n) {
               o.value("ssid", n.ssid);
               o.value("password", n.password).secret();
               o.value("priority", n.priority);
           }).key("ssid");
    }
};

TEST(keyed_read) {
    Fixture f;
    CHECK_EQ(cases::get(f.api, "/sources").body,
             "[{\"name\":\"Sun\",\"az\":120,\"el\":45},{\"name\":\"Moon\",\"az\":200,\"el\":20}]");
    CHECK_EQ(cases::get(f.api, "/sources/Moon/az").body, "200");
    CHECK_EQ(cases::get(f.api, "/sources/Moon").body, "{\"name\":\"Moon\",\"az\":200,\"el\":20}");
    // Keys, not indexes.
    CHECK_EQ(cases::get(f.api, "/sources/0").status, Status::NotFound);
    Resp r = cases::get(f.api, "/sources/Mars/az");
    CHECK_EQ(r.status, Status::NotFound);
    CHECK(r.body.find("\"path\":\"/sources/Mars\"") != std::string::npos);
    CHECK_EQ(cases::get(f.api, "/sources", cases::schema(0)).body, "{\"type\":\"list\",\"maxSize\":4,\"key\":\"name\"}");
    // Secrets stay hidden in elements.
    CHECK_EQ(cases::get(f.api, "/networks/home").body, "{\"ssid\":\"home\",\"password\":null,\"priority\":1}");
}

TEST(keyed_element_writes) {
    Fixture f;
    CHECK_EQ(cases::set(f.api, "/sources/Moon/az", "210").body, "210");
    CHECK_EQ(f.sources[1].az, 210.0f);
    CHECK_EQ(cases::set(f.api, "/sources/Moon", "{\"el\":25}").status, Status::Ok);
    CHECK_EQ(f.sources[1].el, 25.0f);
    // Renaming: writing the key keeps the element's other fields.
    CHECK_EQ(cases::set(f.api, "/sources/Moon/name", "\"Luna\"").body, "\"Luna\"");
    CHECK_EQ(f.sources[1].name, "Luna");
    CHECK_EQ(f.sources[1].az, 210.0f);
    CHECK_EQ(cases::get(f.api, "/sources/Luna/az").body, "210");
    CHECK_EQ(cases::set(f.api, "/networks/home/ssid", "\"house\"").status, Status::Ok);
    CHECK_EQ(f.networks[0].password, "secret1");
    // Through an element patch too.
    CHECK_EQ(cases::set(f.api, "/sources/Luna", "{\"name\":\"Moon\",\"el\":1}").status, Status::Ok);
    CHECK_EQ(f.sources[1].name, "Moon");
    // A key must stay valid and unique.
    Resp r = cases::set(f.api, "/sources/Moon/name", "\"Sun\"");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK_EQ(r.body, "{\"error\":\"invalid_value\",\"path\":\"/sources/Moon/name\",\"message\":\"key already used\"}");
    CHECK_EQ(cases::set(f.api, "/sources/Moon/name", "\"\"").status, Status::InvalidValue);
    CHECK_EQ(cases::set(f.api, "/sources/Moon/name", "\"a/b\"").status, Status::InvalidValue);
    CHECK_EQ(cases::set(f.api, "/sources/Moon", "{\"name\":\"Sun\",\"el\":2}").status, Status::InvalidValue);
    CHECK_EQ(f.sources[1].el, 1.0f);  // validated first: nothing applied
    // Writing the same key again is fine.
    CHECK_EQ(cases::set(f.api, "/sources/Moon/name", "\"Moon\"").status, Status::Ok);
}

TEST(keyed_replace_matches_keys) {
    Fixture f;
    // Removing the first network and editing the second: the second keeps its
    // password (which a client can't read back) because it is matched by key.
    Resp r = cases::set(f.api, "/networks", "[{\"ssid\":\"work\",\"priority\":5},{\"ssid\":\"cafe\"}]");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "[{\"ssid\":\"work\",\"password\":null,\"priority\":5},{\"ssid\":\"cafe\",\"password\":null,\"priority\":0}]");
    CHECK_EQ(f.networks.size(), 2u);
    if (f.networks.size() == 2) {
        CHECK_EQ(f.networks[0].ssid, "work");
        CHECK_EQ(f.networks[0].password, "secret2");
        CHECK_EQ(f.networks[0].priority, 5);
        CHECK_EQ(f.networks[1].ssid, "cafe");
        CHECK_EQ(f.networks[1].password, "");  // new: defaults
    }
    // The array's order is the list's.
    CHECK_EQ(cases::set(f.api, "/sources", "[{\"name\":\"Moon\"},{\"name\":\"Sun\",\"el\":50}]").status, Status::Ok);
    CHECK(f.sources.size() == 2 && f.sources[0].name == "Moon" && f.sources[0].az == 200.0f && f.sources[1].el == 50.0f);
    // Inside a parent patch.
    CHECK_EQ(cases::set(f.api, "/", "{\"sources\":[{\"name\":\"Sun\"}]}").status, Status::Ok);
    CHECK(f.sources.size() == 1 && f.sources[0].az == 120.0f);
}

TEST(keyed_replace_validation) {
    Fixture f;
    Resp r = cases::set(f.api, "/sources", "[{\"name\":\"Sun\"},{\"az\":1}]");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK_EQ(r.body, "{\"error\":\"invalid_value\",\"path\":\"/sources/1/name\",\"message\":\"key must be a string\"}");
    r = cases::set(f.api, "/sources", "[{\"name\":\"Sun\"},{\"name\":\"Sun\"}]");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("duplicate key") != std::string::npos);
    r = cases::set(f.api, "/sources", "[{\"name\":\"Moon\",\"az\":999}]");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("\"path\":\"/sources/Moon/az\"") != std::string::npos);
    CHECK_EQ(cases::set(f.api, "/sources", "[{\"name\":\"a\"},{\"name\":\"b\"},{\"name\":\"c\"},{\"name\":\"d\"},{\"name\":\"e\"}]")
                 .status,
             Status::InvalidValue);
    CHECK_EQ(cases::set(f.api, "/sources", "[1]").status, Status::InvalidValue);
    // Nothing changed.
    CHECK(f.sources.size() == 2 && f.sources[0].name == "Sun");
}

TEST(keyed_persistence) {
    Fixture f;
    f.sourcesNode->persist();
    tesser::MemoryStorage storage;
    f.api.persistence(storage);
    cases::set(f.api, "/sources/Moon/name", "\"Luna\"");
    CHECK(f.api.save());
    CHECK_EQ(storage.records["sources"], "[{\"name\":\"Sun\",\"az\":120,\"el\":45},{\"name\":\"Luna\",\"az\":200,\"el\":20}]");

    // Loading matches by key too; items without a valid key are skipped.
    storage.records["sources"] = "[{\"name\":\"Moon\",\"el\":5},{\"az\":1},{\"name\":\"Venus\",\"az\":10}]";
    Fixture g;
    g.sourcesNode->persist();
    g.api.persistence(storage);
    CHECK(g.api.load());
    CHECK_EQ(g.sources.size(), 2u);
    if (g.sources.size() == 2) {
        CHECK_EQ(g.sources[0].name, "Moon");
        CHECK_EQ(g.sources[0].az, 200.0f);  // the default element's, kept
        CHECK_EQ(g.sources[0].el, 5.0f);
        CHECK_EQ(g.sources[1].name, "Venus");
    }
}

TEST(keyed_declaration_error) {
    int before = tesser::declarationErrors();
    Api api;
    std::vector<Source> s;
    auto& l = api.list("s", s, [](tesser::Object& o, Source& x) { o.value("az", x.az); }).key("az");
    CHECK_EQ(tesser::declarationErrors(), before + 1);
    CHECK(l.keyField() == nullptr);  // stays an indexed list
}

}  // namespace keyed_cases

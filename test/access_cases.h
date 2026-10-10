// Access levels and secret values (docs/DESIGN.md sections 4.1 and 13).
#pragma once

#include <string>

#include "core_cases.h"
#include "subscription_cases.h"

namespace access_cases {

using cases::Resp;
using tesser::Access;
using tesser::Api;
using tesser::Op;
using tesser::Query;
using tesser::Status;

inline Resp request(Api& api, Access level, Op op, const char* path, const char* body = nullptr, Query q = Query(),
                    tesser::Subscriber* sub = nullptr) {
    JsonDocument doc;
    tesser::Request req;
    req.op = op;
    req.path = path;
    req.query = q;
    req.client.access = level;
    req.subscriber = sub;
    if (body) {
        if (deserializeJson(doc, body)) return {Status::Internal, std::string("test body is not JSON: ") + body};
        req.body = doc.as<JsonVariantConst>();
    }
    tesser::StringReply reply;
    api.handle(req, reply);
    return {reply.status, reply.body};
}

inline Resp get(Api& api, Access level, const char* path, Query q = Query(), const char* shape = nullptr) {
    return request(api, level, Op::Get, path, shape, q);
}

inline Resp set(Api& api, Access level, const char* path, const char* body) {
    return request(api, level, Op::Set, path, body);
}

// A device with settings only admins change, and accounts only admins see.
struct Device {
    Api api;
    int brightness = 10;
    int port = 80;
    std::string wifiPassword = "hunter22";
    std::string adminPassword = "root";
    bool rebooted = false;
    tesser::EventNode* login = nullptr;

    Device() {
        api.value("brightness", brightness);
        auto& net = api.object("net").writeAccess(Access::Admin);
        net.value("port", port);
        net.value("password", wifiPassword).secret().persist();
        net.action("reboot", [this] { rebooted = true; });
        auto& users = api.object("users").readAccess(Access::Admin);
        users.value("admin", adminPassword).secret();
        login = &users.event("login");
    }
};

TEST(access_filters_reads) {
    Device d;
    CHECK_EQ(get(d.api, Access::Public, "/").body, "{\"brightness\":10,\"net\":{\"port\":80,\"password\":null}}");
    CHECK_EQ(get(d.api, Access::Admin, "/").body,
             "{\"brightness\":10,\"net\":{\"port\":80,\"password\":null},\"users\":{\"admin\":null}}");
    Resp r = get(d.api, Access::User, "/users/admin");
    CHECK_EQ(r.status, Status::Unauthorized);
    CHECK_EQ(r.body, "{\"error\":\"unauthorized\",\"path\":\"/users/admin\",\"message\":\"not authorized\"}");
    CHECK_EQ(get(d.api, Access::User, "/", cases::keys("users")).status, Status::Unauthorized);
    CHECK_EQ(get(d.api, Access::User, "/", Query(), "{\"users\":null}").status, Status::Unauthorized);
    CHECK_EQ(get(d.api, Access::User, "/", cases::keys("net")).body, "{\"net\":{\"port\":80,\"password\":null}}");

    // The schema leaves out what the client can't read, and says what
    // writing takes.
    Resp s = get(d.api, Access::User, "/", cases::schema());
    CHECK(s.body.find("\"users\"") == std::string::npos);
    CHECK(s.body.find("\"net\":{\"type\":\"object\",\"access\":\"admin\"") != std::string::npos);
    CHECK(s.body.find("\"password\":{\"type\":\"string\",\"writable\":true,\"secret\":true,\"persist\":true}") !=
          std::string::npos);
    CHECK(get(d.api, Access::Admin, "/", cases::schema()).body.find("\"users\":{\"type\":\"object\",\"access\":\"admin\"") !=
          std::string::npos);
}

TEST(access_guards_writes) {
    Device d;
    CHECK_EQ(set(d.api, Access::Public, "/brightness", "20").status, Status::Ok);
    CHECK_EQ(set(d.api, Access::User, "/net/port", "8080").status, Status::Unauthorized);
    CHECK_EQ(set(d.api, Access::User, "/net/reboot", nullptr).status, Status::Unauthorized);
    CHECK(!d.rebooted);
    // A patch is refused whole when one of its keys needs more.
    Resp r = set(d.api, Access::User, "/", "{\"brightness\":30,\"net\":{\"port\":8080}}");
    CHECK_EQ(r.status, Status::Unauthorized);
    CHECK_EQ(r.body, "{\"error\":\"unauthorized\",\"path\":\"/net\",\"message\":\"not authorized\"}");
    CHECK_EQ(d.brightness, 20);
    CHECK_EQ(set(d.api, Access::Admin, "/", "{\"brightness\":30,\"net\":{\"port\":8080,\"reboot\":null}}").status,
             Status::Ok);
    CHECK_EQ(d.port, 8080);
    CHECK(d.rebooted);
}

TEST(access_levels_of_clients) {
    tesser::Client c;
    CHECK(c.level() == Access::Public);
    c.authenticated = true;  // transports that only know "authenticated"
    CHECK(c.level() == Access::Admin);
    c.access = Access::User;
    CHECK(c.level() == Access::User);

    Api api;
    CHECK(api.tokenAccess("anything") == Access::Public);
    api.authenticate([](std::string_view token) { return token == "u" ? Access::User : Access::Public; });
    CHECK(api.tokenAccess("u") == Access::User);
    CHECK(api.tokenAccess("") == Access::Public);

    // readAccess raises the write level with it; writeAccess never lowers
    // it below the read level.
    int v = 0;
    auto& n = api.value("v", v).readAccess(Access::User);
    CHECK(n.writeAccess() == Access::User);
    n.writeAccess(Access::Public);
    CHECK(n.writeAccess() == Access::User);
    n.writeAccess(Access::Admin);
    CHECK(n.readAccess() == Access::User);
    CHECK(n.writeAccess() == Access::Admin);
}

TEST(secret_values) {
    Device d;
    Resp r = set(d.api, Access::Admin, "/net/password", "\"correct horse\"");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "null");  // never read back
    CHECK_EQ(d.wifiPassword, "correct horse");
    CHECK_EQ(set(d.api, Access::Admin, "/net", "{\"password\":\"x\"}").body, "{\"password\":null}");
    // Persistence keeps the real value.
    CHECK_EQ(d.api.persistedState(), "{\"net\":{\"password\":\"x\"}}");
}

TEST(access_filters_notifications_and_events) {
    Device d;
    subscription_cases::FakeSubscriber user, admin;
    CHECK_EQ(request(d.api, Access::User, Op::Subscribe, "/", nullptr, Query(), &user).body,
             "{\"brightness\":10,\"net\":{\"port\":80,\"password\":null}}");
    CHECK_EQ(request(d.api, Access::Admin, Op::Subscribe, "/", nullptr, Query(), &admin).status, Status::Ok);
    CHECK_EQ(request(d.api, Access::User, Op::Subscribe, "/users", nullptr, Query(), &user).status,
             Status::Unauthorized);

    d.adminPassword = "changed";
    d.wifiPassword = "changed";
    d.api.changed("/users");
    d.api.changed("/net/password");
    d.api.poll(1000);
    CHECK_EQ(user.last(), "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"net\":{\"password\":null}}}");
    CHECK_EQ(admin.last(),
             "{\"op\":\"change\",\"path\":\"/\",\"body\":{\"net\":{\"password\":null},\"users\":{\"admin\":null}}}");

    size_t before = user.messages.size();
    d.login->emit("admin");
    CHECK_EQ(user.messages.size(), before);
    CHECK_EQ(admin.last(), "{\"op\":\"event\",\"path\":\"/users/login\",\"body\":\"admin\"}");
    d.api.dropSubscriber(&user);
    d.api.dropSubscriber(&admin);
}

}  // namespace access_cases

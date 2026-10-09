// Core test cases, compiled into both the host test binary and the on-target
// firmware (test/hardware). Keep them free of platform code.
#pragma once

#include <math.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "TesserAPI.h"
#include "check.h"

namespace cases {

using tesser::Api;
using tesser::Query;
using tesser::Status;
using tesser::View;

struct Resp {
    Status status;
    std::string body;
};

inline Resp request(Api& api, tesser::Op op, const char* path, const char* body = nullptr, Query q = Query()) {
    JsonDocument doc;
    tesser::Request req;
    req.op = op;
    req.path = path;
    req.query = q;
    if (body) {
        DeserializationError err = deserializeJson(doc, body);
        if (err) return {Status::Internal, std::string("test body is not JSON: ") + body};
        req.body = doc.as<JsonVariantConst>();
    }
    tesser::StringReply reply;
    api.handle(req, reply);
    if (!reply.begun || !reply.ended) return {Status::Internal, "reply not completed"};
    return {reply.status, reply.body};
}

inline Resp get(Api& api, const char* path, Query q = Query(), const char* shape = nullptr) {
    return request(api, tesser::Op::Get, path, shape, q);
}

inline Resp set(Api& api, const char* path, const char* body) { return request(api, tesser::Op::Set, path, body); }

inline Query keys(const char* k) {
    Query q;
    q.keys = k;
    return q;
}

inline Query depth(int d) {
    Query q;
    q.depth = d;
    return q;
}

inline Query schema(int d = Query::kUnlimited) {
    Query q;
    q.view = View::Schema;
    q.depth = d;
    return q;
}

inline std::string writeJson(void (*fn)(tesser::JsonWriter&)) {
    std::string out;
    tesser::StringSink sink(out);
    tesser::JsonWriter w(sink);
    fn(w);
    return out;
}

// ---- A small device used by most tests -----------------------------------

struct Lamp {
    bool on = false;
    int brightness = 128;
    char label[8] = "desk";
};

struct Device {
    Lamp lamp;
    float temperature = 21.5f;
    double gain = 0.25;
    uint8_t level = 3;
    int64_t big = -9007199254740993LL;
    uint64_t huge = 18446744073709551615ULL;
    std::string owner = "leo";
    const int serial = 4242;
    int toggles = 0;
    std::vector<std::string> log;
    Api api;

    Device() {
        auto& l = api.object("lamp");
        l.value("on", lamp.on);
        l.value("brightness", lamp.brightness).range(0, 255).doc("PWM duty");
        l.value("label", lamp.label);
        l.action("toggle", [this] {
            lamp.on = !lamp.on;
            toggles++;
        });
        auto& s = api.object("sensors");
        s.value("temperature", [this] { return temperature; });
        s.value("serial", serial);
        auto& c = api.object("config");
        c.value("gain", gain).persist();
        c.value("level", level);
        c.value("owner", owner);
        c.value("version", "1.2.0");
        auto& n = c.object("numbers");
        n.value("big", big);
        n.value("huge", huge);
    }
};

// ---- JSON writer -------------------------------------------------------

TEST(writer_structure) {
    std::string s = writeJson([](tesser::JsonWriter& w) {
        w.beginObject();
        w.key("a");
        w.integer(1);
        w.key("b");
        w.beginArray();
        w.boolean(true);
        w.null();
        w.beginObject();
        w.endObject();
        w.endArray();
        w.key("c");
        w.beginObject();
        w.key("d");
        w.string("x");
        w.endObject();
        w.endObject();
    });
    CHECK_EQ(s, "{\"a\":1,\"b\":[true,null,{}],\"c\":{\"d\":\"x\"}}");
}

TEST(writer_escapes) {
    std::string s = writeJson([](tesser::JsonWriter& w) {
        w.beginObject();
        w.key("q\"k");
        w.string(std::string_view("a\"b\\c\n\t\x01z\0", 10));
        w.endObject();
    });
    CHECK_EQ(s, "{\"q\\\"k\":\"a\\\"b\\\\c\\n\\t\\u0001z\\u0000\"}");
}

TEST(writer_numbers) {
    std::string s = writeJson([](tesser::JsonWriter& w) {
        w.beginArray();
        w.integer(INT64_MIN);
        w.uinteger(UINT64_MAX);
        w.integer(0);
        w.number(0.1f, 7);
        w.number(22.7);
        w.number(NAN);
        w.number(INFINITY);
        w.number(-1.5e300);
        w.endArray();
    });
    CHECK_EQ(s, "[-9223372036854775808,18446744073709551615,0,0.1,22.7,null,null,-1.5e+300]");
}

TEST(writer_sink_failure_stops_output) {
    std::string out;
    tesser::StringSink sink(out, 5);
    tesser::JsonWriter w(sink);
    w.beginArray();
    w.string("long string");
    w.endArray();
    CHECK(!w.ok());
    CHECK(out.size() <= 5);
}

// ---- get ---------------------------------------------------------------

TEST(get_root) {
    Device d;
    Resp r = get(d.api, "/");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body,
             "{\"lamp\":{\"on\":false,\"brightness\":128,\"label\":\"desk\"},"
             "\"sensors\":{\"temperature\":21.5,\"serial\":4242},"
             "\"config\":{\"gain\":0.25,\"level\":3,\"owner\":\"leo\",\"version\":\"1.2.0\","
             "\"numbers\":{\"big\":-9007199254740993,\"huge\":18446744073709551615}}}");
    CHECK_EQ(get(d.api, "").body, r.body);
}

TEST(get_paths) {
    Device d;
    CHECK_EQ(get(d.api, "/lamp/brightness").body, "128");
    CHECK_EQ(get(d.api, "/lamp/").body, get(d.api, "/lamp").body);
    CHECK_EQ(get(d.api, "/config/numbers/big").body, "-9007199254740993");

    Resp r = get(d.api, "/lamp/nope/deeper");
    CHECK_EQ(r.status, Status::NotFound);
    CHECK_EQ(r.body, "{\"error\":\"not_found\",\"path\":\"/lamp/nope\",\"message\":\"no such node\"}");

    r = get(d.api, "/lamp/on/x");
    CHECK_EQ(r.status, Status::NotFound);
    CHECK(r.body.find("\"path\":\"/lamp/on/x\"") != std::string::npos);

    CHECK_EQ(get(d.api, "/lamp//on").status, Status::BadRequest);
    CHECK_EQ(get(d.api, "lamp").status, Status::BadRequest);
}

TEST(get_depth) {
    Device d;
    CHECK_EQ(get(d.api, "/", depth(0)).body, "{}");
    CHECK_EQ(get(d.api, "/", depth(1)).body, "{\"lamp\":{},\"sensors\":{},\"config\":{}}");
    CHECK_EQ(get(d.api, "/config", depth(1)).body,
             "{\"gain\":0.25,\"level\":3,\"owner\":\"leo\",\"version\":\"1.2.0\",\"numbers\":{}}");
    CHECK_EQ(get(d.api, "/lamp/on", depth(0)).body, "false");
}

TEST(get_keys_and_exclude) {
    Device d;
    CHECK_EQ(get(d.api, "/lamp", keys("on, label")).body, "{\"on\":false,\"label\":\"desk\"}");
    Query q;
    q.exclude = "brightness";
    CHECK_EQ(get(d.api, "/lamp", q).body, "{\"on\":false,\"label\":\"desk\"}");

    Resp r = get(d.api, "/lamp", keys("on,nope"));
    CHECK_EQ(r.status, Status::NotFound);
    CHECK(r.body.find("\"path\":\"/lamp/nope\"") != std::string::npos);

    q.keys = "on";
    CHECK_EQ(get(d.api, "/lamp", q).status, Status::BadRequest);
    CHECK_EQ(get(d.api, "/lamp/on", keys("x")).status, Status::BadRequest);
}

TEST(get_shape) {
    Device d;
    Resp r = get(d.api, "/", Query(), "{\"config\":{\"numbers\":{\"huge\":null},\"owner\":true},\"lamp\":{\"on\":null}}");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "{\"config\":{\"numbers\":{\"huge\":18446744073709551615},\"owner\":\"leo\"},\"lamp\":{\"on\":false}}");

    CHECK_EQ(get(d.api, "/", Query(), "{\"sensors\":true}").body, "{\"sensors\":{\"temperature\":21.5,\"serial\":4242}}");

    r = get(d.api, "/", Query(), "{\"lamp\":{\"nope\":null}}");
    CHECK_EQ(r.status, Status::NotFound);
    CHECK(r.body.find("\"path\":\"/lamp/nope\"") != std::string::npos);

    CHECK_EQ(get(d.api, "/", Query(), "{\"lamp\":1}").status, Status::BadRequest);
    CHECK_EQ(get(d.api, "/", Query(), "{\"lamp\":{\"on\":{}}}").status, Status::BadRequest);
    CHECK_EQ(get(d.api, "/", Query(), "[1]").status, Status::BadRequest);
    CHECK_EQ(get(d.api, "/", keys("lamp"), "{\"lamp\":true}").status, Status::BadRequest);
}

TEST(get_action_value_not_allowed) {
    Device d;
    CHECK_EQ(get(d.api, "/lamp/toggle").status, Status::NotAllowed);
    CHECK_EQ(get(d.api, "/lamp/toggle", schema()).body, "{\"type\":\"action\"}");
}

// ---- schema ------------------------------------------------------------

TEST(schema_view) {
    Device d;
    Resp r = get(d.api, "/lamp", schema());
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body,
             "{\"type\":\"object\",\"children\":{"
             "\"on\":{\"type\":\"boolean\",\"writable\":true},"
             "\"brightness\":{\"type\":\"integer\",\"writable\":true,\"min\":0,\"max\":255,\"description\":\"PWM duty\"},"
             "\"label\":{\"type\":\"string\",\"writable\":true,\"maxLength\":7},"
             "\"toggle\":{\"type\":\"action\"}}}");
    CHECK_EQ(get(d.api, "/sensors", schema()).body,
             "{\"type\":\"object\",\"children\":{\"temperature\":{\"type\":\"number\"},\"serial\":{\"type\":\"integer\"}}}");
    CHECK_EQ(get(d.api, "/", schema(1)).body,
             "{\"type\":\"object\",\"children\":{\"lamp\":{\"type\":\"object\"},\"sensors\":{\"type\":\"object\"},"
             "\"config\":{\"type\":\"object\"}}}");
    CHECK_EQ(get(d.api, "/config/gain", schema()).body, "{\"type\":\"number\",\"writable\":true,\"persist\":true}");
}

// ---- set ---------------------------------------------------------------

TEST(set_values) {
    Device d;
    Resp r = set(d.api, "/lamp/brightness", "200");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "200");
    CHECK_EQ(d.lamp.brightness, 200);

    CHECK_EQ(set(d.api, "/lamp/on", "true").body, "true");
    CHECK(d.lamp.on);
    CHECK_EQ(set(d.api, "/config/gain", "2").body, "2");
    CHECK_EQ(d.gain, 2.0);
    CHECK_EQ(set(d.api, "/config/owner", "\"ada\"").body, "\"ada\"");
    CHECK_EQ(d.owner, "ada");
    CHECK_EQ(set(d.api, "/lamp/label", "\"shelf\"").body, "\"shelf\"");
    CHECK_EQ(std::string(d.lamp.label), "shelf");
    CHECK_EQ(set(d.api, "/config/numbers/huge", "1").body, "1");
    CHECK_EQ(d.huge, 1u);
}

TEST(set_validation) {
    Device d;
    Resp r = set(d.api, "/lamp/brightness", "300");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK_EQ(r.body, "{\"error\":\"invalid_value\",\"path\":\"/lamp/brightness\",\"message\":\"out of range\"}");
    CHECK_EQ(d.lamp.brightness, 128);

    CHECK_EQ(set(d.api, "/lamp/brightness", "1.5").status, Status::InvalidValue);
    CHECK_EQ(set(d.api, "/lamp/brightness", "\"12\"").status, Status::InvalidValue);
    CHECK_EQ(set(d.api, "/lamp/on", "1").status, Status::InvalidValue);
    CHECK_EQ(set(d.api, "/config/level", "256").status, Status::InvalidValue);
    CHECK_EQ(set(d.api, "/config/level", "-1").status, Status::InvalidValue);
    CHECK_EQ(set(d.api, "/lamp/label", "\"12345678\"").status, Status::InvalidValue);
    CHECK_EQ(set(d.api, "/lamp/label", "\"1234567\"").status, Status::Ok);
    CHECK_EQ(d.level, 3);

    CHECK_EQ(set(d.api, "/sensors/temperature", "1").status, Status::ReadOnly);
    CHECK_EQ(set(d.api, "/sensors/serial", "1").status, Status::ReadOnly);
    CHECK_EQ(set(d.api, "/config/version", "\"2\"").status, Status::ReadOnly);
}

TEST(set_getter_setter) {
    Api api;
    double speed = 0;
    int writes = 0;
    api.value(
        "speed", [&] { return speed; },
        [&](double v) {
            writes++;
            if (v < 0) return false;
            speed = v;
            return true;
        });
    api.value(
        "mode", [&] { return std::string(speed > 1 ? "fast" : "slow"); },
        [&](const std::string& m) { return m == "fast" || m == "slow" ? Status::Ok : Status::InvalidValue; });

    CHECK_EQ(set(api, "/speed", "1.5").body, "1.5");
    CHECK_EQ(speed, 1.5);
    Resp r = set(api, "/speed", "-1");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("rejected") != std::string::npos);
    CHECK_EQ(speed, 1.5);
    CHECK_EQ(writes, 2);
    CHECK_EQ(get(api, "/", schema()).body,
             "{\"type\":\"object\",\"children\":{\"speed\":{\"type\":\"number\",\"writable\":true},"
             "\"mode\":{\"type\":\"string\",\"writable\":true}}}");
    CHECK_EQ(set(api, "/mode", "\"other\"").status, Status::InvalidValue);
}

TEST(set_patch) {
    Device d;
    Resp r = set(d.api, "/", "{\"lamp\":{\"on\":true,\"brightness\":10},\"config\":{\"level\":7}}");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "{\"lamp\":{\"on\":true,\"brightness\":10},\"config\":{\"level\":7}}");
    CHECK(d.lamp.on);
    CHECK_EQ(d.lamp.brightness, 10);
    CHECK_EQ(d.level, 7);
}

TEST(set_patch_is_validated_first) {
    Device d;
    Resp r = set(d.api, "/", "{\"lamp\":{\"on\":true,\"brightness\":999}}");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("\"path\":\"/lamp/brightness\"") != std::string::npos);
    CHECK(!d.lamp.on);

    r = set(d.api, "/lamp", "{\"on\":true,\"nope\":1}");
    CHECK_EQ(r.status, Status::NotFound);
    CHECK(r.body.find("\"path\":\"/lamp/nope\"") != std::string::npos);
    CHECK(!d.lamp.on);

    CHECK_EQ(set(d.api, "/", "{\"sensors\":{\"temperature\":3}}").status, Status::ReadOnly);
    CHECK_EQ(set(d.api, "/", "{\"lamp\":5}").status, Status::InvalidValue);
    CHECK_EQ(set(d.api, "/lamp", "5").status, Status::InvalidValue);
}

TEST(set_patch_runs_actions_after_values) {
    Device d;
    std::vector<std::string> order;
    int position = 0;
    auto& m = d.api.object("motor");
    m.value(
        "position", [&] { return position; },
        [&](int p) {
            order.push_back("set");
            position = p;
        });
    m.action("go", [&] { order.push_back("go@" + std::to_string(position)); });
    m.action("fail", [] { return Status::InvalidValue; });

    Resp r = set(d.api, "/motor", "{\"go\":null,\"position\":10}");
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "{\"go\":null,\"position\":10}");
    CHECK_EQ(order.size(), 2u);
    if (order.size() == 2) {
        CHECK_EQ(order[0], "set");
        CHECK_EQ(order[1], "go@10");
    }

    r = set(d.api, "/motor", "{\"position\":5,\"fail\":null}");
    CHECK_EQ(r.status, Status::InvalidValue);
    CHECK(r.body.find("\"partial\":true") != std::string::npos);
    CHECK(r.body.find("\"path\":\"/motor/fail\"") != std::string::npos);
    CHECK_EQ(position, 5);
}

TEST(set_rejects_query_options) {
    Device d;
    tesser::Query q;
    q.view = View::Schema;
    CHECK_EQ(request(d.api, tesser::Op::Set, "/lamp/on", "true", q).status, Status::BadRequest);
}

// ---- actions -----------------------------------------------------------

TEST(actions) {
    Device d;
    Resp r = set(d.api, "/lamp/toggle", nullptr);
    CHECK_EQ(r.status, Status::Ok);
    CHECK_EQ(r.body, "null");
    CHECK(d.lamp.on);
    CHECK_EQ(d.toggles, 1);

    Api api;
    float moved = 0;
    api.action("moveTo", [&](float deg) { moved = deg; });
    api.action("count", [] { return 42; });
    api.action("echo", [](JsonVariantConst v) { return v["n"].as<int>() + 1; });
    api.action("check", [](int v) { return v > 0 ? Status::Ok : Status::InvalidValue; });
    api.action("manual", [](tesser::Call& call) { call.reply(call.arg()["x"].as<int>() * 2); });

    CHECK_EQ(set(api, "/moveTo", "12.5").body, "null");
    CHECK_EQ(moved, 12.5f);
    CHECK_EQ(set(api, "/moveTo", "\"x\"").status, Status::InvalidValue);
    CHECK_EQ(set(api, "/count", nullptr).body, "42");
    CHECK_EQ(set(api, "/echo", "{\"n\":4}").body, "5");
    CHECK_EQ(set(api, "/check", "1").status, Status::Ok);
    CHECK_EQ(set(api, "/check", "-1").status, Status::InvalidValue);
    CHECK_EQ(set(api, "/manual", "{\"x\":21}").body, "42");
    CHECK_EQ(get(api, "/", schema()).body,
             "{\"type\":\"object\",\"children\":{"
             "\"moveTo\":{\"type\":\"action\",\"arg\":\"number\"},"
             "\"count\":{\"type\":\"action\",\"returns\":\"integer\"},"
             "\"echo\":{\"type\":\"action\",\"arg\":\"any\",\"returns\":\"integer\"},"
             "\"check\":{\"type\":\"action\",\"arg\":\"integer\"},"
             "\"manual\":{\"type\":\"action\",\"arg\":\"any\",\"deferred\":true}}}");
}

// ---- custom nodes ------------------------------------------------------

TEST(custom_nodes) {
    Api api;
    std::vector<int> items{1, 2};
    api.custom(
           "items",
           [&](tesser::JsonWriter& w) {
               w.beginArray();
               for (int i : items) w.integer(i);
               w.endArray();
           },
           [&](JsonVariantConst v) {
               items.clear();
               for (JsonVariantConst e : v.as<JsonArrayConst>()) items.push_back(e.as<int>());
               return Status::Ok;
           })
        .validate([](JsonVariantConst v) {
            return v.is<JsonArrayConst>() ? tesser::Check::ok()
                                          : tesser::Check::fail(Status::InvalidValue, "expected array");
        });
    api.custom("fixed", [](tesser::JsonWriter& w) { w.string("x"); });

    CHECK_EQ(get(api, "/").body, "{\"items\":[1,2],\"fixed\":\"x\"}");
    CHECK_EQ(set(api, "/items", "[3,4,5]").body, "[3,4,5]");
    CHECK_EQ(items.size(), 3u);
    CHECK_EQ(set(api, "/items", "7").status, Status::InvalidValue);
    CHECK_EQ(set(api, "/", "{\"items\":{}}").status, Status::InvalidValue);
    CHECK_EQ(set(api, "/fixed", "1").status, Status::ReadOnly);
    CHECK_EQ(get(api, "/", schema()).body,
             "{\"type\":\"object\",\"children\":{\"items\":{\"type\":\"custom\",\"writable\":true},"
             "\"fixed\":{\"type\":\"custom\"}}}");
}

// ---- composition -------------------------------------------------------

struct Motor {
    double speed = 0;
    bool invert = false;
};

inline void describe(tesser::Object& o, Motor& m) {
    o.value("speed", m.speed);
    o.value("invert", m.invert);
}

struct Pump {
    int rate = 5;
    void describe(tesser::Object& o) { o.value("rate", rate); }
};

TEST(mount) {
    Api api;
    Motor left, right;
    Pump pump;
    api.mount("left", left);
    api.mount("right", right);
    api.mount("pump", pump);
    CHECK_EQ(get(api, "/").body,
             "{\"left\":{\"speed\":0,\"invert\":false},\"right\":{\"speed\":0,\"invert\":false},\"pump\":{\"rate\":5}}");
    CHECK_EQ(set(api, "/right/speed", "3").body, "3");
    CHECK_EQ(right.speed, 3.0);
    CHECK_EQ(left.speed, 0.0);
}

TEST(declaration_errors) {
    Api api;
    int a = 1, b = 2;
    int before = tesser::declarationErrors();
    api.value("a", a);
    api.value("a", b);           // duplicate
    api.value("bad name", b);    // invalid character
    api.object("o").value("x", a);
    api.object("o").value("y", b);  // reuses the object
    CHECK_EQ(tesser::declarationErrors() - before, 2);
    CHECK_EQ(get(api, "/").body, "{\"a\":1,\"o\":{\"x\":1,\"y\":2}}");
}

// ---- envelope and line transport ----------------------------------------

struct Lines {
    std::vector<std::string> out;
    std::string partial;
    void add(const char* data, size_t len) {
        partial.append(data, len);
        size_t nl;
        while ((nl = partial.find('\n')) != std::string::npos) {
            out.push_back(partial.substr(0, nl));
            partial.erase(0, nl + 1);
        }
    }
};

inline std::string roundTrip(Api& api, const char* line) {
    Lines lines;
    tesser::LineTransport t(api, [&](const char* d, size_t n) { lines.add(d, n); });
    t.feed(line, strlen(line));
    t.feed('\n');
    return lines.out.size() == 1 ? lines.out[0] : "<" + std::to_string(lines.out.size()) + " lines>";
}

TEST(envelope_requests) {
    Device d;
    CHECK_EQ(roundTrip(d.api, "{\"id\":7,\"op\":\"get\",\"path\":\"/lamp\",\"keys\":\"on\"}"),
             "{\"id\":7,\"status\":\"ok\",\"body\":{\"on\":false}}");
    CHECK_EQ(roundTrip(d.api, "{\"id\":\"a\\\"b\",\"op\":\"set\",\"path\":\"/lamp/brightness\",\"body\":9}"),
             "{\"id\":\"a\\\"b\",\"status\":\"ok\",\"body\":9}");
    CHECK_EQ(d.lamp.brightness, 9);
    CHECK_EQ(roundTrip(d.api, "{\"op\":\"get\",\"path\":\"/\",\"depth\":1}"),
             "{\"status\":\"ok\",\"body\":{\"lamp\":{},\"sensors\":{},\"config\":{}}}");
    CHECK_EQ(roundTrip(d.api, "{\"op\":\"get\",\"path\":\"/sensors/serial\",\"view\":\"schema\"}"),
             "{\"status\":\"ok\",\"body\":{\"type\":\"integer\"}}");
    CHECK_EQ(roundTrip(d.api, "{\"id\":1,\"op\":\"get\",\"path\":\"/\",\"body\":{\"lamp\":{\"on\":null}}}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"lamp\":{\"on\":false}}}");
}

TEST(envelope_errors) {
    Device d;
    CHECK_EQ(roundTrip(d.api, "{\"id\":3,\"op\":\"get\",\"path\":\"/x\"}"),
             "{\"id\":3,\"status\":\"not_found\",\"body\":{\"error\":\"not_found\",\"path\":\"/x\",\"message\":\"no such node\"}}");
    CHECK_EQ(roundTrip(d.api, "{\"id\":3,\"op\":\"delete\"}"),
             "{\"id\":3,\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/\",\"message\":"
             "\"op must be \\\"get\\\", \\\"set\\\", \\\"sub\\\" or \\\"unsub\\\"\"}}");
    CHECK_EQ(roundTrip(d.api, "{\"op\":\"get\""),
             "{\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/\",\"message\":\"malformed JSON\"}}");
    CHECK(roundTrip(d.api, "{\"id\":{},\"op\":\"get\"}").find("id must be a scalar") != std::string::npos);
    CHECK(roundTrip(d.api, "{\"op\":\"get\",\"depth\":-1}").find("depth must be") != std::string::npos);
    CHECK(roundTrip(d.api, "{\"op\":\"get\",\"keys\":[\"a\"]}").find("keys must be") != std::string::npos);
}

TEST(envelope_response_limit) {
    Device d;
    d.api.config().maxResponse = 64;
    std::string r = roundTrip(d.api, "{\"id\":1,\"op\":\"get\",\"path\":\"/\"}");
    CHECK(r.find("\"status\":\"too_large\"") != std::string::npos);
    CHECK(r.size() <= 120);
    d.api.config().maxResponse = 0;  // unlimited
    CHECK(roundTrip(d.api, "{\"id\":1,\"op\":\"get\",\"path\":\"/\"}").find("\"status\":\"ok\"") != std::string::npos);
}

TEST(line_transport_framing) {
    Device d;
    Lines lines;
    tesser::LineTransport t(d.api, [&](const char* data, size_t n) { lines.add(data, n); });
    const char input[] =
        "I (123) boot: log noise\n"
        "\n"
        "\xff\x00\x13  {\"id\":1,\"op\":\"get\",\"path\":\"/lamp/on\"}\r\n"
        "{\"id\":2,\"op\":\"get\",\"path\":\"/lamp/brightness\"}\n";
    t.feed(input, sizeof(input) - 1);
    CHECK_EQ(lines.out.size(), 2u);
    if (lines.out.size() == 2) {
        CHECK_EQ(lines.out[0], "{\"id\":1,\"status\":\"ok\",\"body\":false}");
        CHECK_EQ(lines.out[1], "{\"id\":2,\"status\":\"ok\",\"body\":128}");
    }

    d.api.config().maxRequestBody = 16;
    lines.out.clear();
    std::string longLine = "{\"op\":\"get\",\"path\":\"/" + std::string(400, 'a') + "\"}\n";
    t.feed(longLine.data(), longLine.size());
    CHECK_EQ(lines.out.size(), 1u);
    if (!lines.out.empty()) CHECK(lines.out[0].find("too_large") != std::string::npos);
    CHECK_EQ(t.overflows(), 1u);
    // The transport recovers on the next line.
    const char* ok = "{\"op\":\"get\",\"path\":\"/lamp/on\"}\n";
    t.feed(ok, strlen(ok));
    CHECK_EQ(lines.out.size(), 2u);
}

TEST(deferred_replies) {
    Api api;
    tesser::Pending saved;
    api.action("slow", [&](tesser::Call& call) { saved = call.defer(); });

    Lines lines;
    tesser::LineTransport t(api, [&](const char* data, size_t n) { lines.add(data, n); });
    const char* req = "{\"id\":5,\"op\":\"set\",\"path\":\"/slow\"}\n";
    t.feed(req, strlen(req));
    CHECK_EQ(lines.out.size(), 0u);
    CHECK(static_cast<bool>(saved));
    CHECK_EQ(api.pending(), 1);
    saved.reply(3.5);
    saved.reply(1);  // ignored: first reply wins
    CHECK_EQ(lines.out.size(), 1u);
    if (!lines.out.empty()) CHECK_EQ(lines.out[0], "{\"id\":5,\"status\":\"ok\",\"body\":3.5}");
    CHECK_EQ(api.pending(), 0);

    // A dropped handle answers "internal" instead of leaving the client hanging.
    lines.out.clear();
    t.feed(req, strlen(req));
    saved = tesser::Pending();
    CHECK_EQ(lines.out.size(), 1u);
    if (!lines.out.empty()) CHECK(lines.out[0].find("\"status\":\"internal\"") != std::string::npos);
    CHECK_EQ(api.pending(), 0);

    // Too many in flight: "busy".
    api.config().maxPending = 1;
    std::vector<tesser::Pending> held;
    api.action("hold", [&](tesser::Call& call) { held.push_back(call.defer()); });
    lines.out.clear();
    const char* hold = "{\"id\":6,\"op\":\"set\",\"path\":\"/hold\"}\n";
    t.feed(hold, strlen(hold));
    t.feed(hold, strlen(hold));
    CHECK_EQ(lines.out.size(), 1u);
    if (!lines.out.empty()) CHECK(lines.out[0].find("\"status\":\"busy\"") != std::string::npos);
    held.clear();  // releases the first: internal error reply
    CHECK_EQ(api.pending(), 0);

    // Transports that can't defer report it.
    Resp r = set(api, "/slow", nullptr);
    CHECK_EQ(r.status, Status::NotAllowed);
    CHECK(!saved);
}

TEST(deferred_not_in_patch) {
    Api api;
    api.action("slow", [](tesser::Call& call) { call.defer(); });
    CHECK_EQ(set(api, "/", "{\"slow\":null}").status, Status::BadRequest);
}

}  // namespace cases

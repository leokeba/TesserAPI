// Persistence and access control.
#pragma once

#include <stdio.h>
#include <string.h>
#if !defined(ESP_PLATFORM)
#include <unistd.h>
#endif

#include <string>

#include "core_cases.h"
#include "datagram_cases.h"
#include "subscription_cases.h"

namespace persistence_cases {

using tesser::Api;
using tesser::MemoryStorage;
using tesser::Status;

struct Settings {
    int brightness = 128;
    double gain = 0.5;
    char name[12] = "lamp";
    bool on = false;  // not persisted
    int calibration = 0;
    Api api;

    explicit Settings(tesser::Storage* storage = nullptr, uint32_t debounceMs = 1000) {
        auto& cfg = api.object("config").persist();
        cfg.value("brightness", brightness).range(0, 255);
        cfg.value("name", name);
        auto& n = cfg.object("tuning");
        n.value("gain", gain);
        api.value("on", on);
        api.value("calibration", calibration).readOnly().persist();
        if (storage) api.persistence(*storage, debounceMs);
    }
};

TEST(persist_state_and_roundtrip) {
    MemoryStorage storage;
    {
        Settings s(&storage);
        CHECK_EQ(s.api.persistedState(),
                 "{\"config\":{\"brightness\":128,\"name\":\"lamp\",\"tuning\":{\"gain\":0.5}},\"calibration\":0}");
        CHECK(!s.api.load());  // nothing stored yet
        cases::set(s.api, "/config", "{\"brightness\":7,\"name\":\"desk\",\"tuning\":{\"gain\":2}}");
        cases::set(s.api, "/on", "true");
        s.calibration = 42;  // read-only through the API, but persisted
        CHECK(s.api.save());
        // One record per top-level node holding persisted state.
        CHECK_EQ(storage.records.size(), 2u);
        CHECK_EQ(storage.records["config"], "{\"brightness\":7,\"name\":\"desk\",\"tuning\":{\"gain\":2}}");
        CHECK_EQ(storage.records["calibration"], "42");
    }
    Settings fresh(&storage);
    CHECK(fresh.api.load());
    CHECK_EQ(fresh.brightness, 7);
    CHECK_EQ(std::string(fresh.name), "desk");
    CHECK_EQ(fresh.gain, 2.0);
    CHECK_EQ(fresh.calibration, 42);
    CHECK(!fresh.on);
}

TEST(persist_lenient_load) {
    MemoryStorage storage;
    storage.records["config"] = "{\"brightness\":999,\"name\":\"kitchen\",\"removed\":1,\"tuning\":5}";
    storage.records["on"] = "true";
    storage.records["calibration"] = "3";
    storage.records["ghost"] = "{\"x\":1}";
    Settings s(&storage);
    CHECK(s.api.load());
    CHECK_EQ(s.brightness, 128);  // out of range: default kept
    CHECK_EQ(std::string(s.name), "kitchen");
    CHECK_EQ(s.gain, 0.5);  // "tuning" is now an object
    CHECK(!s.on);           // not persisted: its record isn't even read
    CHECK_EQ(s.calibration, 3);

    // A record that doesn't parse keeps its node's defaults; the others load.
    storage.records["config"] = "{not json";
    Settings broken(&storage);
    CHECK(broken.api.load());
    CHECK_EQ(broken.brightness, 128);
    CHECK_EQ(broken.calibration, 3);
}

TEST(persist_autosave_debounce) {
    MemoryStorage storage;
    Settings s(&storage, 1000);
    s.api.poll(10000);
    CHECK_EQ(storage.saves, 0);  // nothing changed

    cases::set(s.api, "/config/brightness", "1");
    s.api.poll(10100);  // change noticed: debounce starts
    s.api.poll(10600);
    CHECK_EQ(storage.saves, 0);
    cases::set(s.api, "/config/brightness", "2");  // restarts the debounce
    s.api.poll(10700);
    s.api.poll(11600);
    CHECK_EQ(storage.saves, 0);
    s.api.poll(11800);
    // Only the record that changed is written.
    CHECK_EQ(storage.saves, 1);
    CHECK_EQ(storage.records.count("calibration"), 0u);
    CHECK(storage.records["config"].find("\"brightness\":2") != std::string::npos);
    s.api.poll(13000);
    CHECK_EQ(storage.saves, 1);  // nothing new

    // Non-persisted changes never trigger a save.
    cases::set(s.api, "/on", "true");
    s.api.poll(14000);
    s.api.poll(16000);
    CHECK_EQ(storage.saves, 1);

    // A change elsewhere writes its own record only.
    s.calibration = 5;
    s.api.changed("/calibration");
    s.api.poll(17000);
    s.api.poll(18100);
    CHECK_EQ(storage.saves, 2);
    CHECK_EQ(storage.records["calibration"], "5");
}

TEST(persist_restore) {
    MemoryStorage storage;
    Settings s(&storage);
    subscription_cases::FakeSubscriber watcher;
    subscription_cases::subscribe(s.api, &watcher, "/config");
    std::vector<std::string> skipped;
    Status st = s.api.restore(
        "{\"config\":{\"brightness\":9,\"name\":\"attic\",\"old\":1,\"tuning\":{\"gain\":\"x\"}},\"on\":true,"
        "\"calibration\":12}",
        &skipped);
    CHECK_EQ(st, Status::Ok);
    CHECK_EQ(s.brightness, 9);
    CHECK_EQ(std::string(s.name), "attic");
    CHECK_EQ(s.gain, 0.5);
    CHECK(!s.on);
    CHECK_EQ(s.calibration, 12);
    CHECK_EQ(skipped.size(), 3u);
    if (skipped.size() == 3) {
        CHECK_EQ(skipped[0], "/config/old");
        CHECK_EQ(skipped[1], "/config/tuning/gain");
        CHECK_EQ(skipped[2], "/on");
    }
    // Saved at once, and subscribers hear about it.
    CHECK_EQ(storage.records["config"], "{\"brightness\":9,\"name\":\"attic\",\"tuning\":{\"gain\":0.5}}");
    s.api.poll(100);
    CHECK_EQ(watcher.last(), "{\"op\":\"change\",\"path\":\"/config\",\"body\":{\"brightness\":9,\"name\":\"attic\"}}");

    CHECK_EQ(s.api.restore("{nope"), Status::BadRequest);
    CHECK_EQ(s.api.restore("[1]"), Status::BadRequest);
}

TEST(persist_erase) {
    MemoryStorage storage;
    Settings s(&storage);
    CHECK(s.api.save());
    CHECK(!storage.records.empty());
    CHECK(storage.erase());
    Settings fresh(&storage);
    CHECK(!fresh.api.load());
}

#if !defined(ESP_PLATFORM)
TEST(persist_file_storage) {
    char dir[] = "/tmp/tesser_state_XXXXXX";
    CHECK(mkdtemp(dir) != nullptr);
    std::string path = std::string(dir) + "/state";
    tesser::FileStorage file(path);
    std::string out;
    CHECK(!file.load("config", out));
    CHECK(file.erase());  // nothing yet: fine
    CHECK(file.save("config", "{\"a\":1}"));  // creates the directory
    CHECK(file.save("config", "{\"a\":2}"));  // replaces
    CHECK(file.save("other", "3"));
    CHECK(file.load("config", out));
    CHECK_EQ(out, "{\"a\":2}");
    CHECK(file.load("other", out));
    CHECK_EQ(out, "3");
    CHECK(file.erase());
    CHECK(!file.load("config", out));
    CHECK(!file.load("other", out));
    rmdir(path.c_str());
    rmdir(dir);
}
#endif

// ---- access control -------------------------------------------------------

TEST(auth_read_only_unless_authenticated) {
    cases::Device d;
    d.api.authorize(tesser::authorizers::readOnlyUnlessAuthenticated());
    CHECK_EQ(cases::get(d.api, "/lamp").status, Status::Ok);
    cases::Resp r = cases::set(d.api, "/lamp/on", "true");
    CHECK_EQ(r.status, Status::Unauthorized);
    CHECK_EQ(r.body, "{\"error\":\"unauthorized\",\"path\":\"/lamp/on\",\"message\":\"not authorized\"}");
    CHECK_EQ(cases::set(d.api, "/lamp/toggle", nullptr).status, Status::Unauthorized);
    CHECK(!d.lamp.on);

    JsonDocument doc;
    deserializeJson(doc, "true");
    tesser::Request req;
    req.op = tesser::Op::Set;
    req.path = "/lamp/on";
    req.body = doc.as<JsonVariantConst>();
    req.client.authenticated = true;
    tesser::StringReply reply;
    d.api.handle(req, reply);
    CHECK_EQ(reply.status, Status::Ok);
    CHECK(d.lamp.on);
}

TEST(auth_per_node_in_patches) {
    cases::Device d;
    // Only "config" needs authentication.
    d.api.authorize([](const tesser::Client& c, tesser::Op op, const tesser::Node& n) {
        return op == tesser::Op::Get || c.authenticated || strcmp(n.name(), "config") != 0;
    });
    cases::Resp r = cases::set(d.api, "/", "{\"lamp\":{\"on\":true},\"config\":{\"level\":9}}");
    CHECK_EQ(r.status, Status::Unauthorized);
    CHECK(r.body.find("\"path\":\"/config\"") != std::string::npos);
    CHECK(!d.lamp.on);  // validated before anything was applied
    CHECK_EQ(cases::set(d.api, "/", "{\"lamp\":{\"on\":true}}").status, Status::Ok);
}

TEST(auth_by_transport) {
    cases::Device d;
    d.api.authorize(tesser::authorizers::readOnlyUnlessAuthenticated());
    // Serial: authenticated unless told otherwise.
    cases::Lines lines;
    tesser::LineTransport t(d.api, [&](const char* data, size_t n) { lines.add(data, n); });
    const char* set = "{\"id\":1,\"op\":\"set\",\"path\":\"/lamp/brightness\",\"body\":3}\n";
    t.feed(set, strlen(set));
    t.setAuthenticated(false);
    t.feed(set, strlen(set));
    CHECK_EQ(lines.out.size(), 2u);
    if (lines.out.size() == 2) {
        CHECK(lines.out[0].find("\"status\":\"ok\"") != std::string::npos);
        CHECK(lines.out[1].find("\"status\":\"unauthorized\"") != std::string::npos);
    }

    // Datagram: only trusted peers.
    using datagram_cases::addr;
    datagram_cases::Net net;
    tesser::DatagramEndpoint server(&d.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    server.trust([](const tesser::PeerAddress& p) { return p == addr(7); });
    const char* req = "{\"id\":1,\"op\":\"set\",\"path\":\"/lamp/brightness\",\"body\":4}";
    server.receive(addr(1), req, strlen(req), true);
    server.receive(addr(7), req, strlen(req), true);
    server.process();
    CHECK_EQ(net.frames.size(), 2u);
    if (net.frames.size() == 2) {
        CHECK(net.frames[0].text.find("unauthorized") != std::string::npos);
        CHECK(net.frames[1].text.find("\"status\":\"ok\"") != std::string::npos);
    }
    CHECK_EQ(d.lamp.brightness, 4);
}

}  // namespace persistence_cases

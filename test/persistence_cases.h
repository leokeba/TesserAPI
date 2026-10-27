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
        // A backup holds every value, defaults included.
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

// Stored records nest as deep as they were written, past maxDepth + 1.
TEST(persist_deep_records) {
    MemoryStorage storage;
    std::string state;
    {
        cases::DeepTree t;
        t.api.persistence(storage);
        t.networks[0].ip = 7;
        CHECK(t.api.save());
        state = t.api.persistedState();
    }
    cases::DeepTree fresh;
    fresh.api.persistence(storage);
    CHECK(fresh.api.load());
    CHECK_EQ(fresh.networks[0].ip, 7);
    fresh.networks[0].ip = 0;
    CHECK_EQ(fresh.api.restore(state), Status::Ok);
    CHECK_EQ(fresh.networks[0].ip, 7);
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
    CHECK_EQ(storage.records["config"], "{\"brightness\":9,\"name\":\"attic\"}");  // gain at its default
    s.api.poll(100);
    CHECK_EQ(watcher.last(), "{\"op\":\"change\",\"path\":\"/config\",\"body\":{\"brightness\":9,\"name\":\"attic\"}}");

    CHECK_EQ(s.api.restore("{nope"), Status::BadRequest);
    CHECK_EQ(s.api.restore("[1]"), Status::BadRequest);
}

TEST(persist_erase) {
    MemoryStorage storage;
    Settings s(&storage);
    cases::set(s.api, "/config/brightness", "3");
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
    // A record larger than a read piece comes back whole, in one string
    // sized from the file (unbuffered, docs/DESIGN.md section 12).
    std::string big = "\"" + std::string(3000, 'r') + "\"";
    CHECK(file.save("big", big));
    CHECK(file.load("big", out));
    CHECK_EQ(out, big);
    CHECK_EQ(out.capacity() < 2 * big.size(), true);
    CHECK(file.remove("big"));
    CHECK(file.remove("big"));  // none stored: fine
    CHECK(!file.load("big", out));
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
    CHECK_EQ(r.body, "{\"error\":\"unauthorized\",\"reason\":\"not_authorized\",\"path\":\"/lamp/on\",\"message\":\"not authorized\"}");
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

    // Untrusted peers get the token check's level for no token, like HTTP.
    d.api.authenticate([](std::string_view token) { return token.empty() ? tesser::Access::Admin : tesser::Access::Public; });
    net.frames.clear();
    const char* req5 = "{\"id\":1,\"op\":\"set\",\"path\":\"/lamp/brightness\",\"body\":5}";
    server.receive(addr(1), req5, strlen(req5), true);
    server.process();
    CHECK_EQ(net.frames.size(), 1u);
    if (!net.frames.empty()) CHECK(net.frames[0].text.find("\"status\":\"ok\"") != std::string::npos);
    CHECK_EQ(d.lamp.brightness, 5);
}

// The generation counter wraps (docs/DESIGN.md section 11): a persisted
// change is saved however many other changes came before it.
TEST(persist_generation_wraparound) {
    MemoryStorage storage;
    Settings s(&storage, 1000);
    s.api.poll(10000);
    // Many changes to values that aren't persisted, past half of what 16
    // bits held, then a persisted one.
    for (int i = 0; i < 40000; i++) s.api.changed("/on");
    cases::set(s.api, "/config/brightness", "1");
    s.api.poll(10100);
    s.api.poll(11200);
    CHECK_EQ(storage.saves, 1);
    // More than half the 32-bit counter later, and across its wrap.
    tesser::detail::setGeneration(0xfffffff0u);
    for (int i = 0; i < 40; i++) s.api.changed("/on");
    cases::set(s.api, "/config/brightness", "2");
    s.api.poll(12000);
    s.api.poll(13100);
    CHECK_EQ(storage.saves, 2);
    CHECK(storage.records["config"].find("\"brightness\":2") != std::string::npos);
    tesser::detail::setGeneration(tesser::currentGeneration() + 0x80000000u + 40000);
    s.api.poll(14000);  // nothing to save
    cases::set(s.api, "/config/brightness", "3");
    s.api.poll(14100);
    s.api.poll(15200);
    CHECK_EQ(storage.saves, 3);
    s.api.poll(17000);
    CHECK_EQ(storage.saves, 3);
}

// Records hold what differs from the defaults (docs/DESIGN.md section 12),
// so a release that changes a default reaches every device whose user never
// changed that value.
TEST(persist_sparse_defaults) {
    MemoryStorage storage;
    {
        Settings s(&storage);
        cases::set(s.api, "/config/brightness", "7");
        CHECK(s.api.save());
        CHECK_EQ(storage.records["config"], "{\"brightness\":7}");
        CHECK_EQ(storage.records.count("calibration"), 0u);
        // Back to its default: the record goes.
        cases::set(s.api, "/config/brightness", "128");
        CHECK(s.api.save());
        CHECK_EQ(storage.records.count("config"), 0u);
        cases::set(s.api, "/config/brightness", "9");
        CHECK(s.api.save());
    }
    // A later release with another default for gain: it applies.
    Settings next;
    next.gain = 0.75;
    next.api.persistence(storage);
    CHECK(next.api.load());
    CHECK_EQ(next.brightness, 9);
    CHECK_EQ(next.gain, 0.75);
    // An old full backup restores; what equals today's defaults isn't stored.
    CHECK_EQ(next.api.restore("{\"config\":{\"brightness\":128,\"name\":\"lamp\",\"tuning\":{\"gain\":0.75}},"
                              "\"calibration\":0}"),
             Status::Ok);
    CHECK_EQ(next.brightness, 128);
    CHECK_EQ(storage.records.count("config"), 0u);
}

// A restore gives exactly the backed-up state, whatever changed since:
// values set back to their defaults included (docs/DESIGN.md section 12).
TEST(persist_restore_is_exact) {
    MemoryStorage storage;
    Settings s(&storage);
    cases::set(s.api, "/config/brightness", "7");        // A
    cases::set(s.api, "/config/name", "\"desk\"");        // B
    std::string backup = s.api.persistedState();
    cases::set(s.api, "/config/name", "\"attic\"");       // B again
    cases::set(s.api, "/config/tuning/gain", "2");       // C
    CHECK(s.api.save());
    CHECK_EQ(s.api.restore(backup), Status::Ok);
    CHECK_EQ(s.brightness, 7);
    CHECK_EQ(std::string(s.name), "desk");
    CHECK_EQ(s.gain, 0.5);  // C back at its default
    CHECK_EQ(storage.records["config"], "{\"brightness\":7,\"name\":\"desk\"}");  // still sparse
    // Undoing a restore with the backup taken just before it.
    std::string before = s.api.persistedState();
    cases::set(s.api, "/config/brightness", "128");
    CHECK_EQ(s.api.restore(before), Status::Ok);
    CHECK_EQ(s.brightness, 7);
}

// A key persisted on the device but kept out of backups: a backup restored
// on another device mustn't clone it, and an older backup carrying it
// leaves the current one alone.
TEST(persist_not_in_backups) {
    MemoryStorage storage;
    std::string key = "k1", fleet = "f1";
    int level = 1;
    {
        Settings s(&storage);
        auto& sys = s.api.object("sys").persist();
        sys.value("key", key).secret().backup(false);
        auto& peers = sys.object("peers").backup(false);  // a subtree
        peers.value("password", fleet).secret();
        sys.value("level", level);
        s.api.persistence(storage);  // defaults of the nodes declared since
        cases::set(s.api, "/sys", "{\"key\":\"k2\",\"peers\":{\"password\":\"f2\"},\"level\":3}");
        CHECK(s.api.save());
        CHECK_EQ(storage.records["sys"], "{\"key\":\"k2\",\"peers\":{\"password\":\"f2\"},\"level\":3}");
        std::string backup = s.api.persistedState();
        CHECK(backup.find("k2") == std::string::npos);
        CHECK(backup.find("f2") == std::string::npos);
        CHECK(backup.find("\"sys\":{\"level\":3}") != std::string::npos);

        // An older backup's key is ignored, not listed as skipped.
        std::vector<std::string> skipped;
        CHECK_EQ(s.api.restore("{\"sys\":{\"key\":\"old\",\"peers\":{\"password\":\"old\"},\"level\":2}}", &skipped),
                 Status::Ok);
        CHECK_EQ(key, "k2");
        CHECK_EQ(fleet, "f2");
        CHECK_EQ(level, 2);
        CHECK_EQ(skipped.size(), 0u);
        CHECK_EQ(storage.records["sys"], "{\"key\":\"k2\",\"peers\":{\"password\":\"f2\"},\"level\":2}");
    }
    // Loaded from storage as usual.
    key = fleet = "";
    Settings fresh(&storage);
    auto& sys = fresh.api.object("sys").persist();
    sys.value("key", key).backup(false);
    sys.object("peers").backup(false).value("password", fleet);
    sys.value("level", level);
    CHECK(fresh.api.load());
    CHECK_EQ(key, "k2");
    CHECK_EQ(fleet, "f2");
    CHECK(sys.child("key")->backedUp() == false);
    CHECK(sys.backup(true).backedUp());
}

// Stored state from before a version goes through the migration first:
// TesserKIT drops values its devices froze at old defaults.
TEST(persist_migration) {
    MemoryStorage storage;
    storage.records["config"] = "{\"brightness\":128,\"label\":\"desk\",\"tuning\":{\"gain\":0.5}}";
    storage.records["calibration"] = "3";
    Settings s;
    s.gain = 0.75;  // this release's default
    std::vector<std::string> seen;
    s.api.persistVersion(1, [&](std::string_view record, uint32_t version, JsonVariant value) {
        seen.push_back(std::string(record) + "@" + std::to_string(version));
        if (record != "config") return true;
        if (value["tuning"]["gain"] == 0.5) value["tuning"].as<JsonObject>().remove("gain");  // the old default
        if (value["label"].is<const char*>()) {  // renamed
            value["name"] = value["label"];
            value.as<JsonObject>().remove("label");
        }
        return true;
    });
    s.api.persistence(storage);
    CHECK(s.api.load());
    CHECK_EQ(seen.size(), 2u);
    if (seen.size() == 2) CHECK_EQ(seen[0], "config@0");
    CHECK_EQ(s.gain, 0.75);
    CHECK_EQ(std::string(s.name), "desk");
    CHECK_EQ(s.calibration, 3);
    // Rewritten under the version, sparse.
    CHECK_EQ(storage.records["@version"], "1");
    CHECK_EQ(storage.records["config"], "{\"name\":\"desk\"}");
    CHECK_EQ(s.api.persistedState(),
             "{\"@version\":1,\"config\":{\"brightness\":128,\"name\":\"desk\",\"tuning\":{\"gain\":0.75}},"
             "\"calibration\":3}");
    // Loaded again: current, no migration.
    seen.clear();
    Settings again;
    again.gain = 0.75;
    again.api.persistVersion(1, [&](std::string_view, uint32_t, JsonVariant) {
        seen.push_back("x");
        return true;
    });
    again.api.persistence(storage);
    CHECK(again.api.load());
    CHECK(seen.empty());
    // A backup from before versions is migrated too; one record can be dropped.
    again.api.persistVersion(1, [&](std::string_view record, uint32_t, JsonVariant) { return record != "calibration"; });
    CHECK_EQ(again.api.restore("{\"calibration\":8,\"config\":{\"brightness\":5}}"), Status::Ok);
    CHECK_EQ(again.calibration, 3);
    CHECK_EQ(again.brightness, 5);
}

TEST(persist_unsaved_set) {
    // A set with Query::persist false (a DMX input driving a persisted
    // value) changes the value and notifies, but doesn't rewrite the record.
    MemoryStorage storage;
    Settings s(&storage);
    tesser::LocalClient local(s.api);
    tesser::Query q;
    q.persist = false;
    Status got = Status::Internal;
    auto done = [&got](Status st, JsonVariantConst) { got = st; };
    for (int i = 0; i < 20; i++) {
        CHECK(local.set("/config/brightness", std::to_string(i), done, q));
        s.api.poll(uint32_t(1000 + 100 * i));
    }
    local.process();
    CHECK_EQ(got, Status::Ok);
    CHECK_EQ(s.brightness, 19);
    s.api.poll(10000);
    CHECK_EQ(storage.saves, 0);
    // A save due already takes it along: the record holds current values.
    CHECK_EQ(cases::set(s.api, "/config/name", "\"desk\"").status, Status::Ok);
    CHECK(local.request(tesser::Op::Set, "/config/brightness", "5", done, q));
    s.api.poll(20000);
    s.api.poll(30000);
    CHECK_EQ(storage.saves, 1);
    CHECK_EQ(storage.records["config"], "{\"brightness\":5,\"name\":\"desk\"}");
}

// Refuses to save one key while `failing`.
struct RefusingStorage : MemoryStorage {
    std::string refused;
    bool failing = true;
    bool save(std::string_view key, const std::string& data) override {
        if (failing && key == refused) return false;
        return MemoryStorage::save(key, data);
    }
};

TEST(persist_migration_partly_saved) {
    // The rewrite after a migration fails for one record: the records it
    // rewrote aren't migrated again at the next boot, and poll() finishes
    // the rewrite once the storage takes it.
    RefusingStorage storage;
    storage.records["config"] = "{\"brightness\":128,\"label\":\"desk\"}";
    storage.records["calibration"] = "3";
    storage.refused = "calibration";
    std::vector<std::string> seen;
    auto migrate = [&](std::string_view record, uint32_t version, JsonVariant value) {
        seen.push_back(std::string(record) + "@" + std::to_string(version));
        if (record == "config" && value["label"].is<const char*>()) {
            value["name"] = value["label"];
            value.as<JsonObject>().remove("label");
        }
        return true;
    };
    {
        Settings s;
        s.api.persistVersion(1, migrate);
        s.api.persistence(storage);
        CHECK(s.api.load());
        CHECK_EQ(storage.records.count("@version"), 0u);
        CHECK_EQ(storage.records["config"], "{\"name\":\"desk\"}");
        CHECK_EQ(storage.records["@migrated"], "1:config");
    }
    // Rebooted before it could finish: config is in the new shape already.
    seen.clear();
    Settings s;
    s.api.persistVersion(1, migrate);
    s.api.persistence(storage);
    CHECK(s.api.load());
    CHECK_EQ(seen.size(), 1u);
    if (!seen.empty()) CHECK_EQ(seen[0], "calibration@0");
    CHECK_EQ(std::string(s.name), "desk");
    storage.failing = false;
    s.api.poll(100000);
    CHECK_EQ(storage.records["@version"], "1");
    CHECK_EQ(storage.records.count("@migrated"), 0u);
    CHECK_EQ(storage.records["calibration"], "3");
}

inline size_t g_free = SIZE_MAX;
inline size_t freeProbe() { return g_free; }

// A record is written in one allocation of its exact size, from the byte
// budget: on a short heap it waits, still dirty.
TEST(persist_record_needs_room) {
    MemoryStorage storage;
    std::string notes;
    Settings s;
    s.api.value("notes", notes).persist();
    s.api.persistence(storage);
    notes = std::string(600, 'n');
    s.api.changed("/notes");
    size_t floor = tesser::heapFloor();
    tesser::setHeapFloor(8000);
    tesser::setFreeHeapProbe(&freeProbe);
    g_free = 8300;
    CHECK(!s.api.save());
    CHECK_EQ(storage.records.count("notes"), 0u);
    g_free = 20000;
    CHECK(s.api.save());
    tesser::setFreeHeapProbe(nullptr);
    tesser::setHeapFloor(floor);
    CHECK_EQ(storage.records["notes"].size(), 602u);
}

}  // namespace persistence_cases

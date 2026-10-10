// File nodes and transfers (docs/DESIGN.md section 4.6).
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"

namespace file_cases {

using tesser::Access;
using tesser::Api;
using tesser::FileTransfer;
using tesser::Status;

// Collects an upload; can fail on a given chunk.
struct Ram {
    std::string data;
    std::string partial;
    int aborted = 0;
    int finished = 0;
    int failAt = -1;
};

class RamSink : public tesser::FileSink {
public:
    explicit RamSink(Ram& r) : r_(r) {}
    ~RamSink() override {
        if (!done_) r_.aborted++;
    }
    tesser::Check write(const uint8_t* data, size_t len) override {
        if (r_.failAt >= 0 && chunks_++ == r_.failAt) return tesser::Check::fail(Status::Internal, "flash write failed");
        r_.partial.append(reinterpret_cast<const char*>(data), len);
        return tesser::Check::ok();
    }
    tesser::Check finish(tesser::JsonWriter& reply) override {
        done_ = true;
        r_.data = r_.partial;
        r_.finished++;
        reply.beginObject();
        reply.key("bytes");
        reply.uinteger(r_.data.size());
        reply.endObject();
        return tesser::Check::ok();
    }

private:
    Ram& r_;
    int chunks_ = 0;
    bool done_ = false;
};

struct Fixture {
    Api api;
    Ram ram;
    int level = 3;
    std::string config = "{\"level\":3}";

    Fixture() {
        auto& sys = api.object("sys");
        sys.file("firmware")
            .upload([this](tesser::FileRequest& r) -> std::unique_ptr<tesser::FileSink> {
                if (r.size < 4) {
                    r.refuse(Status::InvalidValue, "not an image");
                    return nullptr;
                }
                ram.partial.clear();
                return std::unique_ptr<tesser::FileSink>(new RamSink(ram));
            })
            .maxSize(64)
            .accept(".bin")
            .writeAccess(Access::Admin);
        sys.file("coredump").download([this](tesser::FileRequest& r) -> std::unique_ptr<tesser::FileSource> {
            r.filename = "core.elf";
            struct Src : tesser::FileSource {
                std::string s;
                size_t at = 0;
                int read(uint8_t* buf, size_t cap) override {
                    size_t n = s.size() - at < cap ? s.size() - at : cap;
                    memcpy(buf, s.data() + at, n);
                    at += n;
                    return static_cast<int>(n);
                }
            };
            auto* src = new Src();
            src->s = ram.data;
            return std::unique_ptr<tesser::FileSource>(src);
        });
        sys.file("config")
            .text([this] { return config; },
                  [this](std::string_view text, tesser::JsonWriter& reply) {
                      if (text.empty() || text.front() != '{') return tesser::Check::fail(Status::InvalidValue, "not JSON");
                      config.assign(text.data(), text.size());
                      reply.string("saved");
                      return tesser::Check::ok();
                  })
            .contentType("application/json");
        sys.value("level", level);
    }

    tesser::Client admin() const {
        tesser::Client c;
        c.authenticated = true;
        return c;
    }
};

inline std::string upload(Api& api, const char* path, const std::string& data, const tesser::Client& client,
                          size_t chunk = 8) {
    FileTransfer t(api);
    tesser::StringReply reply;
    FileTransfer::Open o = t.openUpload(path, client, data.size(), reply);
    if (o == FileTransfer::Open::NotFile) return "not a file";
    if (o == FileTransfer::Open::Failed) return reply.body;
    for (size_t at = 0; at < data.size(); at += chunk) {
        size_t n = data.size() - at < chunk ? data.size() - at : chunk;
        if (!t.write(reinterpret_cast<const uint8_t*>(data.data() + at), n, reply)) return reply.body;
    }
    t.finish(reply);
    return std::string(tesser::toString(reply.status)) + " " + reply.body;
}

inline std::string download(Api& api, const char* path, const tesser::Client& client, std::string* name = nullptr) {
    FileTransfer t(api);
    tesser::StringReply reply;
    FileTransfer::Open o = t.openDownload(path, client, reply);
    if (o == FileTransfer::Open::NotFile) return "not a file";
    if (o == FileTransfer::Open::Failed) return reply.body;
    if (name) *name = t.filename();
    std::string out;
    uint8_t buf[5];
    int n;
    while ((n = t.read(buf, sizeof(buf))) > 0) out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
    return n < 0 ? "read error" : out;
}

TEST(file_schema_and_values) {
    Fixture f;
    CHECK_EQ(cases::get(f.api, "/sys/firmware", cases::schema()).body,
             "{\"type\":\"file\",\"readable\":false,\"writable\":true,\"maxSize\":64,"
             "\"contentType\":\"application/octet-stream\",\"accept\":\".bin\",\"access\":\"admin\"}");
    CHECK_EQ(cases::get(f.api, "/sys/config", cases::schema()).body,
             "{\"type\":\"file\",\"readable\":true,\"writable\":true,\"maxSize\":16384,\"contentType\":\"application/json\"}");
    // No value: left out of reads, and JSON requests can't transfer them.
    CHECK_EQ(cases::get(f.api, "/sys").body, "{\"level\":3}");
    CHECK_EQ(cases::get(f.api, "/sys/config").status, Status::NotAllowed);
    CHECK_EQ(cases::set(f.api, "/sys/config", "\"x\"").status, Status::NotAllowed);
    CHECK_EQ(cases::set(f.api, "/sys", "{\"level\":1,\"config\":\"x\"}").status, Status::NotAllowed);
    CHECK_EQ(f.level, 3);
}

TEST(file_upload) {
    Fixture f;
    std::string image = "IMAGE-0123456789-abcdefghij";
    CHECK_EQ(upload(f.api, "/sys/firmware", image, f.admin()), "ok {\"bytes\":27}");
    CHECK_EQ(f.ram.data, image);
    CHECK_EQ(f.ram.finished, 1);
    // Too large (announced), refused by the handler, not writable, not a file.
    CHECK(upload(f.api, "/sys/firmware", std::string(65, 'x'), f.admin()).find("too_large") != std::string::npos);
    std::string r = upload(f.api, "/sys/firmware", "ab", f.admin());
    CHECK_EQ(r, "{\"error\":\"invalid_value\",\"path\":\"/sys/firmware\",\"message\":\"not an image\"}");
    CHECK(upload(f.api, "/sys/coredump", "abcd", f.admin()).find("not_allowed") != std::string::npos);
    CHECK_EQ(upload(f.api, "/sys/level", "abcd", f.admin()), "not a file");
    CHECK(upload(f.api, "/sys/nope", "abcd", f.admin()).find("not_found") != std::string::npos);
    // Access levels apply.
    CHECK(upload(f.api, "/sys/firmware", image, tesser::Client()).find("unauthorized") != std::string::npos);
    CHECK_EQ(f.ram.finished, 1);
}

TEST(file_upload_failures) {
    Fixture f;
    // A failing write ends the upload: the sink is dropped, not finished.
    f.ram.failAt = 1;
    std::string r = upload(f.api, "/sys/firmware", "0123456789abcdefghij", f.admin());
    CHECK_EQ(r, "{\"error\":\"internal\",\"path\":\"/sys/firmware\",\"message\":\"flash write failed\"}");
    CHECK_EQ(f.ram.aborted, 1);
    CHECK_EQ(f.ram.finished, 0);
    f.ram.failAt = -1;
    {
        // A transfer abandoned midway (the client went away) aborts too, and
        // one at a time per node.
        FileTransfer t(f.api);
        tesser::StringReply reply;
        CHECK(t.openUpload("/sys/firmware", f.admin(), 10, reply) == FileTransfer::Open::Ok);
        CHECK(t.write(reinterpret_cast<const uint8_t*>("01234"), 5, reply));
        CHECK(upload(f.api, "/sys/firmware", "abcdef", f.admin()).find("\"busy\"") != std::string::npos);
        // Fewer bytes than announced.
        t.finish(reply);
        CHECK_EQ(reply.status, Status::BadRequest);
    }
    CHECK_EQ(f.ram.aborted, 2);
    CHECK_EQ(upload(f.api, "/sys/firmware", "abcdef", f.admin()), "ok {\"bytes\":6}");
    {
        // More bytes than announced.
        FileTransfer t(f.api);
        tesser::StringReply reply;
        CHECK(t.openUpload("/sys/firmware", f.admin(), 4, reply) == FileTransfer::Open::Ok);
        CHECK(!t.write(reinterpret_cast<const uint8_t*>("01234"), 5, reply));
        CHECK_EQ(reply.status, Status::TooLarge);
    }
}

TEST(file_download) {
    Fixture f;
    upload(f.api, "/sys/firmware", "core-dump-bytes", f.admin());
    std::string name;
    CHECK_EQ(download(f.api, "/sys/coredump", tesser::Client(), &name), "core-dump-bytes");
    CHECK_EQ(name, "core.elf");
    CHECK_EQ(download(f.api, "/sys/config", tesser::Client(), &name), "{\"level\":3}");
    CHECK_EQ(name, "config");  // the node's name by default
    CHECK(download(f.api, "/sys/firmware", f.admin()).find("not_allowed") != std::string::npos);
    CHECK_EQ(download(f.api, "/sys/level", f.admin()), "not a file");
}

TEST(file_text_upload) {
    Fixture f;
    CHECK_EQ(upload(f.api, "/sys/config", "{\"level\":9}", f.admin()), "ok \"saved\"");
    CHECK_EQ(f.config, "{\"level\":9}");
    CHECK_EQ(upload(f.api, "/sys/config", "nope", f.admin()),
             "invalid_value {\"error\":\"invalid_value\",\"path\":\"/sys/config\",\"message\":\"not JSON\"}");
    CHECK(upload(f.api, "/sys/config", std::string(16385, ' '), f.admin()).find("too_large") != std::string::npos);
}

TEST(file_backup_and_restore) {
    // A backup file: the persisted state down, a restore up (TesserKIT's
    // /sys/backup), reporting the skipped paths.
    Api api;
    int a = 1, b = 2;
    api.object("cfg").persist();
    api.object("cfg").value("a", a);
    api.object("cfg").value("b", b);
    api.file("backup").contentType("application/json").text(
        [&api] { return api.persistedState(); },
        [&api](std::string_view text, tesser::JsonWriter& reply) {
            std::vector<std::string> skipped;
            Status s = api.restore(text, &skipped);
            if (s != Status::Ok) return tesser::Check::fail(s, "not a backup");
            reply.beginObject();
            reply.key("skipped");
            reply.beginArray();
            for (const std::string& p : skipped) reply.string(p);
            reply.endArray();
            reply.endObject();
            return tesser::Check::ok();
        });
    tesser::Client admin;
    admin.authenticated = true;
    std::string backup = download(api, "/backup", admin);
    CHECK_EQ(backup, "{\"cfg\":{\"a\":1,\"b\":2}}");
    CHECK_EQ(upload(api, "/backup", "{\"cfg\":{\"a\":5,\"gone\":1}}", admin), "ok {\"skipped\":[\"/cfg/gone\"]}");
    CHECK_EQ(a, 5);
    CHECK_EQ(upload(api, "/backup", "{oops", admin),
             "bad_request {\"error\":\"bad_request\",\"path\":\"/backup\",\"message\":\"not a backup\"}");
}

TEST(file_in_list_element) {
    struct Item {
        int x = 0;
    };
    Api api;
    std::vector<Item> items(1);
    api.list("items", items, [](tesser::Object& o, Item& i) {
        o.value("x", i.x);
        o.file("blob").text([] { return std::string("b"); }, nullptr);
    });
    tesser::Client admin;
    admin.authenticated = true;
    CHECK(download(api, "/items/0/blob", admin).find("not_allowed") != std::string::npos);
}

}  // namespace file_cases

// File transfers over the message transports: "upload" and "download"
// envelopes (docs/DESIGN.md section 10.1.1), through FileSession.
#pragma once

#include <string>
#include <vector>

#include "datagram_cases.h"
#include "file_cases.h"

namespace file_envelope_cases {

using file_cases::Fixture;
using tesser::Api;
using tesser::FileSession;
using tesser::Status;

// A line transport and what it wrote.
struct Line {
    cases::Lines lines;
    tesser::LineTransport t;
    explicit Line(Api& api) : t(api, [this](const char* d, size_t n) { lines.add(d, n); }) {}
    std::string send(const std::string& text) {
        size_t before = lines.out.size();
        t.feed(text.data(), text.size());
        t.feed('\n');
        if (lines.out.size() != before + 1) return "<" + std::to_string(lines.out.size() - before) + " lines>";
        return lines.out.back();
    }
};

inline std::string b64(const std::string& s) {
    std::string out;
    tesser::detail::base64Encode(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out);
    return out;
}

// Downloads `path` over the line in chunks of `length`, checking offsets.
inline std::string downloadAll(Line& line, const char* path, size_t length) {
    std::string out;
    for (int i = 0; i < 100; i++) {
        std::string r = line.send("{\"op\":\"download\",\"path\":\"" + std::string(path) +
                                  "\",\"offset\":" + std::to_string(out.size()) +
                                  ",\"length\":" + std::to_string(length) + "}");
        JsonDocument doc;
        deserializeJson(doc, r);
        if (doc["status"] != "ok") return r;
        if (doc["body"]["offset"].as<size_t>() != out.size()) return "bad offset: " + r;
        std::vector<uint8_t> bytes;
        if (!tesser::detail::base64Decode(doc["body"]["data"].as<const char*>(), bytes)) return "bad base64: " + r;
        out.append(bytes.begin(), bytes.end());
        if (doc["body"]["end"] == true) return out;
    }
    return "no end";
}

// Uploads `data` over the line in chunks of `chunk`; returns the last reply.
inline std::string uploadAll(Line& line, const char* path, const std::string& data, size_t chunk) {
    std::string r;
    size_t at = 0;
    do {
        size_t n = data.size() - at < chunk ? data.size() - at : chunk;
        std::string env = "{\"id\":" + std::to_string(at) + ",\"op\":\"upload\",\"path\":\"" + path +
                          "\",\"offset\":" + std::to_string(at) + ",\"data\":\"" + b64(data.substr(at, n)) + "\"";
        if (at == 0) env += ",\"size\":" + std::to_string(data.size());
        r = line.send(env + "}");
        at += n;
        if (r.find("\"status\":\"ok\"") == std::string::npos) return r;
    } while (at < data.size());
    return r;
}

TEST(file_base64) {
    // RFC 4648's vectors.
    const char* plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; i++) {
        CHECK_EQ(b64(plain[i]), coded[i]);
        std::vector<uint8_t> out;
        CHECK(tesser::detail::base64Decode(coded[i], out));
        CHECK_EQ(std::string(out.begin(), out.end()), plain[i]);
    }
    std::string all;
    for (int c = 0; c < 256; c++) all += char(c);
    std::vector<uint8_t> out;
    CHECK(tesser::detail::base64Decode(b64(all), out));
    CHECK_EQ(std::string(out.begin(), out.end()), all);
    CHECK(!tesser::detail::base64Decode("Zm9", out));     // length
    CHECK(!tesser::detail::base64Decode("Zm9!", out));    // alphabet
    CHECK(!tesser::detail::base64Decode("Zg==Zg==", out));  // padding inside
    CHECK(!tesser::detail::base64Decode("====", out));
}

TEST(file_envelope_download_again_for_less) {
    // A chunk asked again with a smaller length (a gateway short of memory)
    // sends its start; the rest, already read, goes first with the next.
    Fixture f;
    Line line(f.api);
    CHECK(line.send("{\"id\":1,\"op\":\"download\",\"path\":\"/sys/config\",\"length\":2}").find("\"data\":\"eyI=\"") !=
          std::string::npos);
    CHECK_EQ(line.send("{\"id\":2,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":2,\"length\":6}"),
             "{\"id\":2,\"status\":\"ok\",\"body\":{\"offset\":2,\"data\":\"bGV2ZWwi\",\"end\":false}}");
    CHECK_EQ(line.send("{\"id\":3,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":2,\"length\":3}"),
             "{\"id\":3,\"status\":\"ok\",\"body\":{\"offset\":2,\"data\":\"bGV2\",\"end\":false}}");
    CHECK_EQ(line.send("{\"id\":4,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":8}"),
             "{\"id\":4,\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/sys/config\","
             "\"message\":\"offset must be 5\"}}");
    CHECK_EQ(line.send("{\"id\":5,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":5}"),
             "{\"id\":5,\"status\":\"ok\",\"body\":{\"offset\":5,\"data\":\"ZWwiOjN9\",\"end\":true}}");
}

TEST(file_envelope_download) {
    Fixture f;
    Line line(f.api);
    // The opening reply describes the file; each chunk gives its offset.
    CHECK_EQ(line.send("{\"id\":1,\"op\":\"download\",\"path\":\"/sys/config\",\"length\":4}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"size\":11,\"filename\":\"config\",\"contentType\":"
             "\"application/json\",\"chunk\":4096,\"offset\":0,\"data\":\"eyJsZQ==\",\"end\":false}}");
    CHECK_EQ(line.send("{\"id\":2,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":4,\"length\":4}"),
             "{\"id\":2,\"status\":\"ok\",\"body\":{\"offset\":4,\"data\":\"dmVsIg==\",\"end\":false}}");
    // Only the next offset, or the last chunk's again (its reply was lost).
    CHECK_EQ(line.send("{\"id\":3,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":4,\"length\":4}"),
             "{\"id\":3,\"status\":\"ok\",\"body\":{\"offset\":4,\"data\":\"dmVsIg==\",\"end\":false}}");
    CHECK_EQ(line.send("{\"id\":4,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":2}"),
             "{\"id\":4,\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/sys/config\","
             "\"message\":\"offset must be 8\"}}");
    // Another client finds the path busy meanwhile.
    CHECK(file_cases::download(f.api, "/sys/config", f.admin()).find("\"busy\"") != std::string::npos);
    CHECK_EQ(line.send("{\"id\":5,\"op\":\"download\",\"path\":\"/sys/config/\",\"offset\":8}"),
             "{\"id\":5,\"status\":\"ok\",\"body\":{\"offset\":8,\"data\":\"OjN9\",\"end\":true}}");
    // The end releases the path; the last chunk can still be asked again.
    CHECK_EQ(file_cases::download(f.api, "/sys/config", f.admin()), "{\"level\":3}");
    CHECK_EQ(line.send("{\"id\":6,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":8}"),
             "{\"id\":6,\"status\":\"ok\",\"body\":{\"offset\":8,\"data\":\"OjN9\",\"end\":true}}");
    CHECK_EQ(line.send("{\"id\":7,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":11}"),
             "{\"id\":7,\"status\":\"ok\",\"body\":{\"offset\":11,\"data\":\"\",\"end\":true}}");

    // Whole files, in any chunk size; unknown sizes end with an empty chunk.
    CHECK_EQ(downloadAll(line, "/sys/config", 3), "{\"level\":3}");
    CHECK_EQ(downloadAll(line, "/sys/config", 100), "{\"level\":3}");
    file_cases::upload(f.api, "/sys/firmware", "core-dump-bytes", f.admin());
    CHECK_EQ(downloadAll(line, "/sys/coredump", 5), "core-dump-bytes");
    CHECK_EQ(line.send("{\"op\":\"download\",\"path\":\"/sys/coredump\"}"),
             "{\"status\":\"ok\",\"body\":{\"filename\":\"core.elf\",\"contentType\":\"application/octet-stream\","
             "\"chunk\":4096,\"offset\":0,\"data\":\"Y29yZS1kdW1wLWJ5dGVz\",\"end\":true}}");
    // In list elements too.
    Api api;
    file_cases::Dir dir(api);
    Line l2(api);
    CHECK_EQ(downloadAll(l2, "/files/entries/notes.txt/content", 2), "hello");

    // Refusals.
    CHECK(line.send("{\"id\":8,\"op\":\"download\",\"path\":\"/sys/firmware\"}").find("not_allowed") !=
          std::string::npos);
    CHECK_EQ(line.send("{\"id\":9,\"op\":\"download\",\"path\":\"/sys/level\"}"),
             "{\"id\":9,\"status\":\"not_allowed\",\"body\":{\"error\":\"not_allowed\",\"path\":\"/sys/level\","
             "\"message\":\"not a file\"}}");
    CHECK(line.send("{\"id\":10,\"op\":\"download\",\"path\":\"/sys/nope\"}").find("not_found") != std::string::npos);
    CHECK(line.send("{\"id\":11,\"op\":\"download\",\"path\":\"/sys/config\",\"length\":0}").find("bad_request") !=
          std::string::npos);
    CHECK(line.send("{\"id\":12,\"op\":\"download\",\"path\":\"/sys/coredump\",\"offset\":5}")
              .find("no download in progress") != std::string::npos);
    CHECK(line.send("{\"id\":13,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":-1}").find("bad_request") !=
          std::string::npos);
    CHECK_EQ(line.send("{\"id\":[1],\"op\":\"download\",\"path\":\"/sys/config\"}"),
             "{\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/\",\"message\":\"id must be a scalar\"}}");
}

TEST(file_envelope_chunk_limits) {
    Fixture f;
    Line line(f.api);
    // Chunks fit the transports' limits: a download reply maxResponse, an
    // upload envelope maxRequestBody, each with room for the rest.
    CHECK_EQ(FileSession::downloadChunk(f.api.config()), 4096u);
    CHECK_EQ(FileSession::uploadChunk(f.api.config()), 2880u);
    f.api.config().maxResponse = 1024;
    CHECK_EQ(FileSession::downloadChunk(f.api.config()), 576u);
    std::string r = line.send("{\"op\":\"download\",\"path\":\"/sys/config\",\"length\":100000}");
    CHECK(r.find("\"chunk\":576") != std::string::npos);
    CHECK(r.find("\"end\":true") != std::string::npos);
    // A large file in the largest chunks: base64 of 2880 bytes fits a line.
    std::string big;
    for (int i = 0; i < 6000; i++) big += char('a' + i % 26);
    Api api;
    std::string got;
    api.file("big").maxSize(10000).text([&big] { return big; },
                                        [&got](std::string_view text, tesser::JsonWriter&) {
                                            got = std::string(text);
                                            return tesser::Check::ok();
                                        });
    Line l2(api);
    CHECK_EQ(uploadAll(l2, "/big", big, FileSession::uploadChunk(api.config())), "{\"id\":5760,\"status\":\"ok\",\"body\":null}");
    CHECK_EQ(got, big);
    CHECK_EQ(l2.t.overflows(), 0u);
    CHECK_EQ(downloadAll(l2, "/big", 100000), big);
}

TEST(file_envelope_crc32) {
    // The check value of CRC-32 (IEEE, as zlib's), and of base64 text.
    const char* nine = "123456789";
    CHECK_EQ(tesser::detail::crc32(reinterpret_cast<const uint8_t*>(nine), 9), 0xCBF43926u);
    uint32_t sum = 0;
    CHECK(tesser::detail::crc32OfBase64("eyJsZQ==", sum));
    CHECK_EQ(sum, 1378081651u);
    CHECK(!tesser::detail::crc32OfBase64("eyJsZQ=", sum));

    Fixture f;
    Line line(f.api);
    // An upload chunk with a crc32 that doesn't match its data (bytes lost
    // on a line, still base64) is refused, and the transfer stays open.
    CHECK_EQ(line.send("{\"id\":1,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":6}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"received\":0,\"chunk\":2880}}");
    CHECK_EQ(line.send("{\"id\":2,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":0,\"data\":\"YWJjZGVm\","
                       "\"crc32\":3094309647}"),
             "{\"id\":2,\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"reason\":\"checksum\","
             "\"path\":\"/sys/firmware\",\"message\":\"the data doesn't match its crc32: send the chunk again\"}}");
    CHECK(line.send("{\"id\":3,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":0,\"data\":\"YWJjZGVm\","
                    "\"crc32\":-1}")
              .find("crc32 must be") != std::string::npos);
    CHECK_EQ(line.send("{\"v\":2,\"id\":4,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":0,"
                       "\"data\":\"YWJjZGVm\",\"crc32\":1267612143}"),
             "{\"id\":4,\"status\":\"ok\",\"body\":{\"bytes\":6}}");
    CHECK_EQ(f.ram.data, "abcdef");
    // A download reply carries its chunk's crc32 when asked.
    CHECK_EQ(line.send("{\"v\":2,\"id\":5,\"op\":\"download\",\"path\":\"/sys/config\",\"length\":4,\"crc32\":true}"),
             "{\"id\":5,\"status\":\"ok\",\"body\":{\"size\":11,\"filename\":\"config\",\"contentType\":"
             "\"application/json\",\"chunk\":4096,\"offset\":0,\"data\":\"eyJsZQ==\",\"crc32\":1378081651,\"end\":false}}");
    CHECK(line.send("{\"id\":6,\"op\":\"download\",\"path\":\"/sys/config\",\"crc32\":1}").find("crc32 must be a boolean") !=
          std::string::npos);
}

TEST(file_envelope_upload) {
    Fixture f;
    Line line(f.api);
    // The open gives the size (and a name); replies count the bytes received
    // and give the largest chunk; the last one is the sink's reply.
    CHECK_EQ(line.send("{\"id\":1,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":16,\"filename\":\"fw.bin\","
                       "\"data\":\"SU1BR0Ut\"}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"received\":6,\"chunk\":2880}}");
    CHECK_EQ(line.send("{\"id\":2,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":6,\"data\":\"MDEyMzQ1\"}"),
             "{\"id\":2,\"status\":\"ok\",\"body\":{\"received\":12}}");
    // A chunk sent again is acknowledged; only new bytes are written.
    CHECK_EQ(line.send("{\"id\":3,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":6,\"data\":\"MDEyMzQ1\"}"),
             "{\"id\":3,\"status\":\"ok\",\"body\":{\"received\":12}}");
    // A gap isn't.
    CHECK_EQ(line.send("{\"id\":4,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":13,\"data\":\"Njc4OQ==\"}"),
             "{\"id\":4,\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/sys/firmware\","
             "\"message\":\"offset must be at most 12\"}}");
    CHECK(line.send("{\"id\":5,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":12,\"data\":\"Njc\"}")
              .find("data must be base64") != std::string::npos);
    CHECK(line.send("{\"id\":6,\"op\":\"upload\",\"path\":\"/sys/config\",\"offset\":12,\"data\":\"Njc4OQ==\"}")
              .find("no upload in progress") != std::string::npos);
    CHECK_EQ(f.ram.finished, 0);
    CHECK_EQ(line.send("{\"id\":7,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":12,\"data\":\"Njc4OQ==\"}"),
             "{\"id\":7,\"status\":\"ok\",\"body\":{\"bytes\":16}}");
    CHECK_EQ(f.ram.data, "IMAGE-0123456789");
    CHECK_EQ(f.ram.finished, 1);
    // Over: a chunk sent again finds nothing open.
    CHECK(line.send("{\"id\":8,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":12,\"data\":\"Njc4OQ==\"}")
              .find("no upload in progress") != std::string::npos);

    // Opened without data (to learn the chunk size), then sent from offset 0:
    // only "size" opens an upload.
    CHECK_EQ(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":4}"),
             "{\"status\":\"ok\",\"body\":{\"received\":0,\"chunk\":2880}}");
    CHECK_EQ(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":0,\"data\":\"YWJjZA==\"}"),
             "{\"status\":\"ok\",\"body\":{\"bytes\":4}}");
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"data\":\"YWI=\"}")
              .find("no upload in progress") != std::string::npos);

    // In one envelope; with the data in any chunks; a text() file.
    CHECK_EQ(line.send("{\"id\":9,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":16,"
                       "\"data\":\"SU1BR0UtMDEyMzQ1Njc4OQ==\"}"),
             "{\"id\":9,\"status\":\"ok\",\"body\":{\"bytes\":16}}");
    CHECK_EQ(uploadAll(line, "/sys/firmware", "abcdefghijklmnopqrstuvwxyz", 5),
             "{\"id\":25,\"status\":\"ok\",\"body\":{\"bytes\":26}}");
    CHECK_EQ(f.ram.data, "abcdefghijklmnopqrstuvwxyz");
    CHECK_EQ(uploadAll(line, "/sys/config", "{\"level\":7}", 4), "{\"id\":8,\"status\":\"ok\",\"body\":\"saved\"}");
    CHECK_EQ(f.config, "{\"level\":7}");
    // A name, for the handler.
    Api api;
    file_cases::Dir dir(api);
    Line l2(api);
    CHECK_EQ(l2.send("{\"op\":\"upload\",\"path\":\"/files/upload\",\"size\":2,\"filename\":\"n.txt\",\"data\":\"YWI=\"}"),
             "{\"status\":\"ok\",\"body\":\"n.txt\"}");
    CHECK_EQ(*dir.find("n.txt"), "ab");

    // Refusals: size, access, the handler, the sink.
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":4,\"offset\":2}").find("offset 0") !=
          std::string::npos);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":\"4\"}").find("size must be") !=
          std::string::npos);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":65}").find("too_large") != std::string::npos);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":2,\"data\":\"YWI=\"}").find("not an image") !=
          std::string::npos);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/coredump\",\"size\":4}").find("not_allowed") != std::string::npos);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/level\",\"size\":4}").find("not a file") != std::string::npos);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":4,\"data\":\"YWJjZGU=\"}")
              .find("more bytes than announced") != std::string::npos);
    f.ram.failAt = 0;
    int aborted = f.ram.aborted;
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}")
              .find("flash write failed") != std::string::npos);
    CHECK_EQ(f.ram.aborted, aborted + 1);
    f.ram.failAt = -1;
    Line guest(f.api);
    guest.t.setAuthenticated(false);
    CHECK(guest.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":4}").find("unauthorized") !=
          std::string::npos);
}

TEST(file_envelope_session_lifetime) {
    Fixture f;
    f.api.authenticate([](std::string_view token) { return token == "t" ? tesser::Access::Admin : tesser::Access::Public; });
    Line line(f.api);
    line.t.setAuthenticated(false);
    CHECK(line.send("{\"op\":\"auth\",\"token\":\"t\"}").find("admin") != std::string::npos);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}")
              .find("\"received\":4") != std::string::npos);
    // A new transfer replaces the last: the upload is aborted.
    CHECK(line.send("{\"op\":\"download\",\"path\":\"/sys/config\",\"length\":1}").find("\"end\":false") !=
          std::string::npos);
    CHECK_EQ(f.ram.aborted, 1);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}")
              .find("\"received\":4") != std::string::npos);
    // Signing out ends it: it was opened at the higher level.
    CHECK(line.send("{\"op\":\"auth\"}").find("public") != std::string::npos);
    CHECK_EQ(f.ram.aborted, 2);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":4,\"data\":\"YWJjZA==\"}")
              .find("no upload in progress") != std::string::npos);
    // reset() (a Bluetooth disconnection) and the transport's end too.
    line.send("{\"op\":\"auth\",\"token\":\"t\"}");
    line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}");
    line.t.reset();
    CHECK_EQ(f.ram.aborted, 3);
    {
        Line other(f.api);
        other.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}");
        CHECK(line.send("{\"op\":\"auth\",\"token\":\"t\"}").find("admin") != std::string::npos);
        CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8}").find("\"busy\"") !=
              std::string::npos);
    }
    CHECK_EQ(f.ram.aborted, 4);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8}").find("\"received\":0") !=
          std::string::npos);
}

// A transfer opened at a level the client lost (its token revoked,
// Api::tokensChanged()) ends at its next chunk.
TEST(file_envelope_token_revoked) {
    Fixture f;
    bool valid = true;
    f.api.authenticate([&](std::string_view token) {
        return valid && token == "t" ? tesser::Access::Admin : tesser::Access::Public;
    });
    Line line(f.api);
    line.t.setAuthenticated(false);
    CHECK(line.send("{\"op\":\"auth\",\"token\":\"t\"}").find("admin") != std::string::npos);
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}")
              .find("\"received\":4") != std::string::npos);
    valid = false;
    f.api.tokensChanged();
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":4,\"data\":\"YWJjZA==\"}")
              .find("no upload in progress") != std::string::npos);
    CHECK_EQ(f.ram.aborted, 1);
    // Valid again: a transfer opened since goes on.
    valid = true;
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}")
              .find("\"received\":4") != std::string::npos);
    f.api.tokensChanged();
    CHECK(line.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":4,\"data\":\"YWJjZA==\"}")
              .find("\"error\"") == std::string::npos);  // finished
    CHECK_EQ(f.ram.aborted, 1);
}

TEST(file_envelope_idle_takeover) {
    Fixture f;
    Line a(f.api), b(f.api);
    CHECK(a.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}")
              .find("\"received\":4") != std::string::npos);
    // A transfer that moved nothing for transferIdleMs is taken over by the
    // next one to the same path; its client learns it on its next chunk.
    f.api.config().transferIdleMs = 0;  // never
    CHECK(b.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":4}").find("\"busy\"") != std::string::npos);
    f.api.config().transferIdleMs = 1;
    uint32_t t0 = tesser::millis32();
    while (tesser::millis32() - t0 < 3) {
    }
    CHECK(b.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":4}").find("\"received\":0") !=
          std::string::npos);
    CHECK_EQ(f.ram.aborted, 1);
    CHECK_EQ(a.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":4,\"data\":\"YWJjZA==\"}"),
             "{\"status\":\"timeout\",\"body\":{\"error\":\"timeout\",\"reason\":\"idle\",\"path\":\"/sys/firmware\",\"message\":"
             "\"the transfer was idle too long\"}}");
    // A busy transfer isn't idle, however long ago it started.
    f.api.config().transferIdleMs = 30000;
    CHECK_EQ(b.send("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":0,\"size\":4,\"data\":\"YWJjZA==\"}"),
             "{\"status\":\"ok\",\"body\":{\"bytes\":4}}");
}

TEST(file_envelope_websocket) {
    // handleEnvelope() (WebSocket) takes the session of the connection.
    Fixture f;
    FileSession files(f.api);
    std::string out;
    auto send = [&](const char* text, FileSession* session) {
        out.clear();
        tesser::handleEnvelope(f.api, text, f.admin(), [&](const std::string& m) { out = m; }, nullptr, nullptr,
                               session);
        return out;
    };
    CHECK_EQ(send("{\"id\":1,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":0,\"length\":8}", &files),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"size\":11,\"filename\":\"config\",\"contentType\":"
             "\"application/json\",\"chunk\":4096,\"offset\":0,\"data\":\"eyJsZXZlbCI=\",\"end\":false}}");
    CHECK(files.busy());
    CHECK_EQ(send("{\"id\":2,\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":8,\"length\":8}", &files),
             "{\"id\":2,\"status\":\"ok\",\"body\":{\"offset\":8,\"data\":\"OjN9\",\"end\":true}}");
    // Without a session, they aren't ops.
    CHECK(send("{\"id\":3,\"op\":\"download\",\"path\":\"/sys/config\"}", nullptr).find("bad_request") !=
          std::string::npos);
    files.reset();
    CHECK(!files.busy());
}

TEST(file_envelope_nowtp) {
    using datagram_cases::addr;
    using datagram_cases::Net;
    using datagram_cases::Result;
    Fixture f;
    Net net;
    tesser::DatagramEndpoint server(&f.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    tesser::DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    server.trust([](const tesser::PeerAddress&) { return true; });
    net.nodes = {{addr(1), &client}, {addr(2), &server}};
    auto call = [&](const char* envelope) {
        JsonDocument doc;
        deserializeJson(doc, envelope);
        Result r;
        CHECK(client.request(addr(2), doc.as<JsonObjectConst>(), datagram_cases::capture(r)));
        net.run();
        return std::string(tesser::toString(r.status)) + " " + r.body;
    };
    // The client sends any envelope under its own id.
    CHECK_EQ(call("{\"id\":99,\"op\":\"download\",\"path\":\"/sys/config\",\"length\":8}"),
             "ok {\"size\":11,\"filename\":\"config\",\"contentType\":\"application/json\",\"chunk\":4096,"
             "\"offset\":0,\"data\":\"eyJsZXZlbCI=\",\"end\":false}");
    CHECK_EQ(call("{\"op\":\"download\",\"path\":\"/sys/config\",\"offset\":8}"),
             "ok {\"offset\":8,\"data\":\"OjN9\",\"end\":true}");
    CHECK_EQ(call("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}"),
             "ok {\"received\":4,\"chunk\":2880}");
    CHECK_EQ(call("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":4,\"data\":\"ZWZnaA==\"}"),
             "ok {\"bytes\":8}");
    CHECK_EQ(f.ram.data, "abcdefgh");
    // Uploads must arrive reliably, like sets.
    net.frames.clear();
    const char* up = "{\"id\":4,\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":4}";
    server.receive(addr(3), up, strlen(up), false);
    server.process();
    CHECK_EQ(net.frames.size(), 1u);
    if (!net.frames.empty()) CHECK(net.frames[0].text.find("\"status\":\"not_allowed\"") != std::string::npos);
    // A lost peer's transfer is aborted, and so is an idle one.
    CHECK_EQ(call("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}"),
             "ok {\"received\":4,\"chunk\":2880}");
    server.forgetPeer(addr(1));
    server.process();
    CHECK_EQ(f.ram.aborted, 1);
    CHECK_EQ(call("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}"),
             "ok {\"received\":4,\"chunk\":2880}");
    net.clock += 29000;
    server.process();
    CHECK_EQ(f.ram.aborted, 1);
    net.clock += 1000;
    server.process();
    CHECK_EQ(f.ram.aborted, 2);
    CHECK(call("{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":4,\"data\":\"ZWZnaA==\"}")
              .find("no upload in progress") != std::string::npos);
}

TEST(file_envelope_kept_through_a_link_loss) {
    // NowTP loses a peer whose announcements it missed, though its chunks
    // keep coming (measured relaying a firmware image from DEVKIT-V3 to
    // DEVKIT-R1): a peer heard within the grace keeps its transfer, and one
    // silent that long is forgotten (docs/DESIGN.md section 10.3).
    using datagram_cases::addr;
    using datagram_cases::Net;
    using datagram_cases::Result;
    using tesser::DatagramEndpoint;
    Fixture f;
    Net net;
    DatagramEndpoint server(&f.api, tesser::TransportKind::NowTP, net.sender(addr(2)), net.clockFn());
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    server.trust([](const tesser::PeerAddress&) { return true; });
    net.nodes = {{addr(1), &client}, {addr(2), &server}};
    auto call = [&](const char* envelope) {
        JsonDocument doc;
        deserializeJson(doc, envelope);
        Result r;
        CHECK(client.request(addr(2), doc.as<JsonObjectConst>(), datagram_cases::capture(r)));
        net.run();
        return std::string(tesser::toString(r.status)) + " " + r.body;
    };
    const char* first = "{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"size\":8,\"data\":\"YWJjZA==\"}";
    const char* second = "{\"op\":\"upload\",\"path\":\"/sys/firmware\",\"offset\":4,\"data\":\"ZWZnaA==\"}";
    CHECK_EQ(call(first), "ok {\"received\":4,\"chunk\":2880}");
    server.linkLost(addr(1));
    server.process();
    CHECK(server.lossPending(addr(1)));
    CHECK_EQ(f.ram.aborted, 0);
    CHECK_EQ(server.fileSessions(), 1u);
    CHECK_EQ(call(second), "ok {\"bytes\":8}");
    CHECK_EQ(f.ram.data, "abcdefgh");
    CHECK(!server.lossPending(addr(1)));  // heard again
    // Silent for the grace: forgotten after all, and its transfer with it.
    CHECK_EQ(call(first), "ok {\"received\":4,\"chunk\":2880}");
    server.linkLost(addr(1));
    server.process();
    net.clock += DatagramEndpoint::kLossGraceMs - 1;
    server.process();
    CHECK_EQ(f.ram.aborted, 0);
    net.clock += 1;
    server.process();
    CHECK_EQ(f.ram.aborted, 1);
    CHECK(!server.lossPending(addr(1)));
    CHECK_EQ(server.fileSessions(), 0u);
    // A peer already silent that long is forgotten at once.
    CHECK_EQ(call(first), "ok {\"received\":4,\"chunk\":2880}");
    net.clock += DatagramEndpoint::kLossGraceMs;
    server.linkLost(addr(1));
    server.process();
    CHECK_EQ(f.ram.aborted, 2);
    CHECK(!server.lossPending(addr(1)));
    // forgetPeer() doesn't wait.
    CHECK_EQ(call(first), "ok {\"received\":4,\"chunk\":2880}");
    server.forgetPeer(addr(1));
    server.process();
    CHECK_EQ(f.ram.aborted, 3);
}

}  // namespace file_envelope_cases

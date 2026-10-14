// File transfers relayed to remote nodes (docs/DESIGN.md section 14): a
// gateway at addr(1) mounting the file fixture's device at addr(2) as /dev.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "datagram_cases.h"
#include "file_envelope_cases.h"

namespace relay_cases {

using datagram_cases::addr;
using file_envelope_cases::b64;
using tesser::DatagramEndpoint;
using tesser::FileTransfer;
using tesser::Status;

// Envelopes come in to the gateway through a serial line. The device's
// chunks are smaller than the gateway's (576 bytes both ways), so the relay
// splits uploads. Declaration order matters: endpoints must outlive the
// trees holding remote nodes that use them.
struct Rig {
    file_cases::Fixture device;
    std::string big;
    std::string got;
    datagram_cases::Net net;
    DatagramEndpoint deviceEnd;
    DatagramEndpoint gatewayEnd;
    tesser::Api gateway;
    tesser::RemoteNode& dev;
    cases::Lines lines;
    tesser::LineTransport line;

    explicit Rig(DatagramEndpoint::SendFn deviceSend = nullptr, DatagramEndpoint::SendFn gatewaySend = nullptr,
                 DatagramEndpoint::ClockFn clock = nullptr)
        : deviceEnd(&device.api, tesser::TransportKind::NowTP, deviceSend ? deviceSend : net.sender(addr(2)),
                    clock ? clock : net.clockFn()),
          gatewayEnd(&gateway, tesser::TransportKind::NowTP, gatewaySend ? gatewaySend : net.sender(addr(1)),
                     clock ? clock : net.clockFn()),
          dev(gateway.remote("dev", gatewayEnd, addr(2))),
          line(gateway, [this](const char* d, size_t n) { lines.add(d, n); }) {
        for (int i = 0; i < 3000; i++) big += char('a' + i % 26);
        device.api.file("big").maxSize(100000).text([this] { return big; },
                                                    [this](std::string_view text, tesser::JsonWriter&) {
                                                        got = std::string(text);
                                                        return tesser::Check::ok();
                                                    });
        device.api.config().maxRequestBody = 1024;
        device.api.config().maxResponse = 1024;
        deviceEnd.trust([](const tesser::PeerAddress&) { return true; });
        net.nodes = {{addr(1), &gatewayEnd}, {addr(2), &deviceEnd}};
    }

    std::string ask(const std::string& envelope) {
        lines.out.clear();
        std::string l = envelope + "\n";
        line.feed(l.data(), l.size());
        net.run();
        return lines.out.size() == 1 ? lines.out[0] : "<" + std::to_string(lines.out.size()) + " lines>";
    }

    // Uploads `data` in envelopes of `chunk` bytes; returns the last reply.
    std::string uploadAll(const char* path, const std::string& data, size_t chunk) {
        std::string r;
        size_t at = 0;
        do {
            size_t n = data.size() - at < chunk ? data.size() - at : chunk;
            std::string env = "{\"op\":\"upload\",\"path\":\"" + std::string(path) + "\",\"offset\":" +
                              std::to_string(at) + ",\"data\":\"" + b64(data.substr(at, n)) + "\"";
            if (at == 0) env += ",\"size\":" + std::to_string(data.size());
            r = ask(env + "}");
            at += n;
            if (r.find("\"status\":\"ok\"") == std::string::npos) return r;
        } while (at < data.size());
        return r;
    }

    // Downloads `path` in chunks of up to `length`, checking offsets.
    std::string downloadAll(const char* path, size_t length, size_t* chunks = nullptr) {
        std::string out;
        for (int i = 0; i < 100; i++) {
            std::string r = ask("{\"op\":\"download\",\"path\":\"" + std::string(path) + "\",\"offset\":" +
                                std::to_string(out.size()) + ",\"length\":" + std::to_string(length) + "}");
            JsonDocument doc;
            deserializeJson(doc, r);
            if (doc["status"] != "ok") return r;
            if (doc["body"]["offset"].as<size_t>() != out.size()) return "bad offset: " + r;
            std::vector<uint8_t> bytes;
            if (!tesser::detail::base64Decode(doc["body"]["data"].as<const char*>(), bytes)) return "bad base64: " + r;
            out.append(bytes.begin(), bytes.end());
            if (chunks) ++*chunks;
            if (doc["body"]["end"] == true) return out;
        }
        return "no end";
    }
};

TEST(relay_envelope_upload) {
    Rig r;
    // The gateway's chunk is the smaller of its own and the device's; a
    // larger one is split into the device's.
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":3000,\"filename\":\"b.txt\","
                   "\"data\":\"" + b64(r.big.substr(0, 1500)) + "\"}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"received\":1500,\"chunk\":576}}");
    CHECK_EQ(r.ask("{\"id\":2,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":1500,\"data\":\"" +
                   b64(r.big.substr(1500, 576)) + "\"}"),
             "{\"id\":2,\"status\":\"ok\",\"body\":{\"received\":2076}}");
    // A chunk sent again: only its new bytes go on.
    CHECK_EQ(r.ask("{\"id\":3,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":2000,\"data\":\"" +
                   b64(r.big.substr(2000, 576)) + "\"}"),
             "{\"id\":3,\"status\":\"ok\",\"body\":{\"received\":2576}}");
    CHECK_EQ(r.ask("{\"id\":4,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":2577,\"data\":\"YQ==\"}"),
             "{\"id\":4,\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/dev/big\","
             "\"message\":\"offset must be at most 2576\"}}");
    // The last chunk gets the device's reply to the upload.
    CHECK_EQ(r.ask("{\"id\":5,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":2576,\"data\":\"" +
                   b64(r.big.substr(2576)) + "\"}"),
             "{\"id\":5,\"status\":\"ok\",\"body\":null}");
    CHECK_EQ(r.got, r.big);
    CHECK(r.ask("{\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":3000,\"data\":\"YQ==\"}")
              .find("no upload in progress") != std::string::npos);
    CHECK_EQ(r.uploadAll("/dev/sys/firmware", "0123456789abcdef", 5), "{\"status\":\"ok\",\"body\":{\"bytes\":16}}");
    CHECK_EQ(r.device.ram.data, "0123456789abcdef");
    CHECK_EQ(r.uploadAll("/dev/big", "", 5), "{\"status\":\"ok\",\"body\":null}");
    CHECK_EQ(r.got, "");

    // The device's errors, under the local path.
    CHECK_EQ(r.ask("{\"op\":\"upload\",\"path\":\"/dev/sys/firmware\",\"size\":100}"),
             "{\"status\":\"too_large\",\"body\":{\"error\":\"too_large\",\"path\":\"/dev/sys/firmware\","
             "\"message\":\"file too large\"}}");
    CHECK_EQ(r.ask("{\"op\":\"upload\",\"path\":\"/dev/sys/firmware\",\"size\":2,\"data\":\"YWI=\"}"),
             "{\"status\":\"invalid_value\",\"body\":{\"error\":\"invalid_value\",\"path\":\"/dev/sys/firmware\","
             "\"message\":\"not an image\"}}");
    CHECK_EQ(r.ask("{\"op\":\"upload\",\"path\":\"/dev/sys/level\",\"size\":2}"),
             "{\"status\":\"not_allowed\",\"body\":{\"error\":\"not_allowed\",\"path\":\"/dev/sys/level\","
             "\"message\":\"not a file\"}}");
    CHECK(r.ask("{\"op\":\"upload\",\"path\":\"/dev/nope/x\",\"size\":2}").find("\"path\":\"/dev/nope\"") !=
          std::string::npos);
    // A failed write ends the upload, there and here.
    r.device.ram.failAt = 1;
    CHECK_EQ(r.uploadAll("/dev/sys/firmware", "0123456789", 4),
             "{\"status\":\"internal\",\"body\":{\"error\":\"internal\",\"path\":\"/dev/sys/firmware\","
             "\"message\":\"flash write failed\"}}");
    CHECK(r.ask("{\"op\":\"upload\",\"path\":\"/dev/sys/firmware\",\"offset\":4,\"data\":\"YQ==\"}")
              .find("no upload in progress") != std::string::npos);
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(relay_envelope_download) {
    Rig r;
    // The device's chunks as they come, base64 and all.
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"download\",\"path\":\"/dev/sys/config\",\"length\":4}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"size\":11,\"filename\":\"config\",\"contentType\":"
             "\"application/json\",\"chunk\":576,\"offset\":0,\"data\":\"eyJsZQ==\",\"end\":false}}");
    CHECK_EQ(r.ask("{\"id\":2,\"op\":\"download\",\"path\":\"/dev/sys/config\",\"offset\":4,\"length\":4}"),
             "{\"id\":2,\"status\":\"ok\",\"body\":{\"offset\":4,\"data\":\"dmVsIg==\",\"end\":false}}");
    CHECK_EQ(r.ask("{\"id\":3,\"op\":\"download\",\"path\":\"/dev/sys/config\",\"offset\":4,\"length\":4}"),
             "{\"id\":3,\"status\":\"ok\",\"body\":{\"offset\":4,\"data\":\"dmVsIg==\",\"end\":false}}");
    CHECK_EQ(r.ask("{\"id\":4,\"op\":\"download\",\"path\":\"/dev/sys/config\",\"offset\":2}"),
             "{\"id\":4,\"status\":\"bad_request\",\"body\":{\"error\":\"bad_request\",\"path\":\"/dev/sys/config\","
             "\"message\":\"offset must be 8\"}}");
    CHECK_EQ(r.ask("{\"id\":5,\"op\":\"download\",\"path\":\"/dev/sys/config\",\"offset\":8}"),
             "{\"id\":5,\"status\":\"ok\",\"body\":{\"offset\":8,\"data\":\"OjN9\",\"end\":true}}");
    CHECK_EQ(r.ask("{\"id\":6,\"op\":\"download\",\"path\":\"/dev/sys/config\",\"offset\":8}"),
             "{\"id\":6,\"status\":\"ok\",\"body\":{\"offset\":8,\"data\":\"OjN9\",\"end\":true}}");
    // Whole files, in the device's chunks at most.
    size_t chunks = 0;
    CHECK_EQ(r.downloadAll("/dev/big", 100000, &chunks), r.big);
    CHECK_EQ(chunks, 6u);
    CHECK_EQ(r.downloadAll("/dev/big", 1000), r.big);
    r.device.ram.data = "core";
    CHECK_EQ(r.downloadAll("/dev/sys/coredump", 3), "core");
    CHECK_EQ(r.ask("{\"op\":\"download\",\"path\":\"/dev/sys/level\"}"),
             "{\"status\":\"not_allowed\",\"body\":{\"error\":\"not_allowed\",\"path\":\"/dev/sys/level\","
             "\"message\":\"not a file\"}}");
    CHECK(r.ask("{\"op\":\"download\",\"path\":\"/dev/sys/config\",\"offset\":5}").find("no download in progress") !=
          std::string::npos);
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(relay_one_transfer_per_peer) {
    Rig r;
    tesser::LineTransport other(r.gateway, [&r](const char* d, size_t n) { r.lines.add(d, n); });
    auto askOther = [&](const std::string& envelope) {
        r.lines.out.clear();
        std::string l = envelope + "\n";
        other.feed(l.data(), l.size());
        r.net.run();
        return r.lines.out.size() == 1 ? r.lines.out[0] : std::string("<lines>");
    };
    CHECK(r.ask("{\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":10}").find("\"received\":0") != std::string::npos);
    // The device keeps one transfer per client, and the gateway is one:
    // another path of the same device is busy too.
    CHECK_EQ(askOther("{\"op\":\"download\",\"path\":\"/dev/sys/config\"}"),
             "{\"status\":\"busy\",\"body\":{\"error\":\"busy\",\"reason\":\"transfer_busy\",\"path\":\"/dev/sys/config\","
             "\"message\":\"another transfer to this node is in progress\"}}");
    // Requests that aren't transfers go through meanwhile.
    CHECK_EQ(askOther("{\"op\":\"get\",\"path\":\"/dev/sys/level\"}"), "{\"status\":\"ok\",\"body\":3}");
    // The end frees the device; so does the client's next transfer, or its
    // reset.
    CHECK_EQ(r.ask("{\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":0,\"data\":\"" + b64("0123456789") + "\"}"),
             "{\"status\":\"ok\",\"body\":null}");
    CHECK(askOther("{\"op\":\"download\",\"path\":\"/dev/sys/config\",\"length\":2}").find("\"end\":false") !=
          std::string::npos);
    CHECK(r.ask("{\"op\":\"download\",\"path\":\"/dev/big\"}").find("busy") != std::string::npos);
    CHECK(askOther("{\"op\":\"download\",\"path\":\"/dev/sys/coredump\"}").find("\"status\":\"ok\"") !=
          std::string::npos);
    other.reset();
    CHECK(r.ask("{\"op\":\"download\",\"path\":\"/dev/big\",\"length\":3}").find("\"data\":\"YWJj\"") !=
          std::string::npos);
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(relay_retries_and_timeouts) {
    Rig r;
    CHECK(r.ask("{\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":8}").find("\"received\":0") != std::string::npos);
    // The device's reply to a chunk is lost: the relay sends it again, and
    // the device only acknowledges it.
    r.lines.out.clear();
    std::string env = "{\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":0,\"data\":\"" + b64("abcd") + "\"}\n";
    r.line.feed(env.data(), env.size());
    auto toDevice = r.net.frames;
    r.net.frames.clear();
    for (auto& f : toDevice) r.deviceEnd.receive(f.from, f.text.data(), f.text.size(), true);
    r.deviceEnd.process();
    CHECK_EQ(r.net.frames.size(), 1u);
    r.net.frames.clear();  // the reply
    r.net.clock += 3001;
    r.gatewayEnd.process();
    r.net.run();
    CHECK_EQ(r.lines.out.size(), 1u);
    if (!r.lines.out.empty()) CHECK_EQ(r.lines.out[0], "{\"status\":\"ok\",\"body\":{\"received\":4}}");
    CHECK_EQ(r.ask("{\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":4,\"data\":\"" + b64("efgh") + "\"}"),
             "{\"status\":\"ok\",\"body\":null}");
    CHECK_EQ(r.got, "abcdefgh");

    // A device that never answers: three attempts, then a timeout.
    r.gateway.remote("ghost", r.gatewayEnd, addr(9)).timeout(500);
    CHECK_EQ(r.ask("{\"op\":\"download\",\"path\":\"/ghost/f\"}"), "<0 lines>");
    for (int i = 0; i < 2; i++) {
        r.net.clock += 600;
        r.gatewayEnd.process();
        CHECK_EQ(r.lines.out.size(), 0u);
    }
    r.net.clock += 600;
    r.gatewayEnd.process();
    CHECK_EQ(r.lines.out.size(), 1u);
    if (!r.lines.out.empty()) {
        CHECK_EQ(r.lines.out[0], "{\"status\":\"timeout\",\"body\":{\"error\":\"timeout\",\"reason\":\"no_answer\",\"path\":\"/ghost\","
                                 "\"message\":\"remote node didn't answer\"}}");
    }
    // That ended it, and freed the peer.
    CHECK(r.ask("{\"op\":\"download\",\"path\":\"/ghost/f\",\"offset\":4}").find("no download in progress") !=
          std::string::npos);
    // A peer known to be lost isn't sent to.
    r.gatewayEnd.forgetPeer(addr(9));
    r.gatewayEnd.process();
    CHECK_EQ(r.ask("{\"op\":\"download\",\"path\":\"/ghost/f\"}"),
             "{\"status\":\"timeout\",\"body\":{\"error\":\"timeout\",\"reason\":\"peer_offline\",\"path\":\"/ghost\","
             "\"message\":\"peer offline\"}}");
    CHECK_EQ(r.gateway.pending(), 0);
    CHECK_EQ(r.gatewayEnd.pendingCalls(), 0u);
}

TEST(relay_from_a_peer) {
    // A NowTP client of the gateway: the relay runs in the endpoint that
    // also carries the device's replies, so it must not wait for them.
    Rig r;
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, r.net.sender(addr(3)), r.net.clockFn());
    r.net.nodes.push_back({addr(3), &client});
    r.gatewayEnd.trust([](const tesser::PeerAddress&) { return true; });
    datagram_cases::Result res;
    JsonDocument env;
    env["op"] = "upload";
    env["path"] = "/dev/sys/firmware";
    env["size"] = 6;
    env["data"] = b64("abcdef");
    CHECK(client.request(addr(1), env.as<JsonObjectConst>(), datagram_cases::capture(res)));
    r.net.run();
    CHECK(res.called);
    CHECK_EQ(res.body, "{\"bytes\":6}");
    CHECK_EQ(r.device.ram.data, "abcdef");
}

// The waiting relay (HTTP): FileTransfer with Remotes::Wait in this thread,
// the endpoints processed in another, as on a device.
struct Threaded {
    std::recursive_mutex mutex;  // the link: the relay sends from this thread
    std::atomic<uint32_t> clock{1000};
    std::atomic<bool> stop{false};
    std::unique_ptr<Rig> rig;
    std::thread pump;

    Threaded() {
        auto sender = [this](tesser::PeerAddress self) {
            return [this, self](const tesser::PeerAddress& to, const std::string& m, tesser::Delivery d) {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                rig->net.frames.push_back(datagram_cases::Net::Frame{self, to, m, d});
                return true;
            };
        };
        rig.reset(new Rig(sender(addr(2)), sender(addr(1)), [this] { return clock.load(); }));
        pump = std::thread([this] {
            while (!stop) {
                {
                    std::lock_guard<std::recursive_mutex> lock(mutex);
                    rig->net.run();
                }
                clock += 10;  // simulated time, so timeouts come quickly
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
    }
    ~Threaded() {
        stop = true;
        pump.join();
    }
};

TEST(relay_waiting) {
    Threaded t;
    Rig& r = *t.rig;
    tesser::Client admin;
    admin.authenticated = true;
    {
        // An upload in the gateway's pieces, sent in the device's.
        FileTransfer f(r.gateway, FileTransfer::Remotes::Wait);
        tesser::StringReply reply;
        CHECK(f.openUpload("/dev/big", admin, r.big.size(), reply, "b.txt") == FileTransfer::Open::Ok);
        for (size_t at = 0; at < r.big.size(); at += 1000) {
            CHECK(f.write(reinterpret_cast<const uint8_t*>(r.big.data() + at), 1000, reply));
        }
        f.finish(reply);
        CHECK_EQ(reply.body, "null");
        CHECK_EQ(r.got, r.big);
    }
    {
        FileTransfer f(r.gateway, FileTransfer::Remotes::Wait);
        tesser::StringReply reply;
        CHECK(f.openDownload("/dev/big", admin, reply) == FileTransfer::Open::Ok);
        CHECK_EQ(f.size(), 3000);
        CHECK_EQ(f.filename(), "big");
        CHECK_EQ(std::string(f.contentType()), "application/octet-stream");
        std::string out;
        uint8_t buf[700];
        int n;
        while ((n = f.read(buf, sizeof(buf))) > 0) out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
        CHECK_EQ(n, 0);
        CHECK_EQ(out, r.big);
    }
    {
        // The request says nothing about files: the device's node decides.
        FileTransfer f(r.gateway, FileTransfer::Remotes::Wait);
        tesser::StringReply reply;
        CHECK(f.openDownload("/dev/sys/level", admin, reply) == FileTransfer::Open::NotFile);
        CHECK(f.openUpload("/dev/sys", admin, 2, reply) == FileTransfer::Open::NotFile);
        CHECK(f.openDownload("/dev/nope", admin, reply) == FileTransfer::Open::NotFile);
        CHECK(f.openDownload("/dev", admin, reply) == FileTransfer::Open::NotFile);
        CHECK(!reply.begun);
        CHECK(f.openUpload("/dev/sys/firmware", admin, 100, reply) == FileTransfer::Open::Failed);
        CHECK_EQ(reply.body, "{\"error\":\"too_large\",\"path\":\"/dev/sys/firmware\",\"message\":\"file too large\"}");
        CHECK(f.openUpload("/dev/big", admin, 0, reply) == FileTransfer::Open::Ok);
        f.finish(reply);
        CHECK_EQ(reply.body, "null");
        CHECK_EQ(r.got, "");
        // An unreachable device: an error, not a fall back to forwarding.
        tesser::RemoteNode& ghost = r.gateway.remote("ghost", r.gatewayEnd, addr(9)).timeout(100);
        CHECK(f.openDownload("/ghost/f", admin, reply) == FileTransfer::Open::Failed);
        CHECK_EQ(reply.status, Status::Timeout);
        CHECK_EQ(reply.body, "{\"error\":\"timeout\",\"reason\":\"no_answer\",\"path\":\"/ghost\",\"message\":\"remote node didn't answer\"}");
        // A lost one too, though its error has a body.
        r.gatewayEnd.forgetPeer(addr(9));
        for (int i = 0; i < 1000 && ghost.online(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(f.openUpload("/ghost/f", admin, 4, reply) == FileTransfer::Open::Failed);
        CHECK_EQ(reply.body, "{\"error\":\"timeout\",\"reason\":\"peer_offline\",\"path\":\"/ghost\",\"message\":\"peer offline\"}");
    }
    {
        // A failed write ends the upload with the device's error.
        r.device.ram.failAt = 0;
        FileTransfer f(r.gateway, FileTransfer::Remotes::Wait);
        tesser::StringReply reply;
        CHECK(f.openUpload("/dev/sys/firmware", admin, 8, reply) == FileTransfer::Open::Ok);
        CHECK(!f.write(reinterpret_cast<const uint8_t*>("abcdefgh"), 8, reply));
        CHECK_EQ(reply.body, "{\"error\":\"internal\",\"path\":\"/dev/sys/firmware\",\"message\":\"flash write failed\"}");
        // The device is free for the next one.
        tesser::StringReply r2;
        CHECK(f.openDownload("/dev/sys/config", admin, r2) == FileTransfer::Open::Ok);
    }
    CHECK_EQ(r.gateway.pending(), 0);
}

}  // namespace relay_cases

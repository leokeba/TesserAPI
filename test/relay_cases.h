// File transfers relayed to remote nodes (docs/DESIGN.md section 14): a
// gateway at addr(1) mounting the file fixture's device at addr(2) as /dev.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "datagram_cases.h"
#include "file_envelope_cases.h"
#include "memory_cases.h"

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
    std::unique_ptr<file_cases::Dir> dir;  // the device's /files, for the tests that add it
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
        device.api.file("big").maxSize(100000).text([this] {
                                                        tesser::testing::HeapExempt app;  // the application's copy
                                                        return big;
                                                    },
                                                    [this](std::string_view text, tesser::JsonWriter&) {
                                                        tesser::testing::HeapExempt app;  // the application's copy
                                                        got = std::string(text);
                                                        return tesser::Check::ok();
                                                    });
        device.api.config().maxRequestBody = 1024;
        device.api.config().maxResponse = 1024;
        deviceEnd.trust([](const tesser::PeerAddress&) { return true; });
        net.nodes = {{addr(1), &gatewayEnd}, {addr(2), &deviceEnd}};
    }

    // What the client and the test hold is exempt from a heap limit
    // (heap_cases.h); the gateway's and the device's work isn't.
    std::string ask(const std::string& envelope) {
        bool exempt = tesser::testing::heapExempt();
        tesser::testing::setHeapExempt(true);
        lines.out.clear();
        std::string l = envelope + "\n";
        tesser::testing::setHeapExempt(false);
        line.feed(l.data(), l.size());
        net.run();
        tesser::testing::setHeapExempt(true);
        std::string reply = lines.out.size() == 1 ? lines.out[0] : "<" + std::to_string(lines.out.size()) + " lines>";
        tesser::testing::setHeapExempt(exempt);
        return reply;
    }

    // Uploads `data` in envelopes of `chunk` bytes; returns the last reply.
    std::string uploadAll(const char* path, const std::string& data, size_t chunk) {
        tesser::testing::HeapExempt client;
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
        tesser::testing::HeapExempt client;
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

TEST(relay_upload_survives_an_undelivered_reply) {
    // The device's reply to one chunk is lost (NowTP reports it
    // undelivered): the gateway's transfer stays open on the device, and
    // the chunk sent again goes on from there (docs/DESIGN.md section 10.3).
    Rig r;
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":3000}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"received\":0,\"chunk\":576}}");
    r.deviceEnd.sendFailed(addr(1), 0, tesser::SendFailure::Undelivered);
    r.net.run();
    for (size_t at = 0; at < r.big.size(); at += 576) {
        std::string reply = r.ask("{\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":" + std::to_string(at) +
                                  ",\"data\":\"" + b64(r.big.substr(at, 576)) + "\"}");
        CHECK(reply.find("\"status\":\"ok\"") != std::string::npos);
        if (reply.find("\"status\":\"ok\"") == std::string::npos) break;
    }
    CHECK_EQ(r.got, r.big);
}

TEST(relay_upload_on_a_short_heap) {
    // A gateway short of memory takes as much of a chunk as it holds, and
    // the client goes on from `received`; with no room even for the least
    // of it, it refuses it (busy, low_memory) rather than build it anyway
    // and abort, and the transfer stays open: the client sends it again.
    Rig r;
    r.device.api.config().maxRequestBody = 4096;  // the chunk would go as one part
    std::string first = r.big.substr(0, 1500);
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":3000}").find("\"status\":\"ok\""),
             8u);
    std::string reply;
    auto chunk = [&](size_t free, const std::string& envelope) {
        // The line comes in whole first; then the heap runs short.
        r.line.feed(envelope.data(), envelope.size());
        memory_cases::Floor floor(8000, free);
        r.lines.out.clear();
        r.line.feed("\n", 1);
        r.net.run();
        return r.lines.out.size() == 1 ? r.lines.out[0] : std::string();
    };
    reply = chunk(8500, "{\"id\":2,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":0,\"data\":\"" + b64(first) + "\"}");
    CHECK_EQ(reply.substr(0, 40), "{\"id\":2,\"status\":\"busy\",\"body\":{\"error\":");
    CHECK(reply.find("\"reason\":\"low_memory\"") != std::string::npos);
    // Room for some of it (384 bytes, their part and the link's copies of
    // it): 384 bytes taken.
    reply = chunk(10400, "{\"id\":3,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":0,\"data\":\"" + b64(first) + "\"}");
    CHECK_EQ(reply, "{\"id\":3,\"status\":\"ok\",\"body\":{\"received\":384}}");
    CHECK_EQ(r.ask("{\"id\":4,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":384,\"data\":\"" +
                   b64(r.big.substr(384)) + "\"}"),
             "{\"id\":4,\"status\":\"ok\",\"body\":null}");
    CHECK_EQ(r.got, r.big);
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(relay_line_without_room_is_refused) {
    // A serial line the heap can't hold is read to its end and refused
    // (busy, low_memory), under its id; the next line is read as usual.
    Rig r;
    {
        memory_cases::Floor floor(8000, 8500);
        std::string reply = r.ask("{\"id\":1,\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":3000,\"data\":\"" +
                                  b64(r.big.substr(0, 1500)) + "\"}");
        CHECK_EQ(reply.substr(0, 40), "{\"id\":1,\"status\":\"busy\",\"body\":{\"error\":");
        CHECK(reply.find("\"reason\":\"low_memory\"") != std::string::npos);
    }
    CHECK_EQ(r.ask("{\"id\":2,\"op\":\"get\",\"path\":\"/dev/big\",\"keys\":\"x\"}").find("\"id\":2"), 1u);
    CHECK_EQ(r.line.overflows(), 1u);
}

TEST(relay_reply_without_room_fails_at_once) {
    // A large reply the endpoint can't parse now fails its call at once
    // (busy, low_memory), not after the call's timeout.
    datagram_cases::Net net;
    DatagramEndpoint client(nullptr, tesser::TransportKind::NowTP, net.sender(addr(1)), net.clockFn());
    datagram_cases::Result res;
    CHECK(client.request(addr(2), tesser::Op::Get, "/big", "", datagram_cases::capture(res)));
    uint32_t id = DatagramEndpoint::callIdOf(net.frames.front().text);
    std::string big = "{\"id\":" + std::to_string(id) + ",\"status\":\"ok\",\"body\":\"" + std::string(3000, 'x') + "\"}";
    {
        memory_cases::Floor floor(8000, 11500);  // room to copy it, not to parse it
        client.receive(addr(2), big.data(), big.size(), true);
        client.process();
    }
    CHECK(res.called);
    CHECK_EQ(res.status, Status::Busy);
    CHECK(res.body.find("\"reason\":\"low_memory\"") != std::string::npos);
    CHECK_EQ(client.pendingCalls(), 0u);
}

TEST(relay_download_on_a_short_heap) {
    // The device's chunk is relayed from its reply's text: the gateway
    // parses its other fields alone. A parsed copy of a 4 KB reply took
    // twice its size more (above 18 KB here, both nodes' buffers counted).
    Rig r;
    r.device.api.config().maxResponse = 4096;
    std::string reply;
    {
        memory_cases::Floor floor(8000, 25000);
        reply = r.ask("{\"id\":1,\"op\":\"download\",\"path\":\"/dev/big\"}");
    }
    CHECK_EQ(reply, "{\"id\":1,\"status\":\"ok\",\"body\":{\"size\":3000,\"filename\":\"big\",\"contentType\":"
                    "\"application/octet-stream\",\"chunk\":2880,\"offset\":0,\"data\":\"" +
                        b64(r.big.substr(0, 2880)) + "\",\"end\":false}}");
    // With 8.5 KB above the floor, the gateway asks for what that holds as
    // it asks (the device's reply, its own, and their copies in flight:
    // 16/3 bytes per byte of data, after 1 KB for the rest): up to 1401
    // bytes, less what is held meanwhile, in steps of 384, in one round,
    // rather than 2880 refused and asked again for half.
    Rig tight;
    tight.device.api.config().maxResponse = 4096;
    {
        memory_cases::Floor floor(8000, 16500);
        reply = tight.ask("{\"id\":1,\"op\":\"download\",\"path\":\"/dev/big\"}");
    }
    size_t at = reply.find("\"data\":\"") + 8;
    size_t n = (reply.find('"', at) - at) / 4 * 3;
    CHECK_EQ(n, 1152u);
    CHECK_EQ(reply, "{\"id\":1,\"status\":\"ok\",\"body\":{\"size\":3000,\"filename\":\"big\",\"contentType\":"
                    "\"application/octet-stream\",\"chunk\":2880,\"offset\":0,\"data\":\"" +
                        b64(tight.big.substr(0, n)) + "\",\"end\":false}}");
    // Shorter yet, the parts are halved until they fit, whichever node
    // refuses them (the device its reply, the gateway its copy), and the
    // file comes whole.
    Rig small;
    small.device.api.config().maxResponse = 4096;
    {
        memory_cases::Floor floor(8000, 10000);
        size_t chunks = 0;
        CHECK_EQ(small.downloadAll("/dev/big", 100000, &chunks), small.big);
        CHECK(chunks >= 5);
    }
    CHECK_EQ(small.gateway.pending(), 0);
}

TEST(file_envelope_download_sized_to_the_room) {
    // A chunk is sized when it is asked for, to what the heap holds then:
    // with 8 KB above the floor, a chunk read here holds 5 bytes per byte
    // of data at once (the chunk, its reply, the stack's copies in flight),
    // after 1 KB for the rest: up to 1431 bytes, in steps of 384, rather
    // than 4096, or busy.
    file_cases::Fixture f;
    std::string big;
    for (int i = 0; i < 10000; i++) big += static_cast<char>('a' + i % 26);
    f.api.file("big").text([&] { return big; }, nullptr);
    file_envelope_cases::Line line(f.api);
    std::string first;
    {
        memory_cases::Floor floor(8000, 16192);
        first = line.send("{\"id\":1,\"op\":\"download\",\"path\":\"/big\"}");
        size_t at = first.find("\"data\":\"") + 8;
        size_t n = (first.find('"', at) - at) / 4 * 3;
        CHECK(first.find("\"status\":\"ok\"") != std::string::npos);
        CHECK(first.find("\"chunk\":4096") != std::string::npos);
        CHECK_EQ(n, 1152u);
        CHECK(first.find("\"data\":\"" + b64(big.substr(0, n)) + "\"") != std::string::npos);
        // The client goes on from where each reply ends, to the end.
        CHECK_EQ(file_envelope_cases::downloadAll(line, "/big", 4096), big);
    }
    // With room, the whole chunk.
    first = line.send("{\"id\":2,\"op\":\"download\",\"path\":\"/big\"}");
    CHECK(first.find("\"data\":\"" + b64(big.substr(0, 4096)) + "\"") != std::string::npos);
}

// A second way to the device (UDP, preferred), for routes changing under a
// relay.
struct TwoRoutes : Rig {
    DatagramEndpoint deviceUdp{&device.api, tesser::TransportKind::Udp, net.sender(addr(4)), net.clockFn()};
    DatagramEndpoint gatewayUdp{&gateway, tesser::TransportKind::Udp, net.sender(addr(3)), net.clockFn()};
    TwoRoutes() {
        deviceUdp.trust([](const tesser::PeerAddress&) { return true; });
        net.nodes.push_back({addr(3), &gatewayUdp});
        net.nodes.push_back({addr(4), &deviceUdp});
        dev.routeOrder({tesser::TransportKind::Udp, tesser::TransportKind::NowTP});
        dev.addRoute(gatewayUdp, addr(4));
    }
};

TEST(relay_stays_on_its_route) {
    // A relay goes by the route it opened on: the device keeps a transfer
    // per client, and the gateway by another link is another client, with
    // no upload (measured on DEVKIT-V3 relaying a firmware image to
    // DEVKIT-R1: "no upload in progress at this path" at its last chunk).
    TwoRoutes r;
    r.dev.setRouteOnline(&r.gatewayUdp, addr(4), false, r.net.clock);
    std::string first = r.ask("{\"id\":1,\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":1500,\"data\":\"" +
                              b64(r.big.substr(0, 576)) + "\"}");
    CHECK(first.find("\"received\":576") != std::string::npos);
    // The preferred route comes back: the rest still goes by NowTP.
    r.dev.setRouteOnline(&r.gatewayUdp, addr(4), true, r.net.clock);
    CHECK(r.dev.endpoint() == &r.gatewayUdp);
    CHECK(r.ask("{\"id\":2,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":576,\"data\":\"" +
                b64(r.big.substr(576, 576)) + "\"}")
              .find("\"received\":1152") != std::string::npos);
    CHECK_EQ(r.deviceUdp.fileSessions(), 0u);
    // Its route offline, the transfer fails at once and ends here, though
    // the node is online by the other.
    r.dev.setRouteOnline(&r.gatewayEnd, addr(2), false, r.net.clock);
    CHECK(r.dev.online());
    CHECK_EQ(r.ask("{\"id\":3,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":1152,\"data\":\"" +
                   b64(r.big.substr(1152, 348)) + "\"}"),
             "{\"id\":3,\"status\":\"timeout\",\"body\":{\"error\":\"timeout\",\"reason\":\"peer_offline\","
             "\"path\":\"/dev\",\"message\":\"the route the transfer started on is offline\"}}");
    CHECK(r.ask("{\"id\":4,\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":1152,\"data\":\"" +
                b64(r.big.substr(1152, 348)) + "\"}")
              .find("no upload in progress") != std::string::npos);
    CHECK(r.got.empty());
    // The device drops its side once it has been idle that long.
    CHECK_EQ(r.deviceEnd.fileSessions(), 1u);
    r.net.clock += r.device.api.config().transferIdleMs;
    r.net.run();
    r.deviceEnd.process();
    CHECK_EQ(r.deviceEnd.fileSessions(), 0u);
    // A new transfer takes the node's route now.
    CHECK(r.uploadAll("/dev/big", r.big.substr(0, 1500), 576).find("\"status\":\"ok\"") != std::string::npos);
    CHECK_EQ(r.got, r.big.substr(0, 1500));
}

TEST(file_envelope_upload_sized_to_the_room) {
    // On a fragmented heap (plenty free, no large block), an upload chunk
    // is taken as far as one block holds it decoded: the reply's
    // `received` says where the client goes on from. Its crc32 is of the
    // whole chunk, checked from its text first. Measured on DEVKIT-V3 as a
    // gateway before this: 11 KB free with a largest block of 3.8 KB
    // refused a 2880-byte chunk for 11 s, and the client gave up.
    file_cases::Fixture f;
    std::string big;
    for (int i = 0; i < 6000; i++) big += static_cast<char>('a' + i % 26);
    std::string got;
    f.api.file("big").maxSize(10000).text([&] { return big; },
                                          [&](std::string_view text, tesser::JsonWriter&) {
                                              got = std::string(text);
                                              return tesser::Check::ok();
                                          });
    f.api.config().maxRequestBody = 4096;  // chunks of 2880
    file_envelope_cases::Line line(f.api);
    CHECK(line.send("{\"id\":1,\"op\":\"upload\",\"path\":\"/big\",\"size\":6000}").find("\"chunk\":2880") !=
          std::string::npos);
    std::string chunk = b64(big.substr(0, 2880));
    uint32_t sum = tesser::detail::crc32(reinterpret_cast<const uint8_t*>(big.data()), 2880);
    // Each line comes in whole first (its buffer); then the heap is short.
    auto send = [&](const std::string& envelope) {
        line.t.feed(envelope.data(), envelope.size());
        memory_cases::Floor floor(8000, 30000);
        memory_cases::Heap heap(3840);
        size_t before = line.lines.out.size();
        line.t.feed('\n');
        return line.lines.out.size() == before + 1 ? line.lines.out.back() : std::string("<no reply>");
    };
    {
        // A wrong crc32 of the whole chunk is caught, though only a part
        // would be taken.
        CHECK(send("{\"id\":2,\"op\":\"upload\",\"path\":\"/big\",\"offset\":0,\"crc32\":" +
                        std::to_string(sum + 1) + ",\"data\":\"" + chunk + "\"}")
                  .find("\"reason\":\"checksum\"") != std::string::npos);
        // (3840 - 1024 headroom - 1024) * 3/4 = 1344: 1152 in steps of 384.
        CHECK_EQ(send("{\"id\":3,\"op\":\"upload\",\"path\":\"/big\",\"offset\":0,\"crc32\":" +
                           std::to_string(sum) + ",\"data\":\"" + chunk + "\"}"),
                 "{\"id\":3,\"status\":\"ok\",\"body\":{\"received\":1152}}");
        // The client goes on from there, to the end.
        size_t at = 1152;
        std::string last;
        for (int i = 0; i < 20 && at < big.size(); i++) {
            size_t n = big.size() - at < 2880 ? big.size() - at : 2880;
            last = send("{\"op\":\"upload\",\"path\":\"/big\",\"offset\":" + std::to_string(at) +
                             ",\"data\":\"" + b64(big.substr(at, n)) + "\"}");
            if (last.find("\"received\":") == std::string::npos) break;
            at = std::stoul(last.substr(last.find("\"received\":") + 11));
        }
        CHECK_EQ(last, "{\"status\":\"ok\",\"body\":null}");
        // The chunk that completes an upload is taken whole or not at all:
        // its reply is the upload's, which a client that has sent the end
        // waits for.
        CHECK(send("{\"id\":9,\"op\":\"upload\",\"path\":\"/big\",\"size\":2880,\"data\":\"" + chunk + "\"}")
                  .find("\"reason\":\"low_memory\"") != std::string::npos);
    }
    CHECK_EQ(got, big);
    CHECK_EQ(line.send("{\"id\":10,\"op\":\"upload\",\"path\":\"/big\",\"size\":2880,\"data\":\"" + chunk + "\"}"),
             "{\"id\":10,\"status\":\"ok\",\"body\":null}");
    CHECK_EQ(got, big.substr(0, 2880));
}

TEST(relay_peer_limits) {
    // Each node advertises the largest message it takes; a node sends a
    // peer no more (docs/DESIGN.md section 14.1). Here the gateway takes
    // 600 bytes, the device 700.
    Rig r;
    r.deviceEnd.peerSeen(addr(1), "gw", 0, tesser::PeerAddress(), 2, 600);
    r.gatewayEnd.peerSeen(addr(2), "dev", 0, tesser::PeerAddress(), 2, 700);
    r.net.run();
    CHECK_EQ(r.gatewayEnd.peerLimit(addr(2)), 700u);
    CHECK_EQ(r.deviceEnd.replyLimitFor(addr(1)), 600u);
    // The device's download chunks fit what the gateway takes:
    // (600 - 256) * 3/4 = 258 bytes, where its own limits allow 576.
    std::string first = r.ask("{\"id\":1,\"op\":\"download\",\"path\":\"/dev/big\"}");
    CHECK(first.find("\"chunk\":258,\"offset\":0,\"data\":\"" + b64(r.big.substr(0, 258)) + "\"") !=
          std::string::npos);
    CHECK_EQ(r.downloadAll("/dev/big", 100000), r.big);
    // A request larger than the device takes is refused at once.
    std::string body(800, 'a');
    std::string big = r.ask("{\"id\":2,\"op\":\"set\",\"path\":\"/dev/x\",\"body\":\"" + body + "\"}");
    CHECK(big.find("\"status\":\"too_large\"") != std::string::npos);
    CHECK(big.find("up to 700 bytes") != std::string::npos);
    // A relayed upload goes in parts the device takes.
    std::string data = r.big.substr(0, 2000);
    CHECK(r.uploadAll("/dev/big", data, 576).find("\"status\":\"ok\"") != std::string::npos);
    CHECK_EQ(r.got, data);
    CHECK_EQ(r.gatewayEnd.stats().tooLarge, 0u);
    // Its view=info says what it takes and sends.
    CHECK(r.ask("{\"id\":3,\"op\":\"get\",\"path\":\"/dev\",\"view\":\"info\"}")
              .find("\"limits\":{\"request\":1024,\"response\":600}") != std::string::npos);
}

TEST(relay_download_crc32) {
    // Asked by the client, the gateway adds the chunk's crc32, computed from
    // the device's text: the device isn't asked (it may not know it).
    Rig r;
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"download\",\"path\":\"/dev/sys/config\",\"length\":4,\"crc32\":true}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"size\":11,\"filename\":\"config\",\"contentType\":"
             "\"application/json\",\"chunk\":576,\"offset\":0,\"data\":\"eyJsZQ==\",\"crc32\":1378081651,\"end\":false}}");
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

TEST(relay_download_name) {
    // The client's filename goes on with the download's opening chunk, as
    // an upload's does, and only with it.
    Rig r;
    r.dir.reset(new file_cases::Dir(r.device.api));
    CHECK_EQ(r.ask("{\"id\":1,\"op\":\"download\",\"path\":\"/dev/files/file\",\"filename\":\"cal.json\","
                   "\"length\":2}"),
             "{\"id\":1,\"status\":\"ok\",\"body\":{\"size\":7,\"filename\":\"cal.json\",\"contentType\":"
             "\"application/octet-stream\",\"chunk\":576,\"offset\":0,\"data\":\"eyI=\",\"end\":false}}");
    CHECK_EQ(r.ask("{\"id\":2,\"op\":\"download\",\"path\":\"/dev/files/file\",\"offset\":2}"),
             "{\"id\":2,\"status\":\"ok\",\"body\":{\"offset\":2,\"data\":\"ayI6MX0=\",\"end\":true}}");
    CHECK_EQ(r.ask("{\"id\":3,\"op\":\"download\",\"path\":\"/dev/files/file\",\"filename\":\"nope\"}"),
             "{\"id\":3,\"status\":\"not_found\",\"body\":{\"error\":\"not_found\",\"path\":\"/dev/files/file\","
             "\"message\":\"no such file\"}}");
    CHECK(r.ask("{\"op\":\"download\",\"path\":\"/dev/files/file\"}").find("the file has no name") !=
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

TEST(relay_probe_remembered) {
    // Over HTTP, whether a path below a remote node is a file decides how
    // a request goes: asked of the peer once, then remembered while it
    // advertises the same schema hash.
    Threaded t;
    Rig& r = *t.rig;
    tesser::Client admin;
    admin.authenticated = true;
    auto requests = [&] { return r.deviceEnd.stats().requests; };
    auto notFile = [&] {
        FileTransfer f(r.gateway, FileTransfer::Remotes::Wait);
        tesser::StringReply reply;
        return f.openDownload("/dev/sys/level", admin, reply) == FileTransfer::Open::NotFile;
    };
    auto download = [&] {
        FileTransfer f(r.gateway, FileTransfer::Remotes::Wait);
        tesser::StringReply reply;
        return f.openDownload("/dev/big", admin, reply) == FileTransfer::Open::Ok;
    };
    // No hash advertised: nothing would say an answer went stale.
    uint32_t before = requests();
    CHECK(notFile());
    CHECK(notFile());
    CHECK_EQ(requests() - before, 2u);
    r.dev.setAdvertisedSchema(0x1234);
    before = requests();
    CHECK(notFile());
    CHECK(notFile());
    CHECK_EQ(requests() - before, 1u);  // asked once
    before = requests();
    CHECK(download());  // the probe, then the first chunk
    CHECK(download());  // the first chunk only
    CHECK_EQ(requests() - before, 3u);
    // A new schema (new firmware): asked again.
    r.dev.setAdvertisedSchema(0x5678);
    before = requests();
    CHECK(notFile());
    CHECK_EQ(requests() - before, 1u);
}

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
        // A name the client gave (HTTP: ?filename=) reaches the device.
        {
            std::lock_guard<std::recursive_mutex> lock(t.mutex);  // the pump stays out of the device
            r.dir.reset(new file_cases::Dir(r.device.api));
        }
        FileTransfer f(r.gateway, FileTransfer::Remotes::Wait);
        tesser::StringReply reply;
        CHECK(f.openDownload("/dev/files/file", admin, reply, "notes.txt") == FileTransfer::Open::Ok);
        CHECK_EQ(f.filename(), "notes.txt");
        uint8_t buf[16];
        CHECK_EQ(f.read(buf, sizeof(buf)), 5);
        CHECK_EQ(std::string(reinterpret_cast<char*>(buf), 5), "hello");
        CHECK(f.openDownload("/dev/files/file", admin, reply, "nope") == FileTransfer::Open::Failed);
        CHECK_EQ(reply.body, "{\"error\":\"not_found\",\"path\":\"/dev/files/file\",\"message\":\"no such file\"}");
        f.cancel();
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

TEST(relay_survives_a_link_loss) {
    // NowTP loses a peer whose announcements it missed, though its messages
    // come: on both sides mid-transfer, the relay goes on by its route
    // (DatagramEndpoint::linkLost(); "no upload in progress" relaying a
    // firmware image from DEVKIT-V3 to DEVKIT-R1).
    Rig r;
    auto chunk = [&](size_t at, size_t n) {
        return r.ask("{\"op\":\"upload\",\"path\":\"/dev/big\",\"offset\":" + std::to_string(at) + ",\"data\":\"" +
                     b64(r.big.substr(at, n)) + "\"}");
    };
    CHECK_EQ(r.ask("{\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":3000,\"data\":\"" + b64(r.big.substr(0, 576)) +
                   "\"}"),
             "{\"status\":\"ok\",\"body\":{\"received\":576,\"chunk\":576}}");
    r.gatewayEnd.linkLost(addr(2));
    r.deviceEnd.linkLost(addr(1));
    r.gatewayEnd.process();
    r.deviceEnd.process();
    CHECK(!r.dev.online());
    CHECK(r.gatewayEnd.lossPending(addr(2)));
    CHECK(r.deviceEnd.lossPending(addr(1)));
    for (size_t at = 576; at < 2880; at += 576) {
        CHECK_EQ(chunk(at, 576), "{\"status\":\"ok\",\"body\":{\"received\":" + std::to_string(at + 576) + "}}");
    }
    CHECK_EQ(chunk(2880, 120), "{\"status\":\"ok\",\"body\":null}");
    CHECK_EQ(r.got, r.big);
    CHECK(r.dev.online());  // heard again
    CHECK(!r.gatewayEnd.lossPending(addr(2)));

    // A device silent for the grace is lost after all: the transfer ends.
    r.got.clear();
    CHECK(r.ask("{\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":3000,\"data\":\"" + b64(r.big.substr(0, 576)) +
                "\"}")
              .find("\"received\":576") != std::string::npos);
    r.gatewayEnd.linkLost(addr(2));
    r.gatewayEnd.process();
    r.net.clock += DatagramEndpoint::kLossGraceMs;
    r.gatewayEnd.process();
    CHECK(!r.gatewayEnd.lossPending(addr(2)));
    CHECK_EQ(chunk(576, 576), "{\"status\":\"timeout\",\"body\":{\"error\":\"timeout\",\"reason\":\"peer_offline\","
                              "\"path\":\"/dev\",\"message\":\"peer offline\"}}");
    CHECK(chunk(576, 576).find("no upload in progress") != std::string::npos);
    CHECK(r.got.empty());
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(relay_idle_transfer_is_a_deadline) {
    Rig r;
    CHECK(r.ask("{\"id\":1,\"op\":\"upload\",\"path\":\"/dev/big\",\"size\":1500,\"data\":\"" + b64(r.big.substr(0, 576)) +
                "\"}")
              .find("\"received\":576") != std::string::npos);
    CHECK_EQ(r.deviceEnd.fileSessions(), 1u);
    tesser::Deadline at(r.net.clock);
    r.deviceEnd.nextTick(at);
    CHECK_EQ(at.delay(1000000), r.device.api.config().transferIdleMs);
}

TEST(relay_envelopes_on_the_wire) {
    // What each stage of a relayed transfer sends the peer, member by member.
    std::vector<std::string> sent;
    Rig* rig = nullptr;
    Rig r(nullptr, [&](const tesser::PeerAddress& to, std::string m, tesser::Delivery d) {
        sent.push_back(m.substr(m.find(',') + 1));  // after the call's id
        return rig->net.sender(addr(1))(to, m, d);
    });
    rig = &r;
    r.dir.reset(new file_cases::Dir(r.device.api));

    auto relay = r.dev.relayFile(r.gateway, "/big", "/dev/big");
    relay->probe([](Status, JsonVariantConst) {});
    r.net.run();
    CHECK_EQ(sent.at(0), "\"op\":\"get\",\"path\":\"/big\",\"view\":\"schema\",\"depth\":0,\"v\":2}");
    relay.reset();

    // Upload: the opening envelope, with its name, then the parts.
    sent.clear();
    std::string text(600, 'x');
    CHECK(r.ask("{\"id\":1,\"op\":\"upload\",\"path\":\"/dev/files/file\",\"size\":600,\"filename\":\"a.json\",\"data\":\"" +
                b64(text) + "\"}")
              .find("\"status\":\"ok\"") != std::string::npos);
    CHECK_EQ(sent.size(), 3u);
    CHECK_EQ(sent.at(0), "\"op\":\"upload\",\"path\":\"/files/file\",\"size\":600,\"filename\":\"a.json\",\"v\":2}");
    CHECK_EQ(sent.at(1), "\"op\":\"upload\",\"path\":\"/files/file\",\"offset\":0,\"v\":2,\"data\":\"" +
                             b64(text.substr(0, 576)) + "\"}");
    CHECK_EQ(sent.at(2), "\"op\":\"upload\",\"path\":\"/files/file\",\"offset\":576,\"v\":2,\"data\":\"" +
                             b64(text.substr(576)) + "\"}");

    // Download: the opening chunk's name, and the offset after it.
    sent.clear();
    r.ask("{\"id\":2,\"op\":\"download\",\"path\":\"/dev/files/file\",\"filename\":\"a.json\",\"length\":2}");
    r.ask("{\"id\":3,\"op\":\"download\",\"path\":\"/dev/files/file\",\"offset\":2,\"length\":3}");
    CHECK_EQ(sent.size(), 2u);
    CHECK_EQ(sent.at(0), "\"op\":\"download\",\"path\":\"/files/file\",\"filename\":\"a.json\",\"length\":2,\"v\":2}");
    CHECK_EQ(sent.at(1), "\"op\":\"download\",\"path\":\"/files/file\",\"offset\":2,\"length\":3,\"v\":2}");

    // Below admin, the ops say the client's level.
    sent.clear();
    relay = r.dev.relayFile(r.gateway, "/big", "/dev/big", tesser::Access::User);
    relay->probe([](Status, JsonVariantConst) {});
    relay->openUpload(1, "", [](Status, JsonVariantConst) {});
    relay->download(0, 1, [](Status, JsonVariantConst, std::string_view) {});
    r.net.run();
    CHECK_EQ(sent.size(), 3u);
    CHECK_EQ(sent.at(0), "\"op\":\"get@user\",\"path\":\"/big\",\"view\":\"schema\",\"depth\":0,\"v\":2}");
    CHECK_EQ(sent.at(1), "\"op\":\"upload@user\",\"path\":\"/big\",\"size\":1,\"v\":2}");
    CHECK_EQ(sent.at(2), "\"op\":\"download@user\",\"path\":\"/big\",\"length\":1,\"v\":2}");
}

}  // namespace relay_cases

// A remote node's units on their own, without a node, an endpoint or a link
// (docs/DESIGN.md section 16): the streamed-set queue.
#pragma once

#include <string>
#include <vector>

#include "check.h"
#include "tesser/detail/remote_stream.h"

namespace remote_unit_cases {

using tesser::Status;
using tesser::detail::SetStream;

inline ArduinoJson::JsonDocument parsed(const char* json) {
    ArduinoJson::JsonDocument d;
    deserializeJson(d, json);
    return d;
}

TEST(unit_stream_merges_and_orders) {
    SetStream s;
    CHECK_EQ(s.push("out", parsed("{\"level\":1}")), Status::Ok);  // "out" is "/out"
    CHECK_EQ(s.push("/out/", parsed("{\"on\":true}")), Status::Ok);  // merged
    CHECK_EQ(s.push("/out/level", parsed("2")), Status::Ok);  // below it: queued after
    CHECK_EQ(s.push("/out", parsed("{\"level\":3}")), Status::Ok);  // can't merge past it
    CHECK_EQ(s.size(), 3u);
    CHECK_EQ(s.stats().queued, 4u);
    CHECK_EQ(s.stats().merged, 1u);

    std::string path, body;
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/out");
    CHECK_EQ(body, "{\"level\":1,\"on\":true}");
    CHECK(!s.next(true, path, body));  // one in flight at a time
    CHECK(s.done(Status::Ok, ArduinoJson::JsonVariantConst()));
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/out/level");
    CHECK_EQ(body, "2");
    CHECK(s.done(Status::Ok, ArduinoJson::JsonVariantConst()));
    CHECK(s.next(true, path, body));
    CHECK_EQ(body, "{\"level\":3}");
    CHECK(s.done(Status::Ok, ArduinoJson::JsonVariantConst()));
    CHECK_EQ(s.size(), 0u);
    CHECK_EQ(s.stats().sent, 3u);
}

TEST(unit_stream_limit_and_retries) {
    SetStream s;
    s.setLimit(2);
    CHECK_EQ(s.push("/a", parsed("{\"x\":1}")), Status::Ok);
    CHECK_EQ(s.push("/b", parsed("{\"x\":1}")), Status::Ok);
    CHECK_EQ(s.push("/c", parsed("{\"x\":1}")), Status::Busy);
    CHECK_EQ(s.push("/b", parsed("{\"y\":2}")), Status::Ok);  // merging needs no room

    std::vector<std::string> failed;
    s.onError([&](const std::string& path, Status st, ArduinoJson::JsonVariantConst) {
        failed.push_back(path);
        CHECK_EQ(st, Status::Timeout);
    });
    std::string path, body;
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/a");
    // A newer set to the path comes while it is in flight: a timed-out one
    // goes again with it merged on top.
    CHECK_EQ(s.push("/a", parsed("{\"x\":5}")), Status::Ok);
    CHECK(s.done(Status::Timeout, ArduinoJson::JsonVariantConst()));
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/a");
    CHECK_EQ(body, "{\"x\":5}");
    // Busy: sent again later (the node's tick), not at once.
    CHECK(!s.done(Status::Busy, ArduinoJson::JsonVariantConst()));
    CHECK(s.next(true, path, body));
    CHECK(s.done(Status::Timeout, ArduinoJson::JsonVariantConst()));  // the third attempt
    CHECK_EQ(failed.size(), 1u);
    CHECK_EQ(s.stats().failed, 1u);

    // Couldn't be sent: it waits at the front, not counted as sent.
    uint32_t sent = s.stats().sent;
    CHECK(s.next(true, path, body));
    CHECK_EQ(path, "/b");
    s.unsent();
    CHECK_EQ(s.stats().sent, sent);
    CHECK_EQ(s.size(), 1u);

    // Unreachable: dropped and reported.
    CHECK(!s.next(false, path, body));
    CHECK_EQ(s.size(), 0u);
    CHECK_EQ(failed.size(), 2u);
    if (failed.size() == 2) CHECK_EQ(failed[1], "/b");
    CHECK_EQ(s.stats().failed, 2u);
    CHECK(!s.done(Status::Ok, ArduinoJson::JsonVariantConst()));  // nothing in flight
}

}  // namespace remote_unit_cases

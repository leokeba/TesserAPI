// Streamed envelope replies (docs/DESIGN.md section 10.1).
#pragma once

#include <string>
#include <vector>

#include "core_cases.h"

namespace stream_cases {

using tesser::Api;
using tesser::EnvelopeId;
using tesser::EnvelopeReply;
using tesser::Status;

struct Piece {
    std::string data;
    bool first;
    bool final;
};

// Records the pieces of streamed replies; `failAfter` pieces succeed, then
// sending fails.
struct Pieces {
    std::vector<Piece> got;
    int failAfter = -1;
    EnvelopeReply::Fragment fn() {
        return [this](const char* data, size_t len, bool first, bool final) {
            got.push_back({std::string(data ? data : "", len), first, final});
            return failAfter < 0 || static_cast<int>(got.size()) <= failAfter;
        };
    }
    std::string joined() const {
        std::string s;
        for (const Piece& p : got) s += p.data;
        return s;
    }
};

// A list long enough to exceed any small limit.
struct Big {
    std::vector<int> items;
    Api api;
    Big() {
        items.resize(200);
        for (size_t i = 0; i < items.size(); i++) items[i] = static_cast<int>(i);
        api.list("items", items, [](tesser::Object& o, int& v) { o.value("v", v); });
    }
};

TEST(stream_small_reply_is_one_piece) {
    cases::Device d;
    Pieces p;
    std::string buffered;
    tesser::handleEnvelope(d.api, "{\"id\":1,\"op\":\"get\",\"path\":\"/lamp/on\"}", tesser::Client(),
                           [&](const std::string& m) { buffered = m; }, nullptr, p.fn());
    CHECK(buffered.empty());
    CHECK_EQ(p.got.size(), 1u);
    if (p.got.size() == 1) {
        CHECK(p.got[0].first && p.got[0].final);
        CHECK_EQ(p.got[0].data, "{\"id\":1,\"status\":\"ok\",\"body\":false}");
    }
}

TEST(stream_large_reply_in_pieces) {
    Big b;
    b.api.config().maxResponse = 256;  // doesn't apply to streamed replies
    Pieces p;
    EnvelopeId id;
    EnvelopeReply reply(id, b.api.config().maxResponse, nullptr);
    reply.stream(p.fn(), 100);
    tesser::Request req;
    req.path = "/items";
    b.api.handle(req, reply);
    CHECK(p.got.size() > 10);
    bool shape = !p.got.empty() && p.got.front().first && p.got.back().final;
    for (size_t i = 0; i < p.got.size(); i++) {
        if (i > 0) shape = shape && !p.got[i].first;
        if (i + 1 < p.got.size()) shape = shape && !p.got[i].final && p.got[i].data.size() >= 100;
    }
    CHECK(shape);
    std::string all = p.joined();
    CHECK(all.size() > 1500);
    CHECK_EQ(all.substr(0, 37), "{\"status\":\"ok\",\"body\":[{\"v\":0},{\"v\":1");
    CHECK_EQ(all.substr(all.size() - 11), "{\"v\":199}]}");
    JsonDocument doc;
    CHECK(!deserializeJson(doc, all));
    CHECK_EQ(doc["body"].as<JsonArrayConst>().size(), 200u);
}

TEST(stream_error_before_first_piece) {
    // An error found before anything went out replaces the reply as usual.
    cases::Device d;
    Pieces p;
    tesser::handleEnvelope(d.api, "{\"id\":2,\"op\":\"get\",\"path\":\"/nope\"}", tesser::Client(), nullptr, nullptr,
                           p.fn());
    CHECK_EQ(p.got.size(), 1u);
    if (!p.got.empty()) {
        CHECK(p.got[0].first && p.got[0].final);
        CHECK(p.got[0].data.find("\"status\":\"not_found\"") != std::string::npos);
    }
}

TEST(stream_send_failure) {
    // Once a piece fails, rendering stops and only an empty final piece follows.
    Big b;
    Pieces p;
    p.failAfter = 2;
    EnvelopeId id;
    EnvelopeReply reply(id, 0, nullptr);
    reply.stream(p.fn(), 64);
    tesser::Request req;
    req.path = "/items";
    b.api.handle(req, reply);
    CHECK_EQ(p.got.size(), 4u);
    if (p.got.size() == 4) {
        CHECK(!p.got[2].final);
        CHECK(p.got[3].final && p.got[3].data.empty());
    }
}

TEST(stream_unfinished_reply_still_ends) {
    // A reply destroyed between pieces (never ended) owes a final piece.
    Pieces p;
    {
        EnvelopeId id;
        EnvelopeReply reply(id, 0, nullptr);
        reply.stream(p.fn(), 8);
        tesser::Sink& sink = reply.begin(Status::Ok);
        sink.write("[1,2,3,4,5,6]", 13);
    }
    CHECK(p.got.size() >= 2);
    if (p.got.size() >= 2) {
        CHECK(p.got.front().first);
        CHECK(p.got.back().final && p.got.back().data.empty());
    }
}

TEST(stream_deferred_reply_is_buffered) {
    Api api;
    tesser::Pending saved;
    api.action("slow", [&](tesser::Call& call) { saved = call.defer(); });
    Pieces p;
    std::string buffered;
    tesser::handleEnvelope(api, "{\"id\":3,\"op\":\"set\",\"path\":\"/slow\"}", tesser::Client(),
                           [&](const std::string& m) { buffered = m; }, nullptr, p.fn());
    CHECK(p.got.empty());
    saved.reply(7);
    CHECK_EQ(buffered, "{\"id\":3,\"status\":\"ok\",\"body\":7}");
    CHECK(p.got.empty());
}

TEST(stream_line_transport) {
    // Serial responses stream: no limit, one line however long.
    Big b;
    b.api.config().maxResponse = 128;
    cases::Lines lines;
    std::vector<size_t> writes;
    tesser::LineTransport t(b.api, [&](const char* data, size_t n) {
        writes.push_back(n);
        lines.add(data, n);
    });
    const char* req = "{\"id\":4,\"op\":\"get\",\"path\":\"/items\"}\n";
    t.feed(req, strlen(req));
    CHECK_EQ(lines.out.size(), 1u);
    CHECK(writes.size() > 2);
    if (!lines.out.empty()) {
        JsonDocument doc;
        CHECK(!deserializeJson(doc, lines.out[0]));
        CHECK_EQ(doc["id"].as<int>(), 4);
        CHECK_EQ(doc["body"].as<JsonArrayConst>().size(), 200u);
    }
}

}  // namespace stream_cases

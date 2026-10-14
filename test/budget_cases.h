// The byte accountant (docs/DESIGN.md section 15): buffers reserve their
// bytes, budgets nest, and the heap floor is checked once for everyone.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core_cases.h"
#include "datagram_cases.h"
#include "memory_cases.h"
#include "subscription_cases.h"
#include "udp_cases.h"

namespace budget_cases {

using tesser::ByteBudget;
using tesser::Reservation;
using tesser::Status;

TEST(budget_limits_and_keys) {
    ByteBudget parent("parent", 1000);
    ByteBudget child("child", 800, 500, &parent);
    Reservation a = child.reserve(400, 1, true, false);
    CHECK(!!a);
    CHECK(!child.reserve(200, 1, true, false));  // key 1 would hold 600 > 500
    Reservation b = child.reserve(300, 2, true, false);
    CHECK(!!b);
    CHECK(!child.reserve(200, 3, true, false));  // the child would hold 900 > 800
    CHECK_EQ(child.used(), 700u);
    CHECK_EQ(parent.used(), 700u);
    CHECK_EQ(child.usedBy(1), 400u);
    CHECK_EQ(child.refused(), 2u);
    Reservation c = parent.reserve(300, 0, true, false);
    CHECK(!!c);
    CHECK(!parent.reserve(1, 0, true, false));
    // Moved, released, resized.
    Reservation moved = std::move(a);
    CHECK(!a);
    CHECK_EQ(moved.bytes(), 400u);
    CHECK(!moved.resize(600));  // key 1 cap
    CHECK(moved.resize(100));
    CHECK_EQ(child.usedBy(1), 100u);
    moved.release();
    b = Reservation();
    CHECK_EQ(child.used(), 0u);
    CHECK_EQ(parent.used(), 300u);
    CHECK_EQ(child.peak(), 700u);
    CHECK(tesser::memoryBudget().used() >= 300u);
}

TEST(budget_heap_counts_what_is_about_to_be_taken) {
    // 2000 bytes above the floor: two tasks checking at once can't both
    // count on them. Settled, the heap shows the first buffer itself.
    memory_cases::Floor floor(8000, 10000);
    Reservation a = tesser::reserveBytes(1500);
    CHECK(!!a);
    CHECK(!tesser::reserveBytes(1500));
    CHECK_EQ(tesser::unsettledBytes(), 1500u);
    floor.free(8500);  // the allocation happened
    a.settle();
    CHECK_EQ(tesser::unsettledBytes(), 0u);
    CHECK(!tesser::reserveBytes(1500));
    CHECK(!!tesser::reserveBytes(200));  // small ones always pass
    floor.free(20000);
    // One block: the largest free block, with 1 KB to spare.
    memory_cases::Heap heap(2000);
    CHECK(!tesser::reserveBytes(1500, true));
    CHECK(!!tesser::reserveBytes(1500, false));
}

TEST(budget_incoming_per_peer) {
    // One peer can't fill the queue: its own share runs out first, and its
    // requests are answered busy; another peer still gets in.
    cases::Device dev;
    datagram_cases::Net net;
    tesser::DatagramEndpoint::Options o;
    o.queueBytes = 1000;
    o.queueBytesPerPeer = 600;
    tesser::DatagramEndpoint ep(&dev.api, tesser::TransportKind::NowTP, net.sender(datagram_cases::addr(2)),
                                net.clockFn(), o);
    std::string req = "{\"id\":1,\"op\":\"get\",\"path\":\"/lamp\",\"pad\":\"" + std::string(200, 'p') + "\"}";
    for (int i = 0; i < 3; i++) ep.receive(datagram_cases::addr(1), req.data(), req.size(), true);
    CHECK_EQ(ep.stats().dropped, 1u);
    CHECK_EQ(net.frames.size(), 1u);
    if (!net.frames.empty()) CHECK(net.frames[0].text.find("\"reason\":\"queue_full\"") != std::string::npos);
    ep.receive(datagram_cases::addr(3), req.data(), req.size(), true);
    CHECK_EQ(ep.stats().dropped, 1u);
    ep.process();
    CHECK_EQ(ep.stats().requests, 3u);
    // Handled, the bytes are back.
    for (int i = 0; i < 2; i++) ep.receive(datagram_cases::addr(1), req.data(), req.size(), true);
    CHECK_EQ(ep.stats().dropped, 1u);
}

TEST(budget_link_message_size) {
    // A reply larger than the link's largest message answers too_large, so
    // the client reads in pieces; the link never has to refuse it.
    cases::Device dev;
    std::string big(600, 'x');
    dev.api.value("big", big);
    datagram_cases::Net net;
    tesser::DatagramEndpoint::Options o;
    o.maxMessage = 400;
    tesser::DatagramEndpoint ep(&dev.api, tesser::TransportKind::NowTP, net.sender(datagram_cases::addr(2)),
                                net.clockFn(), o);
    std::string req = "{\"id\":1,\"op\":\"get\",\"path\":\"/big\"}";
    ep.receive(datagram_cases::addr(1), req.data(), req.size(), true);
    ep.process();
    CHECK_EQ(net.frames.size(), 1u);
    if (!net.frames.empty()) {
        CHECK(net.frames[0].text.find("\"status\":\"too_large\"") != std::string::npos);
        CHECK(net.frames[0].text.size() <= 400u);
    }
    // UdpLink gives its endpoint its own limit.
    tesser::UdpLink::Options small;
    small.maxMessage = 1000;
    udp_cases::Pair p(small);
    CHECK_EQ(p.server.endpoint().replyLimit(), 1000u);
}

TEST(budget_udp_outgoing_per_peer) {
    // Reliable messages wait for their acknowledgement holding their bytes:
    // beyond a peer's share, sending to it fails (busy), not to another.
    tesser::UdpLink::Options o;
    o.outgoingBytesPerPeer = 100;  // two requests of about 40 bytes
    udp_cases::Pair p(o);
    p.net.lose = [](const udp_cases::Net::Datagram&) { return true; };
    auto done = [](Status, JsonVariantConst) {};
    int sent = 0;
    for (int i = 0; i < 5; i++) {
        if (p.client.endpoint().request(udp_cases::ip(2), tesser::Op::Get, "/lamp", "", done)) sent++;
    }
    CHECK_EQ(sent, 2);
    CHECK(p.client.endpoint().request(udp_cases::ip(3), tesser::Op::Get, "/lamp", "", done));
}

// A subscriber that keeps what it is given.
struct Keeper : tesser::Subscriber {
    std::vector<std::shared_ptr<const std::string>> shared;
    bool notify(const std::string& m, tesser::Delivery) override {
        shared.push_back(std::make_shared<const std::string>(m));
        return true;
    }
    bool notifyShared(const std::shared_ptr<const std::string>& m, tesser::Delivery) override {
        shared.push_back(m);
        return true;
    }
};

TEST(budget_events_shared) {
    // An event is built once and shared by its subscribers (one buffer per
    // variant: level, overflow), its bytes counted while any holds it.
    cases::Device d;
    tesser::EventNode& ev = d.api.object("button").event("pressed");
    Keeper a, b, c;
    subscription_cases::subscribe(d.api, &a, "/");
    subscription_cases::subscribe(d.api, &b, "/button");
    subscription_cases::subscribe(d.api, &c, "/");
    c.missed = true;  // its copy says "overflow"
    size_t before = tesser::memoryBudget().used();
    ev.emit(3);
    CHECK_EQ(a.shared.size(), 1u);
    CHECK_EQ(b.shared.size(), 1u);
    CHECK_EQ(c.shared.size(), 1u);
    if (a.shared.size() == 1 && b.shared.size() == 1 && c.shared.size() == 1) {
        CHECK(a.shared[0].get() == b.shared[0].get());
        CHECK(c.shared[0].get() != a.shared[0].get());
        CHECK_EQ(*c.shared[0], "{\"op\":\"event\",\"path\":\"/button/pressed\",\"body\":3,\"overflow\":true}");
        CHECK(tesser::memoryBudget().used() > before);
    }
    a.shared.clear();
    b.shared.clear();
    c.shared.clear();
    CHECK_EQ(tesser::memoryBudget().used(), before);
    for (Keeper* k : {&a, &b, &c}) d.api.dropSubscriber(k);
}

TEST(budget_queued_requests) {
    // Queued mode copies a request until poll() runs it: within the budget.
    cases::Device d;
    d.api.config().queued = true;
    memory_cases::Floor floor(8000, 8100);
    std::string body = "\"" + std::string(400, 'x') + "\"";
    std::vector<std::string> out;
    tesser::LocalClient c(d.api);
    c.set("/lamp/label", body, [&](Status s, JsonVariantConst v) {
        std::string t;
        serializeJson(v, t);
        out.push_back(std::string(tesser::toString(s)) + t);
    });
    c.process();
    CHECK_EQ(out.size(), 1u);
    if (!out.empty()) CHECK(out[0].find("busy{\"error\":\"busy\",\"reason\":\"low_memory\"") == 0);
    CHECK_EQ(d.api.queuedRequests(), 0u);
}

TEST(budget_owned_reply_buffers) {
    // A link that sends from a buffer it takes over gets replies built in
    // one, with its slack: the reply isn't copied on its way out.
    cases::Device d;
    tesser::Pending saved;
    d.api.action("later", [&](tesser::Call& call) { saved = call.defer(); });
    datagram_cases::Net net;
    tesser::DatagramEndpoint ep(&d.api, tesser::TransportKind::NowTP, net.sender(datagram_cases::addr(2)),
                                net.clockFn());
    std::vector<size_t> spare;
    std::string got;
    ep.setOwnedSend(
        [&](const tesser::PeerAddress&, tesser::Bytes m, tesser::Delivery) {
            spare.push_back(m.capacity() - m.size());
            got.assign(m.data(), m.size());
            return true;
        },
        32);
    std::string req = "{\"id\":1,\"op\":\"get\",\"path\":\"/lamp\"}";
    ep.receive(datagram_cases::addr(1), req.data(), req.size(), true);
    ep.process();
    CHECK_EQ(got, "{\"id\":1,\"status\":\"ok\",\"body\":{\"on\":false,\"brightness\":128,\"label\":\"desk\"}}");
    CHECK(net.frames.empty());  // not through the copying send
    CHECK(!spare.empty() && spare.back() >= 32u);
    // A deferred reply too, completed later.
    got.clear();
    req = "{\"id\":2,\"op\":\"set\",\"path\":\"/later\"}";
    ep.receive(datagram_cases::addr(1), req.data(), req.size(), true);
    ep.process();
    CHECK(got.empty());
    saved.reply(7);
    CHECK_EQ(got, "{\"id\":2,\"status\":\"ok\",\"body\":7}");
    CHECK(spare.back() >= 32u);
}

TEST(budget_owned_request_buffers) {
    // A request built for a link that takes buffers goes in one, with the
    // link's slack, and is built again (not kept) should it be resent.
    datagram_cases::Net net;
    tesser::DatagramEndpoint ep(nullptr, tesser::TransportKind::NowTP, net.sender(datagram_cases::addr(1)),
                                net.clockFn());
    std::vector<std::string> sent;
    size_t spare = 0;
    ep.setOwnedSend(
        [&](const tesser::PeerAddress&, tesser::Bytes m, tesser::Delivery) {
            spare = m.capacity() - m.size();
            sent.emplace_back(m.data(), m.size());
            return true;
        },
        21);
    int builds = 0;
    auto build = [&builds](uint32_t id, tesser::DatagramEndpoint::EnvelopeOut& out) {
        builds++;
        std::string text = "{\"id\":" + std::to_string(id) + ",\"op\":\"get\",\"path\":\"/\"}";
        if (!out.reserve(text.size())) return false;
        out.append(text);
        return true;
    };
    datagram_cases::Result res;
    CHECK(ep.requestBuilt(datagram_cases::addr(2), build, datagram_cases::capture(res)));
    CHECK_EQ(sent.size(), 1u);
    CHECK(spare >= 21u);
    CHECK(net.frames.empty());  // not through the copying send
    CHECK_EQ(builds, 1);
    // A builder with no room: not sent.
    CHECK(!ep.requestBuilt(datagram_cases::addr(2), [](uint32_t, tesser::DatagramEndpoint::EnvelopeOut&) { return false; },
                           datagram_cases::capture(res)));
    CHECK_EQ(sent.size(), 1u);
}

TEST(budget_growth_falls_back_to_the_need) {
    // Short of room to grow by half again, a buffer grows by what it needs.
    std::string s;
    s.reserve(2000);
    tesser::Bytes b;
    CHECK(b.reserve(2000));
    {
        memory_cases::Floor floor(8000, 10500);
        // By half again: 3000 bytes, and a copy of 2100 for the string.
        CHECK(tesser::growFor(s, 2100));
        CHECK(s.capacity() >= 2100 && s.capacity() < 3000);
        CHECK(tesser::growFor(b, 2100));
        CHECK_EQ(b.capacity(), 2100u);
        CHECK(!tesser::growFor(b, 2600));  // the need alone doesn't fit either
        CHECK_EQ(b.capacity(), 2100u);
    }
    CHECK_EQ(tesser::unsettledBytes(), 0u);
    CHECK(tesser::growFor(b, 2200));  // room again: by half
    CHECK_EQ(b.capacity(), 3150u);
}

}  // namespace budget_cases

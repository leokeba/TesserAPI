// A link that reports failed deliveries (NowTP's completion handlers):
// DatagramEndpoint::sendFailed() fails calls at once instead of letting them
// time out (docs/DESIGN.md section 10.3).
#pragma once

#include <string>
#include <vector>

#include "datagram_cases.h"
#include "remote_cases.h"

namespace link_failure_cases {

using datagram_cases::addr;
using tesser::PeerLoss;
using tesser::SendFailure;
using tesser::Status;

// Sends `envelope` to the gateway; the frame to the device is held back and
// reported as `failure`, as the link would.
inline std::string failedAsk(remote_cases::Rig& r, const std::string& envelope, SendFailure failure) {
    r.lines.out.clear();
    std::string l = envelope + "\n";
    r.line.feed(l.data(), l.size());
    uint32_t id = 0;
    for (size_t i = 0; i < r.net.frames.size(); i++) {
        if (r.net.frames[i].to != addr(2)) continue;
        id = tesser::DatagramEndpoint::callIdOf(r.net.frames[i].text);
        r.net.frames.erase(r.net.frames.begin() + static_cast<std::ptrdiff_t>(i));
        break;
    }
    r.gatewayEnd.sendFailed(addr(2), id, failure);
    r.net.run();  // no clock advance: no timeout involved
    return r.lines.out.size() == 1 ? r.lines.out[0] : "<" + std::to_string(r.lines.out.size()) + " lines>";
}

TEST(link_call_ids) {
    using tesser::DatagramEndpoint;
    CHECK_EQ(DatagramEndpoint::callIdOf("{\"id\":42,\"op\":\"get\",\"path\":\"/\"}"), 42u);
    CHECK_EQ(DatagramEndpoint::callIdOf("{\"id\":42,\"status\":\"ok\",\"body\":1}"), 0u);  // a reply
    CHECK_EQ(DatagramEndpoint::callIdOf("{\"op\":\"change\",\"path\":\"/\",\"body\":{}}"), 0u);
    CHECK_EQ(DatagramEndpoint::callIdOf(tesser::buildRequestEnvelope(7, tesser::Op::Set, "/a", tesser::Query(), "1")),
             7u);
}

TEST(link_undelivered_fails_now) {
    remote_cases::Rig r;
    std::vector<PeerLoss> losses;
    r.gatewayEnd.onPeerLost([&](const tesser::PeerAddress&, const tesser::PeerAddress&, PeerLoss why) {
        losses.push_back(why);
    });
    CHECK(r.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/kitchen/lamp/on\"}").find("\"ok\"") != std::string::npos);
    CHECK_EQ(failedAsk(r, "{\"id\":2,\"op\":\"get\",\"path\":\"/kitchen/lamp/on\"}", SendFailure::Undelivered),
             "{\"id\":2,\"status\":\"timeout\",\"body\":{\"error\":\"timeout\",\"reason\":\"send_failed\","
             "\"path\":\"/kitchen\",\"message\":\"the link couldn't deliver to the remote node\"}}");
    CHECK(!r.kitchen.online());  // its routes fail over now
    CHECK_EQ(losses.size(), 1u);
    if (!losses.empty()) CHECK(losses[0] == PeerLoss::Failed);
    CHECK_EQ(r.gateway.pending(), 0);
}

TEST(link_auth_failed_is_its_own) {
    remote_cases::Rig r;
    std::vector<PeerLoss> losses;
    r.gatewayEnd.onPeerLost([&](const tesser::PeerAddress&, const tesser::PeerAddress&, PeerLoss why) {
        losses.push_back(why);
    });
    // Never heard from (its frames don't authenticate either): reported all
    // the same, once.
    CHECK(failedAsk(r, "{\"id\":1,\"op\":\"get\",\"path\":\"/kitchen/lamp\"}", SendFailure::AuthFailed)
              .find("\"reason\":\"auth_failed\"") != std::string::npos);
    CHECK_EQ(losses.size(), 1u);
    if (!losses.empty()) CHECK(losses[0] == PeerLoss::AuthFailed);
    CHECK_EQ(std::string(tesser::toString(PeerLoss::AuthFailed)), "auth_failed");
    r.kitchen.setOnline(true);
    failedAsk(r, "{\"id\":2,\"op\":\"get\",\"path\":\"/kitchen/lamp\"}", SendFailure::AuthFailed);
    CHECK_EQ(losses.size(), 1u);
    // Heard again (the key fixed), a new failure is reported again.
    r.kitchen.setOnline(true);
    CHECK(r.ask("{\"id\":3,\"op\":\"get\",\"path\":\"/kitchen/lamp/on\"}").find("\"ok\"") != std::string::npos);
    failedAsk(r, "{\"id\":4,\"op\":\"get\",\"path\":\"/kitchen/lamp\"}", SendFailure::AuthFailed);
    CHECK_EQ(losses.size(), 2u);
}

TEST(link_rejected_fails_that_call) {
    remote_cases::Rig r;
    CHECK(r.ask("{\"id\":1,\"op\":\"get\",\"path\":\"/kitchen/lamp/on\"}").find("\"ok\"") != std::string::npos);
    CHECK(failedAsk(r, "{\"id\":2,\"op\":\"get\",\"path\":\"/kitchen/lamp\"}", SendFailure::Rejected)
              .find("\"status\":\"internal\",\"body\":{\"error\":\"internal\",\"reason\":\"rejected\",\"path\":\"/kitchen\"") !=
          std::string::npos);
    CHECK(r.kitchen.online());  // the peer is there
    // A reply or notification refused (no call id) changes nothing.
    r.gatewayEnd.sendFailed(addr(2), 0, SendFailure::Rejected);
    r.net.run();
    CHECK(r.ask("{\"id\":3,\"op\":\"get\",\"path\":\"/kitchen/lamp/on\"}").find("\"ok\"") != std::string::npos);
    CHECK_EQ(r.gateway.pending(), 0);
}

}  // namespace link_failure_cases

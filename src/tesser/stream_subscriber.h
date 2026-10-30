#pragma once

#include <stdint.h>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>

#include "tesser/request.h"

namespace tesser {

class Api;

// One subscription delivered as lines on a long response: HTTP streamed
// subscriptions (docs/DESIGN.md section 10.2). Platform-independent: the
// transport sends the pieces and calls keepAlive() regularly.
//
// Each line is one notification envelope, as on WebSocket and serial; a
// snapshot is the first line, as a "change" of the whole subtree. A bare
// "\n" keeps an idle stream alive.
class StreamSubscriber : public Subscriber {
public:
    // One piece of the stream, in parts: the transport joins them into the
    // one buffer it sends from, reserved (docs/DESIGN.md section 15), rather
    // than the subscriber building a copy first. A snapshot is its head,
    // the body and "}\n"; a notification, the message and "\n".
    struct Piece {
        std::string_view parts[3];
        size_t count = 0;
        size_t size() const {
            size_t n = 0;
            for (size_t i = 0; i < count; i++) n += parts[i].size();
            return n;
        }
        void appendTo(std::string& out) const {
            for (size_t i = 0; i < count; i++) out.append(parts[i].data(), parts[i].size());
        }
        std::string str() const {
            std::string s;
            appendTo(s);
            return s;
        }
    };
    // Queues one piece of the stream, in order; false if it couldn't.
    using Send = std::function<bool(const Piece& piece)>;

    StreamSubscriber(Api& api, Send send);
    ~StreamSubscriber() override;
    StreamSubscriber(const StreamSubscriber&) = delete;
    StreamSubscriber& operator=(const StreamSubscriber&) = delete;

    // Subscribes for `request` (its op and subscriber are replaced). Ok: the
    // stream is open, and the snapshot, if any, went out as the first line.
    // Otherwise the error was written to `error` and nothing was sent.
    // Below a remote node it waits for the remote's snapshot, up to
    // Config::deferTimeoutMs (then timeout), as a forwarded get does: call
    // it without the API lock. Once per stream.
    Status open(Request request, Reply& error);

    // The token the stream's client presented (none: empty), which
    // Api::tokensChanged() checks again; `fixed`: its level doesn't come
    // from the token check (the transport's own token). Before open().
    // Without it, the stream keeps its level.
    void presented(std::string_view token, bool fixed) {
        token_.token.assign(token.data(), token.size());
        token_.fixed = fixed;
    }

    bool notify(const std::string& message, Delivery delivery) override;
    bool recheckLevel(Access& level) override;
    void levelLowered() override;
    // Its level fell below its subscription's (levelLowered()): the
    // transport closes the stream. True once.
    bool takeRevoked() { return revoked_.exchange(false); }

    // Call regularly (every idleMs / 3, say): sends "\n" once nothing has
    // gone out for `idleMs`. False if that send failed.
    bool keepAlive(uint32_t nowMs, uint32_t idleMs);

    // Drops the subscription; nothing is sent after it.
    void close();
    bool isOpen() const { return open_.load(); }

private:
    bool send(std::string_view a, std::string_view b = std::string_view(), std::string_view c = std::string_view());

    Api& api_;
    Send send_;
    std::atomic<bool> open_{false};
    std::atomic<bool> sent_{false};
    std::atomic<bool> revoked_{false};
    ClientToken token_;
    Client client_;  // as it opened, for the token check
    bool opened_ = false;
    bool ticked_ = false;  // keepAlive() state, in the caller's task
    uint32_t lastMs_ = 0;
};

}  // namespace tesser

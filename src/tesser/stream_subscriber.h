#pragma once

#include <stdint.h>

#include <atomic>
#include <functional>
#include <string>

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
    // Queues one piece of the stream, in order; false if it couldn't.
    using Send = std::function<bool(const std::string& piece)>;

    StreamSubscriber(Api& api, Send send);
    ~StreamSubscriber() override;
    StreamSubscriber(const StreamSubscriber&) = delete;
    StreamSubscriber& operator=(const StreamSubscriber&) = delete;

    // Subscribes for `request` (its op and subscriber are replaced). Ok: the
    // stream is open, and the snapshot, if any, went out as the first line.
    // Otherwise the error was written to `error` and nothing was sent.
    // Subscriptions that would wait (below a remote node) are not_allowed.
    // Once per stream.
    Status open(Request request, Reply& error);

    bool notify(const std::string& message, Delivery delivery) override;

    // Call regularly (every idleMs / 3, say): sends "\n" once nothing has
    // gone out for `idleMs`. False if that send failed.
    bool keepAlive(uint32_t nowMs, uint32_t idleMs);

    // Drops the subscription; nothing is sent after it.
    void close();
    bool isOpen() const { return open_.load(); }

private:
    bool send(const std::string& piece);

    Api& api_;
    Send send_;
    std::atomic<bool> open_{false};
    std::atomic<bool> sent_{false};
    bool opened_ = false;
    bool ticked_ = false;  // keepAlive() state, in the caller's task
    uint32_t lastMs_ = 0;
};

}  // namespace tesser

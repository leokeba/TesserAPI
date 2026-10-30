#pragma once

#include <stdint.h>

namespace tesser {

// The earliest of the times something is due, for a task that sleeps until
// then rather than wake on a fixed period (UdpLink::nextDelayMs()). Times are
// a millisecond clock's, and wrap as it does.
class Deadline {
public:
    explicit Deadline(uint32_t nowMs) : now_(nowMs) {}

    // Something is due at `t`; a time already past is due now.
    void at(uint32_t t) {
        int32_t d = static_cast<int32_t>(t - now_);
        uint32_t ms = d > 0 ? static_cast<uint32_t>(d) : 0;
        if (!set_ || ms < delay_) delay_ = ms;
        set_ = true;
    }
    // Something is due `ms` from now.
    void in(uint32_t ms) { at(now_ + ms); }

    uint32_t now() const { return now_; }
    // Milliseconds until the earliest time, 0 if due; `none` if nothing is.
    uint32_t delay(uint32_t none) const { return set_ ? delay_ : none; }

private:
    uint32_t now_;
    uint32_t delay_ = 0;
    bool set_ = false;
};

}  // namespace tesser

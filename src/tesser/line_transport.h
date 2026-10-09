#pragma once

#include <stddef.h>

#include <functional>
#include <string>

#include "tesser/envelope.h"
#include "tesser/platform.h"

namespace tesser {

class Api;

// Newline-delimited JSON envelopes over a byte stream (serial, TCP, ...).
// Platform-independent: feed it received bytes, and it calls `output` with
// each complete response line (including the trailing '\n'). Lines that
// don't start with '{' are ignored, so log output on a shared port is
// harmless. Responses are written atomically with respect to each other, so
// deferred replies from other tasks never interleave.
//
// The line is one client: it can subscribe, and notifications are written to
// the output like responses.
class LineTransport : public Subscriber {
public:
    using Output = std::function<void(const char* data, size_t len)>;

    LineTransport(Api& api, Output output);
    ~LineTransport() override;

    bool notify(const std::string& message, Delivery delivery) override;

    void feed(const char* data, size_t len);
    void feed(char c);

    // Requests on the line count as authenticated (physical access) unless
    // this is set to false; see Api::authorize().
    void setAuthenticated(bool authenticated) { authenticated_ = authenticated; }

    // Lines dropped for exceeding the limit (Config::maxRequestBody + 256).
    size_t overflows() const { return overflows_; }

private:
    void handleLine();
    void send(const std::string& message);

    Api& api_;
    Output output_;
    std::string line_;
    Mutex outputMutex_;
    bool discarding_ = false;
    bool authenticated_ = true;
    size_t overflows_ = 0;
};

}  // namespace tesser

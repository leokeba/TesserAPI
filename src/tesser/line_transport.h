#pragma once

#include <stddef.h>

#include <functional>
#include <string>

#include "tesser/envelope.h"
#include "tesser/file_session.h"
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
// the output like responses. Responses are streamed to the output as they
// are rendered, so Config::maxResponse doesn't limit them; deferred replies
// are buffered and limited by it. File nodes transfer with "upload" and
// "download" envelopes (docs/DESIGN.md section 10.1.1).
class LineTransport : public Subscriber {
public:
    using Output = std::function<void(const char* data, size_t len)>;

    LineTransport(Api& api, Output output);
    ~LineTransport() override;

    bool notify(const std::string& message, Delivery delivery) override;

    void feed(const char* data, size_t len);
    void feed(char c);

    // Requests on the line count as authenticated (physical access), with
    // full access, unless this is set to false; see Api::authorize(). Then
    // the line's level is the one the API's token check (Api::authenticate())
    // gives its token, like an HTTP request's: the empty token until the
    // client sends {"op":"auth","token":"..."} (docs/DESIGN.md section 10.4).
    void setAuthenticated(bool authenticated) { authenticated_ = authenticated; }

    // The line's client is gone (a Bluetooth disconnection): drops its
    // subscriptions, any partial line, its token and its file transfer.
    void reset();

    // Lines dropped for exceeding the limit (Config::maxRequestBody + 256).
    size_t overflows() const { return overflows_; }

private:
    void handleLine();
    void handleAuth(JsonObjectConst envelope, const EnvelopeId& id);
    Access level() const;
    void send(const std::string& message);
    bool sendPiece(const char* data, size_t len, bool first, bool final);

    Api& api_;
    Output output_;
    std::string line_;
    Mutex outputMutex_;
    bool discarding_ = false;
    bool authenticated_ = true;
    std::string token_;  // from the last "auth" envelope
    FileSession files_;
    size_t overflows_ = 0;
};

}  // namespace tesser

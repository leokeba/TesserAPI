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
    bool recheckLevel(Access& level) override;

    void feed(const char* data, size_t len);
    void feed(char c);

    // Requests on the line count as authenticated (physical access), with
    // full access, unless this is set to false; see Api::authorize(). Then
    // the line's level is the one the API's token check (Api::authenticate())
    // gives its token, like an HTTP request's: the empty token until the
    // client sends {"op":"auth","token":"..."} (docs/DESIGN.md section 10.4).
    void setAuthenticated(bool authenticated) { authenticated_ = authenticated; }
    // The transport its clients are shown with (Client::transport), for the
    // token check and the authorizer: Serial by default, Ble on Bluetooth.
    void setTransport(TransportKind kind) { kind_ = kind; }

    // The line's client is gone (a Bluetooth disconnection): drops its
    // subscriptions, any partial line, its token and its file transfer.
    void reset();

    // Lines dropped for exceeding the limit (Config::maxRequestBody + 256).
    size_t overflows() const { return overflows_; }

    // Bytes were lost on the way in (a UART overrun): the line being read
    // is answered busy (line_overrun) under its id at its end, rather than
    // parsed. From the task that feeds the line, between feed() calls;
    // allocates nothing.
    void lineDamaged();
    // A damaged line whose end hasn't come yet.
    bool damagedPending() const { return damaged_; }
    // Lines answered so.
    size_t overruns() const { return overruns_; }

private:
    void handleLine();
    EnvelopeId keptId() const;
    void handleAuth(JsonObjectConst envelope, const EnvelopeId& id);
    Access level() const;
    // A whole message (a notification, `mayDrop`, or a deferred reply):
    // waits in `waiting_` while a reply streams. False if dropped.
    bool send(const std::string& message, bool mayDrop);
    bool sendPiece(const char* data, size_t len, bool first, bool final);

    Api& api_;
    Output output_;
    std::string line_;
    // Held only briefly: a streamed reply sets streaming_ and writes its
    // pieces without it (and without the API lock, docs/DESIGN.md section
    // 9); whole messages wait in waiting_ meanwhile, written after its end.
    Mutex outputMutex_{kRankLeaf, "line output"};
    bool streaming_ = false;
    std::string waiting_;
    bool discarding_ = false;
    bool starved_ = false;  // discarding_ because the heap couldn't hold the line
    bool damaged_ = false;  // lineDamaged(): bytes of this line were lost
    bool authenticated_ = true;
    TransportKind kind_ = TransportKind::Serial;
    std::string token_;  // from the last "auth" envelope; written under the API lock
    FileSession files_;
    size_t overflows_ = 0;
    size_t overruns_ = 0;
};

}  // namespace tesser

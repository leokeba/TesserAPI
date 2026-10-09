#pragma once

// Arduino only: JSON envelopes over any Stream (Serial, a TCP client, ...),
// one per line. Call poll() from loop(); handlers run there.
#if defined(ARDUINO)

#include <Arduino.h>

#include "tesser/line_transport.h"

namespace tesser {

class StreamTransport {
public:
    StreamTransport(Api& api, Stream& stream)
        : stream_(stream),
          line_(api, [this](const char* data, size_t len) {
              stream_.write(reinterpret_cast<const uint8_t*>(data), len);
          }) {}

    void poll() {
        while (stream_.available() > 0) {
            int c = stream_.read();
            if (c < 0) break;
            line_.feed(static_cast<char>(c));
        }
    }

    LineTransport& lines() { return line_; }

private:
    Stream& stream_;
    LineTransport line_;
};

}  // namespace tesser

#endif

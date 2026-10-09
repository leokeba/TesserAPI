#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string_view>

#include <ArduinoJson.h>

#include "tesser/sink.h"

namespace tesser {

// Streaming JSON writer. Inserts commas and colons itself; the caller only
// describes structure. Nothing is buffered: every token goes straight to the
// sink. After the first sink failure (or nesting deeper than kMaxNesting)
// ok() turns false and further output is dropped.
class JsonWriter {
public:
    static constexpr int kMaxNesting = 64;

    explicit JsonWriter(Sink& sink) : sink_(sink) {}

    void beginObject();
    void endObject();
    void beginArray();
    void endArray();
    void key(std::string_view name);

    void null();
    void boolean(bool v);
    void integer(int64_t v);
    void uinteger(uint64_t v);
    // Shortest of %.<digits>g; NaN and infinities are written as null.
    void number(double v, int digits = 15);
    void string(std::string_view v);
    // Serializes an ArduinoJson value.
    void variant(JsonVariantConst v);
    // Pre-serialized JSON, written as one value.
    void raw(std::string_view json);

    bool ok() const { return ok_; }
    Sink& sink() { return sink_; }

private:
    void beforeValue();
    void quoted(std::string_view v);
    void put(const char* data, size_t len);
    void put(char c) { put(&c, 1); }
    void push();
    void pop();

    Sink& sink_;
    uint64_t needComma_ = 0;  // one bit per nesting level
    int level_ = 0;
    bool afterKey_ = false;
    bool ok_ = true;
};

}  // namespace tesser

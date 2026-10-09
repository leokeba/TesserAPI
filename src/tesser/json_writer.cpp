#include "tesser/json_writer.h"

#include <math.h>
#include <stdio.h>

namespace tesser {

namespace {

// Adapts a Sink to ArduinoJson's custom writer interface.
class SinkWriter {
public:
    explicit SinkWriter(Sink& sink, bool& ok) : sink_(sink), ok_(ok) {}

    size_t write(uint8_t c) { return write(&c, 1); }

    size_t write(const uint8_t* data, size_t len) {
        if (!ok_) return 0;
        if (!sink_.write(reinterpret_cast<const char*>(data), len)) {
            ok_ = false;
            return 0;
        }
        return len;
    }

private:
    Sink& sink_;
    bool& ok_;
};

}  // namespace

void JsonWriter::put(const char* data, size_t len) {
    if (!ok_ || len == 0) return;
    if (!sink_.write(data, len)) ok_ = false;
}

void JsonWriter::beforeValue() {
    if (afterKey_) {
        afterKey_ = false;
        return;
    }
    if (level_ == 0) return;
    uint64_t bit = uint64_t(1) << (level_ - 1);
    if (needComma_ & bit) put(',');
    needComma_ |= bit;
}

void JsonWriter::push() {
    if (level_ >= kMaxNesting) {
        ok_ = false;
        return;
    }
    level_++;
    needComma_ &= ~(uint64_t(1) << (level_ - 1));
}

void JsonWriter::pop() {
    if (level_ > 0) level_--;
}

void JsonWriter::beginObject() {
    beforeValue();
    put('{');
    push();
}

void JsonWriter::endObject() {
    pop();
    put('}');
}

void JsonWriter::beginArray() {
    beforeValue();
    put('[');
    push();
}

void JsonWriter::endArray() {
    pop();
    put(']');
}

void JsonWriter::key(std::string_view name) {
    beforeValue();
    quoted(name);
    put(':');
    afterKey_ = true;
}

void JsonWriter::null() {
    beforeValue();
    put("null", 4);
}

void JsonWriter::boolean(bool v) {
    beforeValue();
    if (v) {
        put("true", 4);
    } else {
        put("false", 5);
    }
}

void JsonWriter::uinteger(uint64_t v) {
    beforeValue();
    char buf[21];
    size_t i = sizeof(buf);
    do {
        buf[--i] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    put(buf + i, sizeof(buf) - i);
}

void JsonWriter::integer(int64_t v) {
    if (v >= 0) {
        uinteger(static_cast<uint64_t>(v));
        return;
    }
    beforeValue();
    // Negate in unsigned arithmetic so INT64_MIN doesn't overflow.
    uint64_t u = ~static_cast<uint64_t>(v) + 1;
    char buf[21];
    size_t i = sizeof(buf);
    do {
        buf[--i] = static_cast<char>('0' + u % 10);
        u /= 10;
    } while (u);
    buf[--i] = '-';
    put(buf + i, sizeof(buf) - i);
}

void JsonWriter::number(double v, int digits) {
    if (isnan(v) || isinf(v)) {
        null();
        return;
    }
    beforeValue();
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%.*g", digits, v);
    if (n <= 0) {
        ok_ = false;
        return;
    }
    put(buf, static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf) - 1);
}

void JsonWriter::string(std::string_view v) {
    beforeValue();
    quoted(v);
}

void JsonWriter::quoted(std::string_view v) {
    put('"');
    static const char kHex[] = "0123456789abcdef";
    size_t start = 0;
    for (size_t i = 0; i < v.size(); i++) {
        unsigned char c = static_cast<unsigned char>(v[i]);
        const char* esc = nullptr;
        char u[6];
        switch (c) {
            case '"': esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            case '\b': esc = "\\b"; break;
            case '\f': esc = "\\f"; break;
            default:
                if (c < 0x20) {
                    u[0] = '\\';
                    u[1] = 'u';
                    u[2] = '0';
                    u[3] = '0';
                    u[4] = kHex[c >> 4];
                    u[5] = kHex[c & 0xf];
                }
                break;
        }
        if (!esc && c >= 0x20) continue;
        put(v.data() + start, i - start);
        if (esc) {
            put(esc, 2);
        } else {
            put(u, 6);
        }
        start = i + 1;
    }
    put(v.data() + start, v.size() - start);
    put('"');
}

void JsonWriter::variant(JsonVariantConst v) {
    beforeValue();
    if (!ok_) return;
    SinkWriter w(sink_, ok_);
    serializeJson(v, w);
}

void JsonWriter::raw(std::string_view json) {
    beforeValue();
    put(json.data(), json.size());
}

}  // namespace tesser

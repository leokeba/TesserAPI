#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace tesser {

// Destination for response bytes. write() returns false when the bytes could
// not be accepted (buffer full, connection dropped); writers stop at the first
// failure.
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool write(const char* data, size_t len) = 0;
};

// Appends to a std::string, optionally capped at `limit` bytes (0 = no cap).
class StringSink : public Sink {
public:
    explicit StringSink(std::string& out, size_t limit = 0) : out_(out), limit_(limit) {}

    bool write(const char* data, size_t len) override {
        if (limit_ && out_.size() + len > limit_) return false;
        out_.append(data, len);
        return true;
    }

private:
    std::string& out_;
    size_t limit_;
};

// Counts bytes without storing them.
class CountingSink : public Sink {
public:
    bool write(const char*, size_t len) override {
        count += len;
        return true;
    }
    size_t count = 0;
};

// FNV-1a hash of everything written.
class HashSink : public Sink {
public:
    bool write(const char* data, size_t len) override {
        for (size_t i = 0; i < len; i++) {
            hash ^= static_cast<unsigned char>(data[i]);
            hash *= 16777619u;
        }
        return true;
    }
    uint32_t hash = 2166136261u;
};

}  // namespace tesser

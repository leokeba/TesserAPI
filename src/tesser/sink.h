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

// Grows `s` to hold `need` bytes, by half again at a time, unless the heap
// can't give a block that size and leave room for one copy of the message
// further down (a link's own buffer) plus a little headroom, or past 256
// bytes, unless it leaves the heap floor free (setHeapFloor()): false
// then, and `s` is untouched.
bool growFor(std::string& s, size_t need);
// Gives `s` room for exactly `capacity` bytes, keeping its content. With
// libstdc++ (ESP-IDF's, GCC's), reserve() on a string that has allocated
// takes at least twice its capacity, which the byte accountant wouldn't see.
void reserveExactly(std::string& s, size_t capacity);

// Appends to a string, up to `limit` bytes (0: none). `guarded`: grows
// only as far as the heap can give (growFor()); `lowMemory` then tells why
// a write failed. Replies use it; a record or an envelope that mustn't come
// out truncated doesn't.
class StringSink : public Sink {
public:
    explicit StringSink(std::string& out, size_t limit = 0, bool guarded = false)
        : out_(out), limit_(limit), guarded_(guarded) {}

    bool write(const char* data, size_t len) override {
        if (limit_ && out_.size() + len > limit_) return false;
        if (guarded_ && !growFor(out_, out_.size() + len)) {
            lowMemory = true;
            return false;
        }
        out_.append(data, len);
        return true;
    }
    bool lowMemory = false;

private:
    std::string& out_;
    size_t limit_;
    bool guarded_;
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

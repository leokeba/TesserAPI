#pragma once

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <string_view>

#include "tesser/platform.h"

namespace tesser {

// A message in a buffer free() frees, which a link can take over whole
// (release()) rather than copy: NowTP's sendBuffer() (docs/DESIGN.md section
// 15). From Pool::Large (PSRAM when the chip has it) unless told otherwise;
// a growth the heap can't give fails instead of aborting. Move-only.
class Bytes {
public:
    Bytes() = default;
    Bytes(Bytes&& o) noexcept : data_(o.data_), len_(o.len_), cap_(o.cap_) { o.data_ = nullptr, o.len_ = o.cap_ = 0; }
    Bytes& operator=(Bytes&& o) noexcept {
        if (this != &o) {
            free(data_);
            data_ = o.data_;
            len_ = o.len_;
            cap_ = o.cap_;
            o.data_ = nullptr;
            o.len_ = o.cap_ = 0;
        }
        return *this;
    }
    Bytes(const Bytes&) = delete;
    Bytes& operator=(const Bytes&) = delete;
    ~Bytes() { free(data_); }

    const char* data() const { return data_; }
    size_t size() const { return len_; }
    size_t capacity() const { return cap_; }
    bool empty() const { return len_ == 0; }
    std::string_view view() const { return std::string_view(data_ ? data_ : "", len_); }

    // At least `cap` bytes of room, in `pool`: false, unchanged, if the
    // heap can't.
    bool reserve(size_t cap, Pool pool = Pool::Large) {
        if (cap <= cap_) return true;
        char* p = static_cast<char*>(reallocate(data_, cap, pool));
        if (!p) return false;
        data_ = p;
        cap_ = cap;
        return true;
    }
    // Appends within the capacity reserved (false if it doesn't fit).
    bool append(const char* data, size_t len) {
        if (len_ + len > cap_) return false;
        memcpy(data_ + len_, data, len);
        len_ += len;
        return true;
    }
    bool append(std::string_view s) { return append(s.data(), s.size()); }
    void clear() { len_ = 0; }
    // Frees the memory.
    void reset() {
        free(data_);
        data_ = nullptr;
        len_ = cap_ = 0;
    }
    // Hands the buffer over (to free() when done); the Bytes is empty after.
    char* release() {
        char* p = data_;
        data_ = nullptr;
        len_ = cap_ = 0;
        return p;
    }

private:
    char* data_ = nullptr;
    size_t len_ = 0;
    size_t cap_ = 0;
};

// Room for `need` bytes in `b`, grown by half again at a time through the
// byte accountant, like growFor() for strings (docs/DESIGN.md section 15).
bool growFor(Bytes& b, size_t need);

}  // namespace tesser

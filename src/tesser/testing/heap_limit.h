// A heap with a size, for host tests: what operator new holds is counted,
// and an allocation past the limit aborts, as it does on ESP-IDF, where
// exceptions are off and a failed new is the end of the device. Only C++
// allocations count: malloc() fails softly there (it returns NULL), and the
// library checks it.
//
// Standalone (no TesserAPI include), so NowTP's and TesserKIT's tests can use
// it too. In exactly one translation unit of a test program:
//
//   #define TESSER_HEAP_LIMIT_IMPLEMENTATION
//   #include "tesser/testing/heap_limit.h"
//
// which replaces the global operator new and delete. Everywhere else, the
// header declares the controls:
//
//   tesser::testing::HeapLimit limit(4096);   // 4 KB more than is held now
//   ...                                       // an allocation past it aborts
//   { tesser::testing::HeapExempt client; ... }  // the test's own: not counted
//
// A test that wants to know rather than abort sets onExceeded(): the
// allocation then goes ahead, and the hook gets its size.
// Guards of its own rather than #pragma once: a file that included it
// already can include it again with the implementation macro.
#ifndef TESSER_TESTING_HEAP_LIMIT_H
#define TESSER_TESTING_HEAP_LIMIT_H

#include <stddef.h>

namespace tesser {
namespace testing {

// Bytes operator new holds now, and the most it has held since
// resetHeapPeak().
size_t heapUsed();
size_t heapPeak();
void resetHeapPeak();
// The most operator new may hold; 0, the default, for no limit.
void setHeapLimit(size_t bytes);
size_t heapLimit();
// What is left below the limit (SIZE_MAX without one): a free-heap probe
// for the code under test.
size_t heapLeft();
// With a floor, an allocation larger than `small` that leaves less than
// `floor` below the limit counts as past it too. Code that checks the free
// heap against a floor before a large allocation (TesserAPI's byte
// accountant) never does that, so this finds the large allocations made
// without a check, whatever the heap's size; small ones live on the floor.
// 0 (the default) for none.
void setHeapFloorRule(size_t floor, size_t small = 256);

// Called, instead of aborting, for an allocation of `bytes` past the
// limit, which then goes ahead; nullptr (the default) aborts. Runs inside
// operator new: it must not allocate through it (write to stderr with
// fprintf, keep counts).
using Exceeded = void (*)(size_t bytes, size_t used, size_t limit);
void onExceeded(Exceeded hook);

// While set on a thread, its allocations are neither counted nor limited:
// for what a test's own client or simulated link holds (a client is another
// machine; a link's buffers are malloc()ed on target, and the code under
// test accounts for them itself).
void setHeapExempt(bool exempt);
bool heapExempt();

class HeapExempt {
public:
    HeapExempt() : previous_(heapExempt()) { setHeapExempt(true); }
    ~HeapExempt() { setHeapExempt(previous_); }
    HeapExempt(const HeapExempt&) = delete;
    HeapExempt& operator=(const HeapExempt&) = delete;

private:
    bool previous_;
};

// A limit `above` what is held now, for its scope; the previous one after.
class HeapLimit {
public:
    explicit HeapLimit(size_t above) : previous_(heapLimit()) { setHeapLimit(heapUsed() + above); }
    ~HeapLimit() { setHeapLimit(previous_); }
    HeapLimit(const HeapLimit&) = delete;
    HeapLimit& operator=(const HeapLimit&) = delete;

private:
    size_t previous_;
};

}  // namespace testing
}  // namespace tesser

#endif  // TESSER_TESTING_HEAP_LIMIT_H

#if defined(TESSER_HEAP_LIMIT_IMPLEMENTATION) && !defined(TESSER_HEAP_LIMIT_IMPLEMENTED)
#define TESSER_HEAP_LIMIT_IMPLEMENTED

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <atomic>
#include <new>

namespace tesser {
namespace testing {
namespace heap_limit_detail {

// Each block carries its size in front, so an unsized delete knows it, and
// whether it was counted.
constexpr size_t kHeader = alignof(max_align_t) > 2 * sizeof(size_t) ? alignof(max_align_t) : 2 * sizeof(size_t);

inline std::atomic<size_t> g_used{0};
inline std::atomic<size_t> g_peak{0};
inline std::atomic<size_t> g_limit{0};
inline std::atomic<Exceeded> g_hook{nullptr};
inline thread_local bool g_exempt = false;
inline std::atomic<size_t> g_floor{0};
inline std::atomic<size_t> g_small{256};

inline void* allocate(size_t n, bool nothrow) {
    if (g_exempt) {
        void* p = malloc(kHeader + (n ? n : 1));
        if (!p) {
            if (nothrow) return nullptr;
            abort();
        }
        static_cast<size_t*>(p)[0] = 0;
        static_cast<size_t*>(p)[1] = 0;  // not counted
        return static_cast<char*>(p) + kHeader;
    }
    size_t limit = g_limit.load();
    size_t used = g_used.fetch_add(n) + n;
    size_t floor = g_floor.load();
    bool belowFloor = limit && floor && n > g_small.load() && (used > limit || limit - used < floor);
    if (limit && (used > limit || belowFloor)) {
        Exceeded hook = g_hook.load();
        if (!hook) {
            fprintf(stderr, "heap_limit: operator new(%zu) past the limit%s: %zu held of %zu\n", n,
                    used > limit ? "" : " (under the floor, unchecked)", used - n, limit);
            abort();  // as ESP-IDF does: no exceptions, no way back
        }
        hook(n, used - n, limit);
    }
    size_t peak = g_peak.load();
    while (used > peak && !g_peak.compare_exchange_weak(peak, used)) {
    }
    void* p = malloc(kHeader + (n ? n : 1));
    if (!p) {
        g_used.fetch_sub(n);
        if (nothrow) return nullptr;
        abort();
    }
    static_cast<size_t*>(p)[0] = n;
    static_cast<size_t*>(p)[1] = 1;  // counted
    return static_cast<char*>(p) + kHeader;
}

inline void release(void* p) {
    if (!p) return;
    char* block = static_cast<char*>(p) - kHeader;
    size_t* header = reinterpret_cast<size_t*>(block);
    if (header[1]) g_used.fetch_sub(header[0]);
    free(block);
}

}  // namespace heap_limit_detail

size_t heapUsed() { return heap_limit_detail::g_used.load(); }
size_t heapPeak() { return heap_limit_detail::g_peak.load(); }
void resetHeapPeak() { heap_limit_detail::g_peak = heap_limit_detail::g_used.load(); }
void setHeapLimit(size_t bytes) { heap_limit_detail::g_limit = bytes; }
size_t heapLimit() { return heap_limit_detail::g_limit.load(); }
size_t heapLeft() {
    size_t limit = heapLimit(), used = heapUsed();
    if (!limit) return SIZE_MAX;
    return used < limit ? limit - used : 0;
}
void onExceeded(Exceeded hook) { heap_limit_detail::g_hook = hook; }
void setHeapExempt(bool exempt) { heap_limit_detail::g_exempt = exempt; }
void setHeapFloorRule(size_t floor, size_t small) {
    heap_limit_detail::g_floor = floor;
    heap_limit_detail::g_small = small;
}
bool heapExempt() { return heap_limit_detail::g_exempt; }

}  // namespace testing
}  // namespace tesser

void* operator new(size_t n) { return tesser::testing::heap_limit_detail::allocate(n, false); }
void* operator new[](size_t n) { return tesser::testing::heap_limit_detail::allocate(n, false); }
void* operator new(size_t n, const std::nothrow_t&) noexcept {
    return tesser::testing::heap_limit_detail::allocate(n, true);
}
void* operator new[](size_t n, const std::nothrow_t&) noexcept {
    return tesser::testing::heap_limit_detail::allocate(n, true);
}
void operator delete(void* p) noexcept { tesser::testing::heap_limit_detail::release(p); }
void operator delete[](void* p) noexcept { tesser::testing::heap_limit_detail::release(p); }
void operator delete(void* p, size_t) noexcept { tesser::testing::heap_limit_detail::release(p); }
void operator delete[](void* p, size_t) noexcept { tesser::testing::heap_limit_detail::release(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { tesser::testing::heap_limit_detail::release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { tesser::testing::heap_limit_detail::release(p); }

#endif  // TESSER_HEAP_LIMIT_IMPLEMENTATION

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <utility>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/platform.h"

namespace tesser {

class ByteBudget;

// Bytes held from a ByteBudget for a buffer (docs/DESIGN.md section 15),
// given back when it is destroyed or released. Empty (false) when the
// budget refused it. Move-only; not thread-safe itself, though budgets are.
class Reservation {
public:
    Reservation() = default;
    Reservation(Reservation&& o) noexcept { *this = std::move(o); }
    Reservation& operator=(Reservation&& o) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation() { release(); }

    explicit operator bool() const { return budget_ != nullptr; }
    size_t bytes() const { return bytes_; }
    // The pool its bytes were found in: where to allocate them
    // (allocate(), Bytes::reserve(), jsonAllocator()).
    Pool pool() const { return pool_; }
    // Grows or shrinks to `bytes`: false, unchanged, if the budget can't
    // give the growth. An empty reservation can't grow.
    bool resize(size_t bytes);
    // The buffer is allocated, so the heap shows it: it stops counting as
    // heap about to be taken. Called right after allocating; until then,
    // and if it never is, the bytes count both ways (the safe side).
    void settle();
    void release();

private:
    friend class ByteBudget;
    ByteBudget* budget_ = nullptr;
    size_t bytes_ = 0;
    size_t unsettled_ = 0;  // reserved from the heap, not yet seen allocated
    uintptr_t key_ = 0;
    Pool pool_ = Pool::Internal;
};

// A byte accountant: buffers reserve their bytes before they are allocated
// and give them back when freed, so memory is bounded by bytes, not by
// message counts, and checks made from several tasks at once can't all pass
// (docs/DESIGN.md section 15). Budgets nest: a reservation counts against
// its budget and every one above it, up to the root, memoryBudget(), which
// also keeps the heap floor (setHeapFloor()) and the largest free block,
// each pool's (internal RAM, PSRAM) apart. Thread-safe.
class ByteBudget {
public:
    // `limit` bytes in all, `perKey` for any one key (a peer, say); 0: no
    // limit. Within `parent`'s (null: the root's). Lives as long as its
    // reservations.
    ByteBudget(const char* name, size_t limit, size_t perKey = 0, ByteBudget* parent = nullptr);
    ~ByteBudget() = default;
    ByteBudget(const ByteBudget&) = delete;
    ByteBudget& operator=(const ByteBudget&) = delete;

    // `bytes` for a buffer of `key`. With `heap`, `pool`'s free heap must
    // also hold them above its floor, besides what other reservations are
    // about to take from it, and `contiguous` ones (one allocation) must fit
    // in its largest free block with 1 KB to spare; buffers up to 256 bytes
    // always pass the heap check, so error replies always go. Pool::Large
    // without room in PSRAM is found in internal RAM, as malloc() would;
    // the reservation's pool() tells which. Without `heap`, the bytes are
    // only counted: memory a link already allocated and hands over. Empty if
    // refused (refused() counts it).
    Reservation reserve(size_t bytes, uintptr_t key = 0, bool contiguous = true, bool heap = true,
                        Pool pool = Pool::Internal);
    void setLimit(size_t limit, size_t perKey = 0);

    const char* name() const { return name_; }
    size_t limit() const;
    size_t perKey() const;
    size_t used() const;
    size_t peak() const;
    uint32_t refused() const;
    // Bytes held for `key`.
    size_t usedBy(uintptr_t key) const;

private:
    friend class Reservation;
    ByteBudget() = default;  // the root
    friend ByteBudget& memoryBudget();
    friend size_t heapRoom(bool contiguous, Pool pool);
    // In `pool` only, unless `fallback` (Pool::Large to internal RAM).
    Reservation reserveIn(size_t bytes, uintptr_t key, bool contiguous, bool heap, Pool pool, bool fallback);
    // Under the accountant's lock.
    bool fits(size_t bytes, uintptr_t key) const;
    void add(size_t bytes, uintptr_t key);
    void remove(size_t bytes, uintptr_t key);

    const char* name_ = "memory";
    ByteBudget* parent_ = nullptr;
    size_t limit_ = 0;
    size_t perKey_ = 0;
    size_t used_ = 0;
    size_t peak_ = 0;
    uint32_t refused_ = 0;
    std::vector<std::pair<uintptr_t, size_t>> keys_;
};

// The root budget: every library buffer that reserves counts here, as may
// the application's (no limit unless setLimit(); the heap floor always).
ByteBudget& memoryBudget();
// Heap about to be taken from `pool`: reserved, not yet settled.
size_t unsettledBytes(Pool pool = Pool::Internal);
// What a heap reservation in `pool` could take now: its free heap above
// its floor, less what other reservations are about to take from it, and
// within the root's limit; with `contiguous`, also one block with the
// headroom reserve() leaves. SIZE_MAX without a floor or a limit off
// target. For sizing a buffer to what there is, rather than refusing it
// (docs/DESIGN.md section 15); the bytes still need reserving.
size_t heapRoom(bool contiguous = false, Pool pool = Pool::Internal);

// For applications: bytes for a large allocation (a TLS session, a
// download, a backup) out of the same accounting, so they and the
// library's buffers can't both count on the same free heap. `contiguous`:
// one block (checked against the largest free block). `pool`: where the
// memory comes from; Pool::Large for memory that goes to PSRAM when the
// chip has it (mbedTLS's buffers with CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC,
// allocate(bytes, Pool::Large)), mallocPool(bytes) for a plain malloc().
// Keep the reservation while the memory is in use.
inline Reservation reserveBytes(size_t bytes, bool contiguous = false, Pool pool = Pool::Internal) {
    return memoryBudget().reserve(bytes, 0, contiguous, true, pool);
}

// An ArduinoJson allocator taking from `pool` (Pool::Large: PSRAM when the
// chip has it, else internal RAM), for documents that hold a whole tree or
// a backup: JsonDocument doc(jsonAllocator(Pool::Large)). Lives forever.
ArduinoJson::Allocator* jsonAllocator(Pool pool);

}  // namespace tesser

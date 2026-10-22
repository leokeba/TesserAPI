#include "tesser/budget.h"

#include "tesser/platform.h"

#include <stdlib.h>

namespace tesser {

namespace {

// One lock for every budget: a reservation checks and updates its whole
// chain, and the heap, at once. A leaf: nothing is called while it is held
// but the heap probes.
Mutex& accountant() {
    static Mutex m(kRankAccountant, "accountant");  // constructed on first use (budgets are often globals)
    return m;
}

size_t g_unsettled[2] = {0, 0};  // per pool (internal RAM, PSRAM), under accountant()

size_t& unsettledIn(Pool pool) { return g_unsettled[pool == Pool::Large ? 1 : 0]; }

constexpr size_t kSmall = 256;      // always passes the heap check
constexpr size_t kHeadroom = 1024;  // left in the largest block

// `pool` has `bytes` above its floor, after what other reservations are
// about to take from it: two tasks can't both count on the same bytes.
// Under accountant().
bool heapFits(size_t bytes, bool contiguous, Pool pool) {
    size_t floor = heapFloor(pool);
    size_t free = freeHeap(pool);
    bool ok = !floor || (free >= floor && free - floor >= unsettledIn(pool) + bytes);
    if (ok && contiguous && bytes > kHeadroom) ok = largestFreeBlock(pool) >= bytes + kHeadroom;
    return ok;
}

class PoolAllocator : public ArduinoJson::Allocator {
public:
    explicit PoolAllocator(Pool pool) : pool_(pool) {}
    void* allocate(size_t size) override { return tesser::allocate(size, pool_); }
    void deallocate(void* p) override { free(p); }
    void* reallocate(void* p, size_t size) override { return tesser::reallocate(p, size, pool_); }

private:
    Pool pool_;
};

}  // namespace

ArduinoJson::Allocator* jsonAllocator(Pool pool) {
    static PoolAllocator internal(Pool::Internal), large(Pool::Large);
    return pool == Pool::Large ? &large : &internal;
}

ByteBudget& memoryBudget() {
    static ByteBudget root;
    return root;
}

size_t unsettledBytes(Pool pool) {
    MutexGuard guard(accountant());
    return unsettledIn(poolOf(pool));
}

size_t heapRoom(bool contiguous, Pool pool) {
    pool = poolOf(pool);
    MutexGuard guard(accountant());
    ByteBudget& root = memoryBudget();
    size_t room = SIZE_MAX;
    if (root.limit_) room = root.used_ < root.limit_ ? root.limit_ - root.used_ : 0;
    size_t floor = heapFloor(pool);
    size_t free = freeHeap(pool);
    if (floor || free != SIZE_MAX) {
        size_t above = free > floor ? free - floor : 0;
        above = above > unsettledIn(pool) ? above - unsettledIn(pool) : 0;
        if (above < room) room = above;
    }
    if (contiguous) {
        size_t largest = largestFreeBlock(pool);
        size_t block = largest > kHeadroom ? largest - kHeadroom : 0;
        if (largest != SIZE_MAX && block < room) room = block;
    }
    return room;
}

ByteBudget::ByteBudget(const char* name, size_t limit, size_t perKey, ByteBudget* parent)
    : name_(name), parent_(parent ? parent : &memoryBudget()), limit_(limit), perKey_(perKey) {}

bool ByteBudget::fits(size_t bytes, uintptr_t key) const {
    for (const ByteBudget* b = this; b; b = b->parent_) {
        if (b->limit_ && b->used_ + bytes > b->limit_) return false;
        if (b->perKey_) {
            size_t mine = 0;
            for (const auto& k : b->keys_) {
                if (k.first == key) mine = k.second;
            }
            if (mine + bytes > b->perKey_) return false;
        }
    }
    return true;
}

void ByteBudget::add(size_t bytes, uintptr_t key) {
    for (ByteBudget* b = this; b; b = b->parent_) {
        b->used_ += bytes;
        if (b->used_ > b->peak_) b->peak_ = b->used_;
        if (!b->perKey_) continue;
        bool found = false;
        for (auto& k : b->keys_) {
            if (k.first != key) continue;
            k.second += bytes;
            found = true;
        }
        if (!found) b->keys_.emplace_back(key, bytes);
    }
}

void ByteBudget::remove(size_t bytes, uintptr_t key) {
    for (ByteBudget* b = this; b; b = b->parent_) {
        b->used_ -= bytes < b->used_ ? bytes : b->used_;
        for (size_t i = 0; i < b->keys_.size(); i++) {
            if (b->keys_[i].first != key) continue;
            auto& k = b->keys_[i];
            k.second -= bytes < k.second ? bytes : k.second;
            if (!k.second) b->keys_.erase(b->keys_.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
}

Reservation ByteBudget::reserve(size_t bytes, uintptr_t key, bool contiguous, bool heap, Pool pool) {
    return reserveIn(bytes, key, contiguous, heap, pool, true);
}

Reservation ByteBudget::reserveIn(size_t bytes, uintptr_t key, bool contiguous, bool heap, Pool pool,
                                  bool fallback) {
    Reservation r;
    pool = poolOf(pool);  // checked before the lock: it may ask the heap
    MutexGuard guard(accountant());
    bool ok = fits(bytes, key);
    if (ok && heap && bytes > kSmall) {
        // The floor left to everything else. PSRAM short, internal RAM, as
        // malloc() and allocate() fall back.
        ok = heapFits(bytes, contiguous, pool);
        if (!ok && fallback && pool == Pool::Large) {
            pool = Pool::Internal;
            ok = heapFits(bytes, contiguous, pool);
        }
    }
    if (!ok) {
        refused_++;
        return r;
    }
    add(bytes, key);
    r.budget_ = this;
    r.bytes_ = bytes;
    r.key_ = key;
    r.pool_ = pool;
    if (heap && bytes > kSmall) {
        r.unsettled_ = bytes;
        unsettledIn(pool) += bytes;
    }
    return r;
}

void ByteBudget::setLimit(size_t limit, size_t perKey) {
    MutexGuard guard(accountant());
    limit_ = limit;
    perKey_ = perKey;
}

size_t ByteBudget::limit() const {
    MutexGuard guard(accountant());
    return limit_;
}

size_t ByteBudget::perKey() const {
    MutexGuard guard(accountant());
    return perKey_;
}

size_t ByteBudget::used() const {
    MutexGuard guard(accountant());
    return used_;
}

size_t ByteBudget::peak() const {
    MutexGuard guard(accountant());
    return peak_;
}

uint32_t ByteBudget::refused() const {
    MutexGuard guard(accountant());
    return refused_;
}

size_t ByteBudget::usedBy(uintptr_t key) const {
    MutexGuard guard(accountant());
    for (const auto& k : keys_) {
        if (k.first == key) return k.second;
    }
    return 0;
}

// ---- Reservation -------------------------------------------------------------

Reservation& Reservation::operator=(Reservation&& o) noexcept {
    if (this != &o) {
        release();
        budget_ = o.budget_;
        bytes_ = o.bytes_;
        unsettled_ = o.unsettled_;
        key_ = o.key_;
        pool_ = o.pool_;
        o.budget_ = nullptr;
        o.bytes_ = o.unsettled_ = 0;
    }
    return *this;
}

bool Reservation::resize(size_t bytes) {
    if (!budget_) return false;
    if (bytes <= bytes_) {
        MutexGuard guard(accountant());
        size_t less = bytes_ - bytes;
        budget_->remove(less, key_);
        size_t drop = less < unsettled_ ? less : unsettled_;
        unsettled_ -= drop;
        unsettledIn(pool_) -= drop;
        bytes_ = bytes;
        return true;
    }
    // From the same pool: the buffer is there.
    Reservation more = budget_->reserveIn(bytes - bytes_, key_, false, true, pool_, false);
    if (!more) return false;
    MutexGuard guard(accountant());
    bytes_ += more.bytes_;
    unsettled_ += more.unsettled_;
    more.budget_ = nullptr;  // merged into this one
    more.bytes_ = more.unsettled_ = 0;
    return true;
}

void Reservation::settle() {
    if (!unsettled_) return;
    MutexGuard guard(accountant());
    unsettledIn(pool_) -= unsettled_;
    unsettled_ = 0;
}

void Reservation::release() {
    if (!budget_) return;
    MutexGuard guard(accountant());
    budget_->remove(bytes_, key_);
    unsettledIn(pool_) -= unsettled_;
    budget_ = nullptr;
    bytes_ = unsettled_ = 0;
}

}  // namespace tesser

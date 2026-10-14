#include "tesser/budget.h"

#include "tesser/platform.h"

namespace tesser {

namespace {

// One lock for every budget: a reservation checks and updates its whole
// chain, and the heap, at once. A leaf: nothing is called while it is held
// but the heap probes.
Mutex& accountant() {
    static Mutex m;  // constructed on first use (budgets are often globals)
    return m;
}

size_t g_unsettled = 0;  // under accountant()

constexpr size_t kSmall = 256;      // always passes the heap check
constexpr size_t kHeadroom = 1024;  // left in the largest block

}  // namespace

ByteBudget& memoryBudget() {
    static ByteBudget root;
    return root;
}

size_t unsettledBytes() {
    MutexGuard guard(accountant());
    return g_unsettled;
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

Reservation ByteBudget::reserve(size_t bytes, uintptr_t key, bool contiguous, bool heap) {
    Reservation r;
    MutexGuard guard(accountant());
    bool ok = fits(bytes, key);
    if (ok && heap && bytes > kSmall) {
        // The floor left to everything else, after what other reservations
        // are about to take: two tasks can't both count on the same bytes.
        size_t floor = heapFloor();
        size_t free = freeHeap();
        if (floor) ok = free >= floor && free - floor >= g_unsettled + bytes;
        if (ok && contiguous && bytes > kHeadroom) ok = largestFreeBlock() >= bytes + kHeadroom;
    }
    if (!ok) {
        refused_++;
        return r;
    }
    add(bytes, key);
    r.budget_ = this;
    r.bytes_ = bytes;
    r.key_ = key;
    if (heap && bytes > kSmall) {
        r.unsettled_ = bytes;
        g_unsettled += bytes;
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
        g_unsettled -= drop;
        bytes_ = bytes;
        return true;
    }
    Reservation more = budget_->reserve(bytes - bytes_, key_, false, true);
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
    g_unsettled -= unsettled_;
    unsettled_ = 0;
}

void Reservation::release() {
    if (!budget_) return;
    MutexGuard guard(accountant());
    budget_->remove(bytes_, key_);
    g_unsettled -= unsettled_;
    budget_ = nullptr;
    bytes_ = unsettled_ = 0;
}

}  // namespace tesser

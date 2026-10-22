#pragma once

// The only platform-specific pieces the core needs: a recursive mutex, a
// signal and logging.

#include <stdint.h>

#include <functional>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#else
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace tesser {

// Lock ranks (docs/DESIGN.md section 9): a task may take a ranked mutex
// only while every ranked mutex it holds has a lower rank. Unranked (0)
// mutexes aren't checked.
enum LockRank : uint8_t {
    kRankNone = 0,
    kRankSubscribers = 10,  // DatagramEndpoint's subscribers: before the API lock
    kRankApi = 20,          // Api: the tree
    kRankRemote = 30,       // a RemoteNode's copies: after the API lock
    kRankEndpoint = 80,     // DatagramEndpoint's state: its remote nodes' routes are taken inside
    kRankLeaf = 90,         // routes, streams, endpoint and link state, request queues: nothing taken inside
    kRankAccountant = 99,   // ByteBudget: taken inside leaves
};

// Host builds check lock ranks (TESSER_LOCK_CHECK; off with NDEBUG); a
// violation calls this, by default logging both locks and aborting.
using LockOrderHandler = void (*)(const char* held, const char* taking);
void setLockOrderHandler(LockOrderHandler fn);
#if !defined(ESP_PLATFORM) && !defined(NDEBUG) && !defined(TESSER_LOCK_CHECK)
#define TESSER_LOCK_CHECK 1
#endif

class Mutex {
public:
#if defined(ESP_PLATFORM)
    // Statically allocated, so safe to construct from global constructors.
    explicit Mutex(uint8_t rank = kRankNone, const char* name = nullptr)
        : handle_(xSemaphoreCreateRecursiveMutexStatic(&storage_)) {
        (void)rank;
        (void)name;
    }
    void lock() {
        xSemaphoreTakeRecursive(handle_, portMAX_DELAY);
        depth_++;
    }
    void unlock() {
        depth_--;
        xSemaphoreGiveRecursive(handle_);
    }
    bool tryLock() {
        if (xSemaphoreTakeRecursive(handle_, 0) != pdTRUE) return false;
        depth_++;
        return true;
    }
    bool heldByMe() const { return xSemaphoreGetMutexHolder(handle_) == xTaskGetCurrentTaskHandle(); }
#else
    explicit Mutex(uint8_t rank = kRankNone, const char* name = nullptr) : rank_(rank), name_(name) {}
    void lock() {
        if (!heldByMe()) checkOrder();
        mutex_.lock();
        locked();
    }
    void unlock() {
        unlocking();
        mutex_.unlock();
    }
    bool tryLock() {
        if (!mutex_.try_lock()) return false;
        locked();
        return true;
    }
    bool heldByMe() const { return owner_.load() == std::this_thread::get_id(); }
#endif
    // Times the calling task holds it (0 if another task, or none, does).
    int depth() const { return heldByMe() ? depth_ : 0; }

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

private:
#if defined(ESP_PLATFORM)
    StaticSemaphore_t storage_;
    SemaphoreHandle_t handle_;
#else
    void checkOrder();
    void locked();
    void unlocking();
    std::recursive_mutex mutex_;
    std::atomic<std::thread::id> owner_{};
    uint8_t rank_;
    const char* name_;
#endif
    int depth_ = 0;  // changed by the holder only
};

// Lets go of `m` while it lives, if the calling task holds it exactly once
// (else does nothing: an outer caller still counts on it): for work that
// must not run under the lock, such as sending a reply.
class MutexRelease {
public:
    explicit MutexRelease(Mutex& m) : m_(m), released_(m.depth() == 1) {
        if (released_) m_.unlock();
    }
    ~MutexRelease() {
        if (released_) m_.lock();
    }
    bool released() const { return released_; }
    MutexRelease(const MutexRelease&) = delete;
    MutexRelease& operator=(const MutexRelease&) = delete;

private:
    Mutex& m_;
    bool released_;
};

class MutexGuard {
public:
    explicit MutexGuard(Mutex& m) : m_(m) { m_.lock(); }
    ~MutexGuard() { m_.unlock(); }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;

private:
    Mutex& m_;
};

// A binary semaphore: one task waits for it, another gives it.
class Signal {
public:
#if defined(ESP_PLATFORM)
    Signal() : handle_(xSemaphoreCreateBinaryStatic(&storage_)) {}
    void give() { xSemaphoreGive(handle_); }
    // False if not given within `ms`.
    bool take(uint32_t ms) { return xSemaphoreTake(handle_, pdMS_TO_TICKS(ms)) == pdTRUE; }
#else
    Signal() = default;
    void give() {
        std::lock_guard<std::mutex> lock(mutex_);
        given_ = true;
        cv_.notify_one();
    }
    bool take(uint32_t ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(ms), [this] { return given_; })) return false;
        given_ = false;
        return true;
    }
#endif

    Signal(const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;

private:
#if defined(ESP_PLATFORM)
    StaticSemaphore_t storage_;
    SemaphoreHandle_t handle_;
#else
    std::mutex mutex_;
    std::condition_variable cv_;
    bool given_ = false;
#endif
};

// Runs `work` on the library's background task, without the API lock
// (docs/DESIGN.md section 9): slow work that an action defers (a key
// derivation at sign-in) and answers through its Pending, so the API stays
// free meanwhile. One task, created on first use, runs the jobs in turn;
// false if 8 are already waiting.
bool background(std::function<void()> work);
// Its stack, before the first background() (default 6144 bytes on target).
void setBackgroundStack(uint32_t bytes);

// Milliseconds since boot (wraps after ~49 days).
uint32_t millis32();

// Where a buffer's bytes come from (docs/DESIGN.md section 15). Internal
// RAM is what runs out: Wi-Fi, lwIP, DMA and task stacks need it. Large is
// PSRAM on a chip that has it (found at run time), else internal RAM too.
enum class Pool : uint8_t { Internal, Large };
// The chip has PSRAM in its heap, so Pool::Large is a pool of its own
// (false off target).
bool hasPsram();
// Replaces it (tests: a chip with PSRAM, or without, whatever this one
// has); null restores the platform's.
void setPsramProbe(bool (*probe)());
// The pool `pool` draws from: Pool::Large only with PSRAM.
Pool poolOf(Pool pool);
// Where malloc() puts `bytes` (std::string, std::vector): in PSRAM, with
// CONFIG_SPIRAM_USE_MALLOC, above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL;
// else internal RAM. Reservations for such buffers ask this pool.
Pool mallocPool(size_t bytes);
// The size above which malloc() takes PSRAM first, for an application that
// changed it with heap_caps_malloc_extmem_enable(), and for tests; SIZE_MAX:
// never.
void setMallocPsramAbove(size_t bytes);
size_t mallocPsramAbove();
// `bytes` from `pool` (Pool::Large: PSRAM, else internal RAM), and grown
// there; free() frees them. Null if the heap can't.
void* allocate(size_t bytes, Pool pool);
void* reallocate(void* p, size_t bytes, Pool pool);

// The largest block the heap can allocate now in `pool` (bytes; SIZE_MAX
// off target). Large buffers check it first, so a fragmented heap refuses
// a reply instead of aborting the device (docs/DESIGN.md section 15).
size_t largestFreeBlock(Pool pool = Pool::Internal);
// Replaces the probe (tests); null restores the platform's.
void setLargestFreeBlockProbe(size_t (*probe)(), Pool pool = Pool::Internal);
// The heap free in all in `pool` (bytes; SIZE_MAX off target), and its
// probe.
size_t freeHeap(Pool pool = Pool::Internal);
void setFreeHeapProbe(size_t (*probe)(), Pool pool = Pool::Internal);
// What guarded buffers leave free (docs/DESIGN.md section 15): growing one
// past 256 bytes must leave this much of the heap, or replies answer busy
// and notifications wait, before the heap is so short that some small
// allocation elsewhere aborts. Forwarded subscriptions needing a new copy
// of a remote's state want twice as much. Internal RAM: 8 KB on target by
// default; PSRAM's own: 16 KB; 0 (off) elsewhere. Any task.
void setHeapFloor(size_t bytes, Pool pool = Pool::Internal);
size_t heapFloor(Pool pool = Pool::Internal);
// Free heap in `pool` is at least `bytes` above its floor (always without
// a floor).
bool heapAbove(size_t bytes, Pool pool = Pool::Internal);

// The chip's Wi-Fi station MAC, the device id UDP advertises and NowTP
// addresses (false off target).
bool stationMac(uint8_t mac[6]);

void logWarning(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace tesser

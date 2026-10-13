#pragma once

// The only platform-specific pieces the core needs: a recursive mutex, a
// signal and logging.

#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <chrono>
#include <condition_variable>
#include <mutex>
#endif

namespace tesser {

class Mutex {
public:
#if defined(ESP_PLATFORM)
    // Statically allocated, so safe to construct from global constructors.
    Mutex() : handle_(xSemaphoreCreateRecursiveMutexStatic(&storage_)) {}
    void lock() { xSemaphoreTakeRecursive(handle_, portMAX_DELAY); }
    void unlock() { xSemaphoreGiveRecursive(handle_); }
    bool tryLock() { return xSemaphoreTakeRecursive(handle_, 0) == pdTRUE; }
#else
    Mutex() = default;
    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }
    bool tryLock() { return mutex_.try_lock(); }
#endif

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

private:
#if defined(ESP_PLATFORM)
    StaticSemaphore_t storage_;
    SemaphoreHandle_t handle_;
#else
    std::recursive_mutex mutex_;
#endif
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

// Milliseconds since boot (wraps after ~49 days).
uint32_t millis32();

// The chip's Wi-Fi station MAC, the device id UDP advertises and NowTP
// addresses (false off target).
bool stationMac(uint8_t mac[6]);

void logWarning(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace tesser

#pragma once

// The only platform-specific pieces the core needs: a recursive mutex and
// logging.

#include <stdint.h>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
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
#else
    Mutex() = default;
    void lock() { mutex_.lock(); }
    void unlock() { mutex_.unlock(); }
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

// Milliseconds since boot (wraps after ~49 days).
uint32_t millis32();

void logWarning(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void logError(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace tesser

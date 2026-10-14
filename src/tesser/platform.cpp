#include "tesser/platform.h"

#include "tesser/budget.h"
#include "tesser/sink.h"

#include <stdarg.h>
#include <stdlib.h>

#include <atomic>
#include <vector>
#include <stdint.h>
#include <stdio.h>

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "tesser/api.h"
#else
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace tesser {

namespace {

void logv(char level, const char* fmt, va_list ap) {
#if defined(ESP_PLATFORM)
    char buf[160];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    if (level == 'E') {
        ESP_LOGE("tesser", "%s", buf);
    } else {
        ESP_LOGW("tesser", "%s", buf);
    }
#else
    fprintf(stderr, "%c tesser: ", level);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
}

}  // namespace

namespace {
size_t (*g_probe)() = nullptr;
size_t (*g_freeProbe)() = nullptr;
#if defined(ESP_PLATFORM)
std::atomic<size_t> g_floor{8192};
#else
std::atomic<size_t> g_floor{0};
#endif
}  // namespace

void setLargestFreeBlockProbe(size_t (*probe)()) { g_probe = probe; }
void setFreeHeapProbe(size_t (*probe)()) { g_freeProbe = probe; }
void setHeapFloor(size_t bytes) { g_floor = bytes; }
size_t heapFloor() { return g_floor.load(); }

size_t freeHeap() {
    if (g_freeProbe) return g_freeProbe();
#if defined(ESP_PLATFORM)
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
#else
    return SIZE_MAX;
#endif
}

bool heapAbove(size_t bytes) {
    size_t floor = heapFloor();
    if (!floor) return true;
    size_t free = freeHeap();
    return free >= floor && free - floor >= bytes;
}

namespace {
void defaultLockOrder(const char* held, const char* taking) {
    logError("lock order: taking %s while holding %s", taking ? taking : "?", held ? held : "?");
#if !defined(ESP_PLATFORM)
    abort();
#endif
}
std::atomic<LockOrderHandler> g_lockOrder{&defaultLockOrder};
}  // namespace

void setLockOrderHandler(LockOrderHandler fn) { g_lockOrder = fn ? fn : &defaultLockOrder; }

#if !defined(ESP_PLATFORM)
namespace {
// The ranked mutexes this thread holds, innermost last.
thread_local std::vector<std::pair<uint8_t, const char*>> t_held;
}  // namespace

void Mutex::checkOrder() {
#if defined(TESSER_LOCK_CHECK)
    if (!rank_) return;
    for (const auto& h : t_held) {
        if (h.first >= rank_) g_lockOrder.load()(h.second, name_);
    }
#endif
}

void Mutex::locked() {
    if (heldByMe()) {
        depth_++;
        return;
    }
    owner_ = std::this_thread::get_id();
    depth_ = 1;
#if defined(TESSER_LOCK_CHECK)
    if (rank_) t_held.emplace_back(rank_, name_);
#endif
}

void Mutex::unlocking() {
    if (--depth_ > 0) return;
    owner_ = std::thread::id();
#if defined(TESSER_LOCK_CHECK)
    if (!rank_) return;
    for (size_t i = t_held.size(); i-- > 0;) {
        if (t_held[i].second == name_ && t_held[i].first == rank_) {
            t_held.erase(t_held.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
#endif
}
#endif

// ---- background task ------------------------------------------------------------

namespace {
constexpr size_t kBackgroundJobs = 8;
std::atomic<uint32_t> g_backgroundStack{6144};

#if defined(ESP_PLATFORM)
struct Background {
    Mutex mutex{kRankLeaf, "background"};
    std::vector<std::function<void()>> jobs;
    TaskHandle_t task = nullptr;
};

void backgroundLoop(void* arg) {
    auto* b = static_cast<Background*>(arg);
    for (;;) {
        std::function<void()> job;
        {
            MutexGuard guard(b->mutex);
            if (!b->jobs.empty()) {
                job = std::move(b->jobs.front());
                b->jobs.erase(b->jobs.begin());
            }
        }
        if (job) {
            job();
        } else {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}
#else
struct Background {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::function<void()>> jobs;
    bool started = false;
};

void backgroundLoop(Background* b) {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(b->mutex);
            b->cv.wait(lock, [b] { return !b->jobs.empty(); });
            job = std::move(b->jobs.front());
            b->jobs.erase(b->jobs.begin());
        }
        if (job) job();
    }
}
#endif

Background& backgroundState() {
    static Background* b = new Background();  // never destroyed: its task runs on
    return *b;
}
}  // namespace

void setBackgroundStack(uint32_t bytes) { g_backgroundStack = bytes; }

bool background(std::function<void()> work) {
    if (!work) return false;
    Background& b = backgroundState();
#if defined(ESP_PLATFORM)
    {
        MutexGuard guard(b.mutex);
        if (b.jobs.size() >= kBackgroundJobs) return false;
        if (!b.task && xTaskCreate(&backgroundLoop, "tesser_bg", g_backgroundStack.load(), &b, 4, &b.task) != pdPASS) {
            b.task = nullptr;
            return false;
        }
        b.jobs.push_back(std::move(work));
    }
    xTaskNotifyGive(b.task);
#else
    {
        std::lock_guard<std::mutex> lock(b.mutex);
        if (b.jobs.size() >= kBackgroundJobs) return false;
        if (!b.started) {
            std::thread(&backgroundLoop, &b).detach();
            b.started = true;
        }
        b.jobs.push_back(std::move(work));
    }
    b.cv.notify_one();
#endif
    return true;
}

size_t largestFreeBlock() {
    if (g_probe) return g_probe();
#if defined(ESP_PLATFORM)
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
    return SIZE_MAX;
#endif
}

bool growFor(std::string& s, size_t need) {
    if (need <= s.capacity()) return true;
    size_t grown = s.capacity() + s.capacity() / 2;
    size_t capacity = need > grown ? need : grown;
    // Small buffers (error replies) always go: refusing them would leave the
    // client with nothing at all.
    Reservation growth;
    if (capacity > 256) {
        // The heap as a whole: the growth and a copy further down, and the
        // floor left to everything else, after what other tasks' buffers
        // are about to take (the accountant, docs/DESIGN.md section 15).
        growth = memoryBudget().reserve(capacity - s.capacity() + need, 0, false);
        if (!growth) return false;
        // One block that size, a copy further down, and headroom.
        if (capacity > 1024 && largestFreeBlock() < capacity + need + 1024) return false;
    }
    s.reserve(capacity);
    return true;  // released: the heap shows the buffer now
}

bool stationMac(uint8_t mac[6]) {
#if defined(ESP_PLATFORM)
    return esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK;
#else
    (void)mac;
    return false;
#endif
}

uint32_t millis32() {
#if defined(ESP_PLATFORM)
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
#else
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
#endif
}

#if defined(ESP_PLATFORM)
namespace {
struct PollTask {
    Api* api;
    uint32_t periodMs;
};

void pollTask(void* arg) {
    auto* t = static_cast<PollTask*>(arg);
    for (;;) {
        t->api->poll();
        vTaskDelay(pdMS_TO_TICKS(t->periodMs) ? pdMS_TO_TICKS(t->periodMs) : 1);
    }
}
}  // namespace

bool Api::startTask(uint32_t periodMs, uint32_t stackSize, unsigned priority) {
    auto* t = new PollTask{this, periodMs};
    if (xTaskCreate(&pollTask, "tesser_poll", stackSize, t, priority, nullptr) != pdPASS) {
        delete t;
        return false;
    }
    return true;
}
#endif

void logWarning(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv('W', fmt, ap);
    va_end(ap);
}

void logError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    logv('E', fmt, ap);
    va_end(ap);
}

}  // namespace tesser

#include "tesser/platform.h"

#include "tesser/budget.h"
#include "tesser/bytes.h"
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
#include "sdkconfig.h"
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
// Per pool: [0] internal RAM, [1] PSRAM.
size_t (*g_probe[2])() = {nullptr, nullptr};
size_t (*g_freeProbe[2])() = {nullptr, nullptr};
bool (*g_psramProbe)() = nullptr;
#if defined(ESP_PLATFORM)
std::atomic<size_t> g_floor[2] = {{8192}, {16384}};
#else
std::atomic<size_t> g_floor[2] = {{0}, {0}};
#endif
#if defined(ESP_PLATFORM) && defined(CONFIG_SPIRAM_USE_MALLOC)
std::atomic<size_t> g_mallocPsramAbove{CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL};
#else
std::atomic<size_t> g_mallocPsramAbove{SIZE_MAX};
#endif

size_t slot(Pool pool) { return pool == Pool::Large ? 1 : 0; }

#if defined(ESP_PLATFORM)
uint32_t capsOf(Pool pool) {
    return pool == Pool::Large ? MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT : MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
}
#endif
}  // namespace

void setPsramProbe(bool (*probe)()) { g_psramProbe = probe; }

bool hasPsram() {
    if (g_psramProbe) return g_psramProbe();
#if defined(ESP_PLATFORM)
    // Found at run time: an image built with CONFIG_SPIRAM_IGNORE_NOTFOUND
    // runs without it.
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
    return false;
#endif
}

Pool poolOf(Pool pool) { return pool == Pool::Large && hasPsram() ? Pool::Large : Pool::Internal; }

Pool mallocPool(size_t bytes) { return bytes > g_mallocPsramAbove.load() ? poolOf(Pool::Large) : Pool::Internal; }
void setMallocPsramAbove(size_t bytes) { g_mallocPsramAbove = bytes; }
size_t mallocPsramAbove() { return g_mallocPsramAbove.load(); }

void* allocate(size_t bytes, Pool pool) {
#if defined(ESP_PLATFORM)
    // Without PSRAM (or with it full), from internal RAM, as malloc() would.
    if (pool == Pool::Large) {
        if (void* p = heap_caps_malloc(bytes, capsOf(Pool::Large))) return p;
    }
    return heap_caps_malloc(bytes, capsOf(Pool::Internal));
#else
    (void)pool;
    return malloc(bytes);
#endif
}

void* reallocate(void* p, size_t bytes, Pool pool) {
#if defined(ESP_PLATFORM)
    if (!p) return allocate(bytes, pool);
    // Moved between pools if it isn't in the one asked for.
    if (pool == Pool::Large) {
        if (void* q = heap_caps_realloc(p, bytes, capsOf(Pool::Large))) return q;
    }
    return heap_caps_realloc(p, bytes, capsOf(Pool::Internal));
#else
    (void)pool;
    return realloc(p, bytes);
#endif
}

void setLargestFreeBlockProbe(size_t (*probe)(), Pool pool) { g_probe[slot(pool)] = probe; }
void setFreeHeapProbe(size_t (*probe)(), Pool pool) { g_freeProbe[slot(pool)] = probe; }
void setHeapFloor(size_t bytes, Pool pool) { g_floor[slot(pool)] = bytes; }
size_t heapFloor(Pool pool) { return g_floor[slot(poolOf(pool))].load(); }

size_t freeHeap(Pool pool) {
    pool = poolOf(pool);
    if (size_t (*probe)() = g_freeProbe[slot(pool)]) return probe();
#if defined(ESP_PLATFORM)
    return heap_caps_get_free_size(capsOf(pool));
#else
    return SIZE_MAX;
#endif
}

bool heapAbove(size_t bytes, Pool pool) {
    size_t floor = heapFloor(pool);
    if (!floor) return true;
    size_t free = freeHeap(pool);
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

// How long the task waits for another job before it exits, giving its stack
// back: sign-ins come in bursts, far apart.
constexpr uint32_t kBackgroundIdleMs = 5000;

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
        } else if (!ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kBackgroundIdleMs))) {
            // A job queued meanwhile was queued for this task, under the
            // mutex: it stays. Otherwise the next background() starts another.
            MutexGuard guard(b->mutex);
            if (b->jobs.empty()) {
                b->task = nullptr;
                break;
            }
        }
    }
    vTaskDelete(nullptr);
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
    TaskHandle_t task;
    {
        MutexGuard guard(b.mutex);
        if (b.jobs.size() >= kBackgroundJobs) return false;
        if (!b.task && xTaskCreate(&backgroundLoop, "tesser_bg", g_backgroundStack.load(), &b, 4, &b.task) != pdPASS) {
            b.task = nullptr;
            return false;
        }
        b.jobs.push_back(std::move(work));
        task = b.task;  // can't exit before it has run the job
    }
    xTaskNotifyGive(task);
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

size_t largestFreeBlock(Pool pool) {
    pool = poolOf(pool);
    if (size_t (*probe)() = g_probe[slot(pool)]) return probe();
#if defined(ESP_PLATFORM)
    return heap_caps_get_largest_free_block(capsOf(pool));
#else
    return SIZE_MAX;
#endif
}

// What a small buffer's growth leaves of the heap.
constexpr size_t kSmallGrowthSlack = 1024;

// A buffer grows by half again at a time; short of room for that, by just
// what it needs (a reply nearly its full size: measured on DEVKIT-R1, a 4 KB
// download chunk's reply was refused for growing to 5.9 KB). The growth stays
// reserved until the buffer has it.
void reserveExactly(std::string& s, size_t capacity) {
    if (capacity <= s.capacity()) return;
    // Copied into a new string, reserved from its small buffer: the same
    // peak as reserve() (old and new buffers), without libstdc++'s doubling
    // of an allocated capacity, empty or not.
    std::string grown;
    grown.reserve(capacity);
    grown.append(s);
    s.swap(grown);
}

bool growFor(std::string& s, size_t need) {
    if (need <= s.capacity()) return true;
    size_t grown = s.capacity() + s.capacity() / 2;
    Reservation held;
    auto fits = [&](size_t capacity) {
        // Small buffers (error replies) go without the floor: refusing them
        // would leave the client with nothing at all. Not on a heap that
        // can't even hold them, though (taken by the network stack in a
        // burst, say), where the allocation would abort the device: the free
        // heap there, a counter, as the largest block costs a walk of the
        // heap.
        if (capacity <= 256) return freeHeap(mallocPool(capacity)) >= capacity + kSmallGrowthSlack;
        // The heap as a whole: the growth and a copy further down, and the
        // floor left to everything else, after what other tasks' buffers
        // are about to take (the accountant, docs/DESIGN.md section 15).
        // In the pool malloc() takes it from.
        held = Reservation();
        held = memoryBudget().reserve(capacity - s.capacity() + need, 0, false, true, mallocPool(capacity));
        // One block that size, a copy further down, and headroom.
        return held && (capacity <= 1024 || largestFreeBlock(held.pool()) >= capacity + need + 1024);
    };
    size_t capacity = grown > need && fits(grown) ? grown : fits(need) ? need : 0;
    if (!capacity) return false;
    reserveExactly(s, capacity);
    return true;  // released: the heap shows the buffer now
}

bool growFor(Bytes& b, size_t need) {
    if (need <= b.capacity()) return true;
    size_t grown = b.capacity() + b.capacity() / 2;
    Reservation held;
    auto fits = [&](size_t capacity) {
        if (capacity <= 256) return true;
        // realloc() may need the new block while the old one is held. In
        // PSRAM when the chip has it, unless that is short.
        held = Reservation();
        held = memoryBudget().reserve(capacity, 0, false, true, Pool::Large);
        return held && (capacity <= 1024 || largestFreeBlock(held.pool()) >= capacity + 1024);
    };
    size_t capacity = grown > need && fits(grown) ? grown : fits(need) ? need : 0;
    return capacity && b.reserve(capacity, held ? held.pool() : Pool::Large);
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

#include "tesser/platform.h"

#include "tesser/sink.h"

#include <stdarg.h>
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
}

void setLargestFreeBlockProbe(size_t (*probe)()) { g_probe = probe; }

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
    if (capacity > 1024 && largestFreeBlock() < capacity + need + 1024) return false;
    s.reserve(capacity);
    return true;
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

#include "tesser/platform.h"

#include <stdarg.h>
#include <stdio.h>

#if defined(ESP_PLATFORM)
#include "esp_log.h"
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

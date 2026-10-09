// On-target test for TesserAPI. Needs a single board.
//
// 1. Runs the core test cases (shared with the host tests) on the chip:
//    real compiler, no exceptions, no RTTI, real heap.
// 2. Measures heap per node, request latency and heap stability.
// 3. Serves a demo API over UART0 (test/e2e_serial.py), HTTP when
//    test/secrets.h provides Wi-Fi credentials (test/e2e_http.py), and NowTP
//    (test/e2e_nowtp.py, two boards).
//
// Results are printed as "PASS <name>" / "FAIL <name>: ..." lines followed by
// "DONE pass=<n> fail=<n>", then "MEM ..." / "PERF ..." lines and "SERVING".
#include <stdio.h>

#include "core_cases.h"
#include "datagram_cases.h"
#include "subscription_cases.h"
#include "persistence_cases.h"
#include "list_cases.h"
#include "remote_cases.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void startNetwork(tesser::Api& api);

namespace {

size_t freeHeap() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

constexpr int kNodes = 100;
char g_names[kNodes][8];
int g_ints[kNodes];

void measureNodes() {
    for (int i = 0; i < kNodes; i++) snprintf(g_names[i], sizeof(g_names[i]), "n%d", i);

    size_t h0 = freeHeap();
    auto* api = new tesser::Api();
    size_t h1 = freeHeap();
    for (int i = 0; i < kNodes; i++) api->value(g_names[i], g_ints[i]);
    size_t h2 = freeHeap();
    delete api;

    api = new tesser::Api();
    size_t h3 = freeHeap();
    for (int i = 0; i < kNodes; i++) {
        int* p = &g_ints[i];
        api->value(
            g_names[i], [p] { return *p; }, [p](int v) { *p = v; });
    }
    size_t h4 = freeHeap();
    delete api;

    api = new tesser::Api();
    size_t h5 = freeHeap();
    for (int i = 0; i < kNodes; i++) api->object(g_names[i]);
    size_t h6 = freeHeap();
    delete api;

    api = new tesser::Api();
    size_t h7 = freeHeap();
    for (int i = 0; i < kNodes; i++) api->value(g_names[i], g_ints[i]).range(0, 100);
    size_t h8 = freeHeap();
    delete api;
    size_t h9 = freeHeap();

    printf("MEM api=%u bytes\n", unsigned(h0 - h1));
    printf("MEM value_ref=%u bytes/node\n", unsigned((h1 - h2) / kNodes));
    printf("MEM value_fn=%u bytes/node\n", unsigned((h3 - h4) / kNodes));
    printf("MEM object=%u bytes/node\n", unsigned((h5 - h6) / kNodes));
    printf("MEM value_ref_range=%u bytes/node\n", unsigned((h7 - h8) / kNodes));
    printf("MEM leak_after_delete=%d bytes\n", int(h0) - int(h9));
}

void measureLatency() {
    cases::Device d;
    tesser::Request req;
    req.path = "/";
    const int n = 200;
    tesser::StringReply reply;
    size_t heapBefore = freeHeap();
    int64_t t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) d.api.handle(req, reply);
    int64_t t1 = esp_timer_get_time();
    printf("PERF get_root=%lld us (%u bytes)\n", (long long)((t1 - t0) / n), unsigned(reply.body.size()));

    JsonDocument doc;
    deserializeJson(doc, "{\"lamp\":{\"on\":true,\"brightness\":10},\"config\":{\"level\":7}}");
    req.op = tesser::Op::Set;
    req.body = doc.as<JsonVariantConst>();
    t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) d.api.handle(req, reply);
    t1 = esp_timer_get_time();
    printf("PERF patch=%lld us\n", (long long)((t1 - t0) / n));

    tesser::LineTransport lines(d.api, [](const char*, size_t) {});
    const char* line = "{\"id\":1,\"op\":\"get\",\"path\":\"/lamp\",\"keys\":\"on,brightness\"}\n";
    size_t len = strlen(line);
    t0 = esp_timer_get_time();
    for (int i = 0; i < n; i++) lines.feed(line, len);
    t1 = esp_timer_get_time();
    printf("PERF envelope_get=%lld us\n", (long long)((t1 - t0) / n));
    // A long run must not leak: request handling allocates nothing that survives it.
    printf("MEM heap_drift_after_requests=%d bytes\n", int(heapBefore) - int(freeHeap()));
}

// ---- demo API served over UART0 -------------------------------------------

cases::Device* g_demo;
bool g_running = false;
uint32_t g_counter = 0;
uint32_t g_ticks = 0;
tesser::ValueNode* g_ticksNode;
tesser::EventNode* g_fired;
char g_settingName[24] = "unnamed";
uint32_t g_boots = 0;
int g_secret = 0;
tesser::Node* g_secureNode;
tesser::Node* g_secretNode;
tesser::NvsStorage* g_storage;
tesser::UartTransport* g_uart;

void delayedReply(void* arg) {
    auto* pending = static_cast<tesser::Pending*>(arg);
    vTaskDelay(pdMS_TO_TICKS(200));
    pending->reply("done");
    delete pending;
    vTaskDelete(nullptr);
}

void serveDemo() {
    g_demo = new cases::Device();
    auto& sys = g_demo->api.object("system");
    sys.value("heap", [] { return uint32_t(freeHeap()); });
    sys.value("minHeap", [] { return uint32_t(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)); });
    sys.value("uptimeMs", [] { return int64_t(esp_timer_get_time() / 1000); });
    sys.value("idf", esp_get_idf_version());
    // Runs in the UART task: reports that task's unused stack.
    sys.action("stackFree", [] { return uint32_t(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)); });
    sys.action("later", [](tesser::Call& call) {
        tesser::Pending p = call.defer();
        if (!p) return;
        xTaskCreate(delayedReply, "later", 3072, new tesser::Pending(p), 5, nullptr);
    });
    sys.value("subscriptions", [] { return uint32_t(g_demo->api.subscriptionCount()); });

    // Change sources for the subscription tests: a counter only watch() notices,
    // a value reported with changed(), and an event.
    auto& demo = g_demo->api.object("demo");
    demo.value("running", g_running);
    demo.value("counter", g_counter).watch();
    g_ticksNode = &demo.value("ticks", g_ticks);
    g_fired = &demo.event("fired");
    demo.action("fire", [](JsonVariantConst payload) { g_fired->emit(payload); });
    esp_timer_create_args_t timer = {};
    timer.callback = [](void*) {
        if (!g_running) return;
        g_counter++;
        if (g_counter % 10 == 0) {
            g_ticks++;
            g_ticksNode->changed();
        }
    };
    timer.name = "demo";
    esp_timer_handle_t handle;
    esp_timer_create(&timer, &handle);
    esp_timer_start_periodic(handle, 100 * 1000);
    g_demo->api.startTask(20);

    // Persistence: /settings survives reboots (NVS, saved 500 ms after a change).
    auto& settings = g_demo->api.object("settings").persist();
    settings.value("name", g_settingName);
    settings.value("boots", g_boots).readOnly();
    // Access control: writes under /secure need an authenticated client
    // (HTTP/WebSocket token "test-token", serial; no NowTP peer is trusted).
    auto& secure = g_demo->api.object("secure");
    g_secureNode = &secure;
    g_secretNode = &secure.value("secret", g_secret);
    g_demo->api.authorize([](const tesser::Client& c, tesser::Op op, const tesser::Node& n) {
        if (op == tesser::Op::Get || op == tesser::Op::Subscribe || c.authenticated) return true;
        return &n != g_secureNode && &n != g_secretNode;
    });

    startNetwork(g_demo->api);  // NVS is initialized there
    g_storage = new tesser::NvsStorage("tesser_test");
    g_demo->api.persistence(*g_storage, 500);
    bool loaded = g_demo->api.load();
    g_boots++;
    g_demo->api.save();
    printf("PERSIST loaded=%d boots=%u name=%s\n", int(loaded), unsigned(g_boots), g_settingName);
    g_uart = new tesser::UartTransport(g_demo->api);
    esp_err_t err = g_uart->begin();
    // UART0 is also the console: keep logs from interleaving with replies.
    esp_log_level_set("*", ESP_LOG_WARN);
    printf("SERVING err=%d\n", int(err));
}

}  // namespace

extern "C" void app_main() {
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("TESSER hardware test: model=%d cores=%d idf=%s heap=%u\n", int(chip.model), chip.cores,
           esp_get_idf_version(), unsigned(freeHeap()));

    size_t heap0 = freeHeap();
    check::run(nullptr);
    printf("MEM heap_drift_after_cases=%d bytes\n", int(heap0) - int(freeHeap()));
    measureNodes();
    measureLatency();
    printf("MEM main_stack_free=%u bytes\n", unsigned(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));
    serveDemo();
}

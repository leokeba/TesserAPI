// A small device API over HTTP with ESP-IDF. Set the Wi-Fi network with
// `idf.py menuconfig` (Example Connection Configuration), then:
//   curl http://<ip>/api/
//   curl "http://<ip>/api/?view=schema"
//   curl -X PUT -d 200 http://<ip>/api/led/brightness
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"

#if !defined(FOOTPRINT_BASELINE)
#include "TesserAPI.h"

namespace {

struct Led {
    bool on = false;
    int brightness = 128;
} led;

char deviceName[32] = "led-01";

tesser::Api api;
tesser::HttpServer http(api);

}  // namespace

static void startApi() {
    auto& l = api.object("led");
    l.value("on", led.on);
    l.value("brightness", led.brightness).range(0, 255).doc("PWM duty");
    l.action("toggle", [] { led.on = !led.on; });

    auto& sys = api.object("system");
    sys.value("name", deviceName);
    sys.value("uptimeMs", [] { return esp_timer_get_time() / 1000; });
    sys.value("freeHeap", [] { return esp_get_free_heap_size(); });
    sys.action("restart", [] { esp_restart(); });

    http.enableCors();
    ESP_ERROR_CHECK(http.begin(80, "/api"));
}

#else
// Baseline for footprint measurements: the same server with one plain handler.
#include "esp_http_server.h"

static esp_err_t hello(httpd_req_t* req) { return httpd_resp_sendstr(req, "{}"); }

static void startApi() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t server = nullptr;
    ESP_ERROR_CHECK(httpd_start(&server, &config));
    httpd_uri_t uri = {};
    uri.uri = "/api*";
    uri.method = HTTP_GET;
    uri.handler = hello;
    httpd_register_uri_handler(server, &uri);
}
#endif

extern "C" void app_main() {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());
    startApi();
}

// A small device API over HTTP with ESP-IDF, driving an LED with PWM. Set the
// Wi-Fi network (Example Connection Configuration) and the LED's GPIO
// (Example Configuration, default 2: the onboard LED of most ESP32 devkits)
// with `idf.py menuconfig`, then:
//   curl http://<ip>/api/
//   curl "http://<ip>/api/?view=schema"
//   curl -X PUT -d true http://<ip>/api/led/on
//   curl -X PUT -d 20 http://<ip>/api/led/brightness
#include "driver/ledc.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

// 8-bit PWM on the LED's pin, off at first. Both builds have it, so the
// footprint comparison measures TesserAPI alone.
static void initLed() {
    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_8_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel = {};
    channel.gpio_num = CONFIG_EXAMPLE_LED_GPIO;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

[[maybe_unused]] static void setLedDuty(int duty) {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, static_cast<uint32_t>(duty));
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

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

void applyLed() { setLedDuty(led.on ? led.brightness : 0); }

}  // namespace

static void startApi() {
    // Setters, so that every write reaches the pin.
    auto& l = api.object("led");
    l.value("on", [] { return led.on; }, [](bool on) {
        led.on = on;
        applyLed();
    });
    l.value("brightness", [] { return led.brightness; }, [](int brightness) {
        led.brightness = brightness;
        applyLed();
    }).range(0, 255).doc("PWM duty");
    l.action("toggle", [] {
        led.on = !led.on;
        applyLed();
    });

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
    initLed();
    ESP_ERROR_CHECK(example_connect());
    startApi();
}

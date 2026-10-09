// Wi-Fi, HTTP and NowTP for the on-target test firmware.
//
// Exposes under /net: ip, mac, channel, discovered peers, NowTP endpoint
// counters, and the action "remote", which relays a request to another board
// over NowTP and replies (deferred) with {"status", "body", "ms"}. That lets
// a script on the host drive board-to-board traffic through one serial port.
#include <stdio.h>
#include <string.h>

#include <string>

#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "TesserAPI.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {

EventGroupHandle_t g_events;
constexpr EventBits_t kGotIp = 1;
char g_ip[16] = "";
char g_mac[18] = "";
nowtp::EspNowTransport* g_now;
tesser::NowTpTransport* g_nowApi;
tesser::HttpServer* g_http;

void onEvent(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(g_events, kGotIp);
    }
}

bool connectWifi() {
#if defined(TEST_WIFI_SSID)
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onEvent, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onEvent, nullptr);
    wifi_config_t wc = {};
    strncpy(reinterpret_cast<char*>(wc.sta.ssid), TEST_WIFI_SSID, sizeof(wc.sta.ssid));
    strncpy(reinterpret_cast<char*>(wc.sta.password), TEST_WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);
    // 15 dBm: at the driver's default 20 dBm, boards on weak USB supplies
    // trip the brownout detector while connecting.
    esp_wifi_set_max_tx_power(60);
    esp_wifi_connect();
    EventBits_t bits = xEventGroupWaitBits(g_events, kGotIp, pdFALSE, pdTRUE, pdMS_TO_TICKS(20000));
    return bits & kGotIp;
#else
    return false;
#endif
}

// Notifications received from nodes this one subscribed to over NowTP.
uint32_t g_notifications = 0;
std::string g_lastNotification = "null";
tesser::Mutex g_notifMutex;

bool parseMac(const char* s, nowtp::Mac& out) {
    unsigned b[6];
    if (!s || sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; i++) out.bytes[i] = static_cast<uint8_t>(b[i]);
    return true;
}

void describeNet(tesser::Object& net) {
    net.value("ip", g_ip);
    net.value("mac", g_mac);
    net.value("channel", [] { return g_now ? g_now->channel() : uint8_t(0); });
    net.custom("peers", [](tesser::JsonWriter& w) {
        w.beginArray();
        if (g_now) {
            for (const nowtp::PeerInfo& p : g_now->peers()) {
                char mac[18];
                snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", p.mac.bytes[0], p.mac.bytes[1],
                         p.mac.bytes[2], p.mac.bytes[3], p.mac.bytes[4], p.mac.bytes[5]);
                w.beginObject();
                w.key("mac");
                w.string(mac);
                w.key("name");
                w.string(p.name);
                w.key("rssi");
                w.integer(p.rssi);
                w.endObject();
            }
        }
        w.endArray();
    });
    auto& stats = net.object("endpoint");
    stats.value("requests", [] { return g_nowApi ? g_nowApi->endpoint().stats().requests : 0u; });
    stats.value("responses", [] { return g_nowApi ? g_nowApi->endpoint().stats().responses : 0u; });
    stats.value("timeouts", [] { return g_nowApi ? g_nowApi->endpoint().stats().timeouts : 0u; });
    stats.value("dropped", [] { return g_nowApi ? g_nowApi->endpoint().stats().dropped : 0u; });

    auto& notif = net.object("notifications");
    notif.value("count", [] { return g_notifications; });
    notif.custom("last", [](tesser::JsonWriter& w) {
        tesser::MutexGuard guard(g_notifMutex);
        w.raw(g_lastNotification);
    });

    net.action("remote", [](tesser::Call& call) {
        JsonVariantConst a = call.arg();
        nowtp::Mac mac;
        tesser::Op op = tesser::Op::Get;
        if (!g_nowApi || !parseMac(a["mac"].as<const char*>(), mac) ||
            (a["op"].is<const char*>() && !tesser::parseOp(a["op"].as<const char*>(), op))) {
            call.fail(tesser::Status::InvalidValue, "expected {mac, op, path, ...}");
            return;
        }
        tesser::Query q;
        if (a["depth"].is<int>()) q.depth = a["depth"].as<int>();
        if (a["keys"].is<const char*>()) q.keys = a["keys"].as<const char*>();
        if (a["exclude"].is<const char*>()) q.exclude = a["exclude"].as<const char*>();
        if (a["view"].is<const char*>()) tesser::parseView(a["view"].as<const char*>(), q.view);
        if (a["interval"].is<uint32_t>()) q.interval = a["interval"].as<uint32_t>();
        if (a["events"].is<bool>()) q.events = a["events"].as<bool>();
        tesser::Pending pending = call.defer();
        if (!pending) return;
        int64_t t0 = esp_timer_get_time();
        bool sent = g_nowApi->endpoint().request(
            tesser::NowTpTransport::address(mac), op, a["path"] | "/", a["body"],
            [pending, t0](tesser::Status s, JsonVariantConst body) mutable {
                int64_t ms = (esp_timer_get_time() - t0) / 1000;
                pending.replyWith([&](tesser::JsonWriter& w) {
                    w.beginObject();
                    w.key("status");
                    w.string(tesser::toString(s));
                    w.key("body");
                    w.variant(body);
                    w.key("ms");
                    w.integer(ms);
                    w.endObject();
                });
            },
            q, a["timeoutMs"] | 0u);
        if (!sent) pending.fail(tesser::Status::Busy, "NowTP send failed");
    });
}

}  // namespace

// Brings up Wi-Fi (when test/secrets.h exists), NowTP and HTTP, and adds /net.
void startNetwork(tesser::Api& api) {
    g_events = xEventGroupCreate();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();

    bool wifi = connectWifi();
    printf("WIFI %s ip=%s\n", wifi ? "connected" : "off", g_ip);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_mac, sizeof(g_mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    g_now = new nowtp::EspNowTransport();
    nowtp::EspNowConfig cfg;
    cfg.enableDiscovery = true;
    char name[24];
    snprintf(name, sizeof(name), "tesser-%02x%02x", mac[4], mac[5]);
    cfg.discovery.name = name;
    nowtp::Status ns = g_now->begin(cfg);
    g_nowApi = new tesser::NowTpTransport(&api, *g_now);
    bool nowOk = ns == nowtp::Status::Ok && g_nowApi->begin();
    g_nowApi->endpoint().onNotification([](const tesser::PeerAddress&, JsonObjectConst env) {
        tesser::MutexGuard guard(g_notifMutex);
        g_lastNotification.clear();
        serializeJson(env, g_lastNotification);
        g_notifications++;
    });
    g_now->onPeerEvent([](nowtp::PeerEvent e, const nowtp::PeerInfo& p) {
        if (e == nowtp::PeerEvent::Lost) g_nowApi->peerLost(p.mac);
    });
    printf("NOWTP %s mac=%s channel=%u port=%u\n", nowOk ? "started" : nowtp::toString(ns), g_mac,
           unsigned(g_now->channel()), unsigned(g_nowApi->port()));

    describeNet(api.object("net"));

    if (wifi) {
        g_http = new tesser::HttpServer(api);
        g_http->enableCors();
        g_http->enableWebSocket("/ws");
        g_http->setToken("test-token");
        printf("HTTP err=%d url=http://%s/api/\n", int(g_http->begin(80, "/api")), g_ip);
    }
}

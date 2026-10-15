// Wi-Fi, HTTP, NowTP and UDP for the on-target test firmware.
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
tesser::UdpTransport* g_udp;
tesser::Api* g_api;
bool g_udpEnabled = false;

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
                uint8_t port;
                uint32_t hash;
                if (tesser::NowTpTransport::parseAdvertisement(p.metadata, port, hash)) {
                    char hex[9];
                    snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(hash));
                    w.key("tesser");
                    w.beginObject();
                    w.key("port");
                    w.uinteger(port);
                    w.key("schema");
                    w.string(hex);
                    w.endObject();
                }
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

    // Mounts another board's tree as /peer (once), optionally mirrored: the
    // gateway feature, driven by test/e2e_gateway.py.
    net.action("mount", [](tesser::Call& call) {
        JsonVariantConst a = call.arg();
        nowtp::Mac mac;
        if (!g_nowApi || !parseMac(a["mac"].as<const char*>(), mac)) {
            call.fail(tesser::Status::InvalidValue, "expected {mac, mirror}");
            return;
        }
        if (g_api->child("peer")) {
            call.fail(tesser::Status::NotAllowed, "already mounted");
            return;
        }
        // Runs under the API lock, so growing the tree here is safe.
        tesser::RemoteNode& peer = g_nowApi->remote(*g_api, "peer", mac);
        if (a["mirror"] | false) peer.mirror(a["interval"] | 200u, 30000);
        call.reply(true);  // the advertisement follows the schema on its own
    });

    net.action("remote", [](tesser::Call& call) {
        JsonVariantConst a = call.arg();
        nowtp::Mac mac;
        tesser::Op op = tesser::Op::Get;
        // "upload" and "download" go as they are (the extra keys are ignored).
        bool file = a["op"] == "upload" || a["op"] == "download";
        if (!g_nowApi || !parseMac(a["mac"].as<const char*>(), mac) ||
            (!file && a["op"].is<const char*>() && !tesser::parseOp(a["op"].as<const char*>(), op))) {
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
        auto done = [pending, t0](tesser::Status s, JsonVariantConst body) mutable {
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
        };
        tesser::PeerAddress to = tesser::NowTpTransport::address(mac);
        bool sent = file ? g_nowApi->endpoint().request(to, a.as<JsonObjectConst>(), done, a["timeoutMs"] | 0u)
                         : g_nowApi->endpoint().request(to, op, a["path"] | "/", a["body"], done, q,
                                                        a["timeoutMs"] | 0u);
        if (!sent) pending.fail(tesser::Status::Busy, "NowTP send failed");
    });
}

// /net/routes: streamed sets to the boards under /both (test/e2e_routes.py).
uint32_t g_streamSeq = 0;
void describeRoutes(tesser::Object& net, tesser::Object& both) {
    auto& routes = net.object("routes");
    // {count, node}: streams /demo/seq = 1, 2, ... (counting on from the
    // last run) to the node of that name under /both (every one without),
    // at once; replies with the last value and how many sets were refused.
    routes.action("stream", [&both](tesser::Call& call) {
        uint32_t count = call.arg()["count"] | 1u;
        std::string only = call.arg()["node"] | "";
        uint32_t refused = 0;
        for (uint32_t i = 0; i < count; i++) {
            g_streamSeq++;
            char body[32];
            snprintf(body, sizeof(body), "{\"seq\":%lu}", static_cast<unsigned long>(g_streamSeq));
            for (const tesser::Node* c = both.first(); c; c = c->next()) {
                if (c->type() != tesser::NodeType::Remote || (!only.empty() && only != c->name())) continue;
                auto* node = const_cast<tesser::RemoteNode*>(static_cast<const tesser::RemoteNode*>(c));
                if (node->stream("/demo", body) != tesser::Status::Ok) refused++;
            }
        }
        JsonDocument out;
        out["last"] = g_streamSeq;
        out["refused"] = refused;
        call.reply(out.as<JsonVariantConst>());
    });
    // Per node: route in use, routes known, stream statistics.
    routes.value("state", [&both] {
        std::string s = "{";
        for (const tesser::Node* c = both.first(); c; c = c->next()) {
            if (c->type() != tesser::NodeType::Remote) continue;
            auto* node = static_cast<const tesser::RemoteNode*>(c);
            tesser::RemoteNode::Route r;
            bool have = node->route(r);
            tesser::RemoteNode::StreamStats st = node->streamStats();
            char item[200];
            snprintf(item, sizeof(item),
                     "%s\"%s\":{\"route\":\"%s\",\"routes\":%u,\"online\":%s,\"rttMs\":%lu,\"merged\":%lu,"
                     "\"sent\":%lu,\"failed\":%lu}",
                     s.size() > 1 ? "," : "", c->name(), have ? tesser::toString(r.kind()) : "none",
                     unsigned(node->routes().size()), node->online() ? "true" : "false",
                     static_cast<unsigned long>(have ? r.rttMs : 0), static_cast<unsigned long>(st.merged),
                     static_cast<unsigned long>(st.sent), static_cast<unsigned long>(st.failed));
            s += item;
        }
        return s + "}";
    });
}

// /net/udp: the UDP transport's state and controls (test/e2e_udp.py).
void describeUdp(tesser::Object& net) {
    auto& udp = net.object("udp");
    udp.value("port", [] { return g_udp->port(); });
    // Switched at run time, from any transport, UDP included.
    udp.value(
        "enabled", [] { return g_udp->running(); },
        [](bool on) {
            if (on && !g_udp->running()) {
                g_udpEnabled = g_udp->begin();
            } else if (!on) {
                g_udp->end();
                g_udpEnabled = false;
            }
        });
    udp.value(
        "peerAccess", [] { return std::string(tesser::toString(g_udp->endpoint().peerAccess())); },
        [](const std::string& s) {
            tesser::Access a;
            if (tesser::parseAccess(s, a)) g_udp->setPeerAccess(a);
        });
    // "ethernet", "wifi", "ap", comma-separated, or "all": restarts UDP on
    // them (docs/DESIGN.md section 10.7).
    udp.value(
        "interfaces",
        [] {
            uint8_t m = g_udp->interfaces();
            if (m == tesser::UdpTransport::kAllInterfaces) return std::string("all");
            std::string s;
            for (auto i : {tesser::NetInterface::Ethernet, tesser::NetInterface::Wifi, tesser::NetInterface::Ap}) {
                if (!(m & tesser::UdpTransport::interfaceBit(i))) continue;
                if (!s.empty()) s += ",";
                s += tesser::toString(i);
            }
            return s;
        },
        [](const std::string& text) {
            uint8_t m = 0;
            if (text == "all") m = tesser::UdpTransport::kAllInterfaces;
            size_t pos = 0;
            while (m != tesser::UdpTransport::kAllInterfaces && pos < text.size()) {
                size_t comma = text.find(',', pos);
                if (comma == std::string::npos) comma = text.size();
                tesser::NetInterface i;
                if (tesser::parseNetInterface(std::string_view(text).substr(pos, comma - pos), i)) {
                    m |= tesser::UdpTransport::interfaceBit(i);
                }
                pos = comma + 1;
            }
            if (!m) return false;
            g_udp->setInterfaces(m);
            g_udp->end();
            g_udpEnabled = g_udp->begin();
            return true;
        });
    // The interface each peer under /lan is reached by.
    udp.value("route", [] {
        std::string out;
        for (const tesser::Node* c = g_api->child("lan") ? static_cast<const tesser::Object*>(g_api->child("lan"))->first()
                                                         : nullptr;
             c; c = c->next()) {
            if (c->type() != tesser::NodeType::Remote) continue;
            tesser::RemoteNode::Route r;
            if (!static_cast<const tesser::RemoteNode*>(c)->route(r)) continue;
            if (!out.empty()) out += ",";
            out += std::string(c->name()) + "=" + r.name() + (r.online ? "" : "(offline)");
        }
        return out;
    });
    auto& stats = udp.object("stats");
    stats.value("sent", [] { return g_udp->link().stats().sent; });
    stats.value("received", [] { return g_udp->link().stats().received; });
    stats.value("retransmits", [] { return g_udp->link().stats().retransmits; });
    stats.value("failed", [] { return g_udp->link().stats().failed; });
    stats.value("duplicates", [] { return g_udp->link().stats().duplicates; });
    stats.value("dropped", [] { return g_udp->link().stats().dropped; });
    stats.value("requests", [] { return g_udp->endpoint().stats().requests; });
    // {path, value, to}: one broadcast write; `to` empty or absent is every
    // network. With "replies": true, waits 300 ms and lists who answered.
    udp.action("broadcast", [](tesser::Call& call) {
        JsonVariantConst a = call.arg();
        const char* path = a["path"] | "";
        const char* toText = a["to"] | "";
        tesser::PeerAddress to;
        if (!*path || (*toText && !tesser::parseUdpAddress(toText, g_udp->port(), to))) {
            call.fail(tesser::Status::InvalidValue, "expected {path, value, to}");
            return;
        }
        std::string body;
        serializeJson(a["value"], body);
        if (!(a["replies"] | false)) {
            if (!g_udp->broadcastSet(to, path, body)) call.fail(tesser::Status::Busy, "not sent");
            else call.reply(true);
            return;
        }
        tesser::Pending pending = call.defer();
        if (!pending) return;
        auto replies = std::make_shared<std::string>("[");
        bool sent = g_udp->endpoint().broadcast(
            to, tesser::Op::Set, path, body,
            [pending, replies](const tesser::PeerAddress& from, tesser::Status s, JsonVariantConst) mutable {
                if (from.length == 0) {
                    *replies += "]";
                    std::string r = *replies;
                    pending.replyWith([&](tesser::JsonWriter& w) { w.raw(r); });
                    return;
                }
                if (replies->size() > 1) *replies += ",";
                *replies += "{\"from\":\"" + tesser::formatUdpAddress(from) + "\",\"status\":\"" +
                            tesser::toString(s) + "\"}";
            },
            300);
        if (!sent) pending.fail(tesser::Status::Busy, "not sent");
    });
}

}  // namespace

// Brings up Wi-Fi (when test/secrets.h exists), NowTP and HTTP, and adds /net.
void startNetwork(tesser::Api& api) {
    g_api = &api;
    // Tokens over the message transports ("auth": UDP, NowTP, Bluetooth).
    api.authenticate([](std::string_view token) {
        return tesser::secretEquals(token, "test-token") ? tesser::Access::Admin : tesser::Access::Public;
    });
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
    // TesserAPI peers are mounted under /peers as they are discovered
    // (test/e2e_gateway.py), and shown offline when lost.
    g_now->onPeerEvent([](nowtp::PeerEvent e, const nowtp::PeerInfo& p) { g_nowApi->peerEvent(e, p); });
    printf("NOWTP %s mac=%s channel=%u port=%u\n", nowOk ? "started" : nowtp::toString(ns), g_mac,
           unsigned(g_now->channel()), unsigned(g_nowApi->port()));

    describeNet(api.object("net"));
    g_nowApi->mountPeers(api.object("peers").doc("TesserAPI boards around, mounted as they are discovered"));
    // Kept current from now on (re-advertised 1 s after the tree changes);
    // clients read the hash with view=hash.
    printf("ADVERTISE %d\n", int(g_nowApi->advertise()));

    if (wifi) {
        g_http = new tesser::HttpServer(api);
        g_http->enableCors();
        g_http->enableWebSocket("/ws");
        g_http->setToken("test-token");
        printf("HTTP err=%d url=http://%s/api/\n", int(g_http->begin(80, "/api")), g_ip);
        // <name>.local, and a _tesser._tcp service for TesserUI's bridge.
        printf("MDNS %d\n", int(g_http->advertise(name, name)));
    }

    // UDP on the LAN: LAN peers are mounted under /lan (/peers holds the
    // NowTP ones, and would take each board once).
    g_udp = new tesser::UdpTransport(&api);
    g_udp->setName(name);
    describeUdp(api.object("net"));
    g_udp->mountPeers(api.object("lan").doc("TesserAPI boards on the LAN, mounted as they advertise"));
    g_udpEnabled = wifi && g_udp->begin();
    printf("UDP %s port=%u advertise=%d\n", g_udpEnabled ? "started" : "off", unsigned(g_udp->port()),
           int(g_udp->advertise()));

    // Every board both ways (docs/DESIGN.md section 14.3): one node per
    // board, with a route per transport, UDP first (test/e2e_routes.py).
    tesser::RemoteNode::setDefaultRouteOrder({tesser::TransportKind::Udp, tesser::TransportKind::NowTP});
    auto& both = api.object("both").doc("TesserAPI boards over UDP and NowTP: a route each");
    g_udp->mountPeers(both);
    g_nowApi->mountPeers(both);
    describeRoutes(api.object("net"), both);
}

// One API, three transports: HTTP and WebSocket on the local network, and
// ESP-NOW (NowTP) for nearby boards. Flash it on two boards on the same Wi-Fi
// network, then:
//   curl http://<ip>/api/
//   curl -X PUT -d 200 http://<ip>/api/lamp/brightness
//   websocat ws://<ip>/ws   then   {"id":1,"op":"sub","path":"/lamp"}
// Each board subscribes to the other's lamp over ESP-NOW and prints changes.
#include <NowTP.h>
#include <TesserAPI.h>
#include <WiFi.h>

const char* ssid = "your-ssid";
const char* password = "your-password";

tesser::Api api;
tesser::HttpServer http(api);
nowtp::EspNowTransport radio;
tesser::NowTpTransport nowApi(&api, radio);

struct Lamp {
    bool on = false;
    int brightness = 128;
} lamp;

void setup() {
    Serial.begin(115200);

    auto& l = api.object("lamp");
    l.value("on", lamp.on);
    l.value("brightness", lamp.brightness).range(0, 255);
    l.action("toggle", [] { lamp.on = !lamp.on; });
    api.value("uptimeMs", [] { return millis(); });

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) delay(200);
    Serial.printf("http://%s/api/\n", WiFi.localIP().toString().c_str());

    http.enableWebSocket("/ws");
    http.begin(80, "/api");
    api.startTask();  // delivers change notifications to subscribers

    nowtp::EspNowConfig cfg;
    cfg.enableDiscovery = true;
    cfg.discovery.name = "tesser-lamp";
    radio.begin(cfg);  // shares the Wi-Fi channel
    radio.onPeerEvent([](nowtp::PeerEvent e, const nowtp::PeerInfo& p) {
        if (e == nowtp::PeerEvent::Lost) nowApi.peerLost(p.mac);
    });
    nowApi.begin();

    nowApi.endpoint().onNotification([](const tesser::PeerAddress&, JsonObjectConst msg) {
        Serial.printf("peer %s %s\n", msg["path"].as<const char*>(), msg["body"].as<std::string>().c_str());
    });
}

void loop() {
    // Subscribe to every newly discovered peer's lamp.
    static std::vector<nowtp::Mac> subscribed;
    for (const nowtp::PeerInfo& peer : radio.peers()) {
        bool known = false;
        for (const nowtp::Mac& m : subscribed) known = known || m == peer.mac;
        if (known) continue;
        subscribed.push_back(peer.mac);
        tesser::Query q;
        q.interval = 200;
        nowApi.endpoint().request(
            tesser::NowTpTransport::address(peer.mac), tesser::Op::Subscribe, "/lamp", std::string_view(),
            [](tesser::Status s, JsonVariantConst body) {
                Serial.printf("subscribed: %s %s\n", tesser::toString(s), body.as<std::string>().c_str());
            },
            q);
    }
    delay(500);
}

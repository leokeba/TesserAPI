// One API, two transports: HTTP on the local network and ESP-NOW (NowTP)
// for nearby boards. Flash it on two boards on the same Wi-Fi network, then:
//   curl http://<ip>/api/
//   curl -X PUT -d 200 http://<ip>/api/lamp/brightness
// Each board also polls the other's lamp over ESP-NOW and prints it.
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

    http.begin(80, "/api");

    nowtp::EspNowConfig cfg;
    cfg.enableDiscovery = true;
    cfg.discovery.name = "tesser-lamp";
    radio.begin(cfg);  // shares the Wi-Fi channel
    radio.onPeerEvent([](nowtp::PeerEvent e, const nowtp::PeerInfo& p) {
        if (e == nowtp::PeerEvent::Lost) nowApi.peerLost(p.mac);
    });
    nowApi.begin();
}

void loop() {
    static uint32_t last = 0;
    if (millis() - last < 5000) return;
    last = millis();
    for (const nowtp::PeerInfo& peer : radio.peers()) {
        nowApi.get(peer.mac, "/lamp", [name = peer.name](tesser::Status s, JsonVariantConst body) {
            Serial.printf("%s: %s %s\n", name.c_str(), tesser::toString(s), body.as<std::string>().c_str());
        });
    }
}

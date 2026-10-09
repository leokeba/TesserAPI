// One API, three transports: HTTP and WebSocket on the local network, and
// ESP-NOW (NowTP) for nearby boards. Flash it on two boards on the same Wi-Fi
// network, then:
//   curl http://<ip>/api/
//   curl -X PUT -d 200 http://<ip>/api/lamp/brightness
//   websocat ws://<ip>/ws   then   {"id":1,"op":"sub","path":"/lamp"}
// Each board also mounts every TesserAPI peer it discovers under
// /peers/<name>, so either board's HTTP API reaches the other over ESP-NOW
// (a gateway):
//   curl "http://<ip>/api/peers/?view=schema&depth=1"   the peers, online or not
//   curl http://<ip>/api/peers/tesser-lamp/lamp
//   curl -X POST http://<ip>/api/peers/tesser-lamp/lamp/toggle
//   websocat ws://<ip>/ws   then   {"id":1,"op":"sub","path":"/peers/tesser-lamp/lamp"}
#include <NowTP.h>
#include <TesserAPI.h>
#include <WiFi.h>

#ifndef WIFI_SSID
#define WIFI_SSID "your-ssid"
#define WIFI_PASSWORD "your-password"
#endif

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
    api.object("peers").doc("other boards, mounted as they are discovered");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) delay(200);
    Serial.printf("http://%s/api/\n", WiFi.localIP().toString().c_str());

    http.enableWebSocket("/ws");
    http.begin(80, "/api");
    api.startTask();  // delivers change notifications to subscribers

    nowtp::EspNowConfig cfg;
    cfg.enableDiscovery = true;
    cfg.discovery.name = "tesser-lamp";
    radio.begin(cfg);  // shares the Wi-Fi channel
    // Peers that advertise TesserAPI are mounted under /peers as they are
    // found, and shown offline when lost.
    radio.onPeerEvent([](nowtp::PeerEvent e, const nowtp::PeerInfo& p) { nowApi.peerEvent(e, p); });
    nowApi.mountPeers(api.object("peers"));
    nowApi.begin();
    nowApi.advertise();  // once the tree is complete
}

void loop() { delay(1000); }

// One API, three transports: HTTP and WebSocket on the local network, and
// ESP-NOW (NowTP) for nearby boards. Flash it on two boards on the same Wi-Fi
// network, then:
//   curl http://<ip>/api/
//   curl -X PUT -d 200 http://<ip>/api/lamp/brightness
//   websocat ws://<ip>/ws   then   {"id":1,"op":"sub","path":"/lamp"}
// Each board also mounts every peer it discovers under /peers/<id>, so either
// board's HTTP API reaches the other over ESP-NOW (a gateway):
//   curl http://<ip>/api/peers/            mirrored state of every peer
//   curl -X POST http://<ip>/api/peers/a1b2c3/lamp/toggle
#include <NowTP.h>
#include <TesserAPI.h>
#include <WiFi.h>

#include <list>
#include <string>
#include <vector>

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
    radio.onPeerEvent([](nowtp::PeerEvent e, const nowtp::PeerInfo& p) {
        if (e == nowtp::PeerEvent::Lost) nowApi.peerLost(p.mac);
    });
    nowApi.begin();
}

void loop() {
    // Mount each newly discovered peer as /peers/<last 3 MAC bytes>.
    static std::list<std::string> names;  // node names must outlive the tree
    static std::vector<nowtp::Mac> mounted;
    for (const nowtp::PeerInfo& peer : radio.peers()) {
        bool known = false;
        for (const nowtp::Mac& m : mounted) known = known || m == peer.mac;
        if (known) continue;
        mounted.push_back(peer.mac);
        char id[8];
        snprintf(id, sizeof(id), "%02x%02x%02x", peer.mac.bytes[3], peer.mac.bytes[4], peer.mac.bytes[5]);
        names.push_back(id);
        tesser::Lock lock(api);  // transports are running: grow the tree under the lock
        nowApi.remote(api.object("peers"), names.back().c_str(), peer.mac).mirror(200);
        Serial.printf("mounted %s as /peers/%s\n", peer.name.c_str(), id);
    }
    delay(500);
}

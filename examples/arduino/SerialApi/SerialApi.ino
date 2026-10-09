// Serves a small API over Serial, one JSON envelope per line. Try, in the
// serial monitor (newline line ending):
//   {"id":1,"op":"get","path":"/"}
//   {"id":2,"op":"set","path":"/lamp/brightness","body":200}
//   {"id":3,"op":"set","path":"/lamp/toggle"}
//   {"id":4,"op":"get","path":"/","view":"schema"}
//   {"id":5,"op":"sub","path":"/lamp"}       (then change something)
#include <TesserAPI.h>

#ifndef LED_BUILTIN
#define LED_BUILTIN 2  // most ESP32 devkits
#endif

tesser::Api api;
tesser::StreamTransport serialApi(api, Serial);

struct Lamp {
    bool on = false;
    int brightness = 128;
} lamp;

char deviceName[24] = "lamp-01";

void setup() {
    Serial.begin(115200);

    auto& l = api.object("lamp");
    l.value("on", lamp.on);
    l.value("brightness", lamp.brightness).range(0, 255);
    l.action("toggle", [] { lamp.on = !lamp.on; });

    auto& sys = api.object("system");
    sys.value("name", deviceName);
    sys.value("uptimeMs", [] { return millis(); });
    sys.value("heap", [] { return ESP.getFreeHeap(); });
    sys.action("restart", [] { ESP.restart(); });
}

void loop() {
    serialApi.poll();
    api.poll();  // change notifications for subscribers
    analogWrite(LED_BUILTIN, lamp.on ? lamp.brightness : 0);
}

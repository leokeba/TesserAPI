# TesserAPI

Describe your ESP32 firmware's state and operations **once**, as a tree of values and actions. Serve it as JSON over HTTP, over ESP-NOW (with [NowTP](https://github.com/leokeba/NowTP)), or over serial, from Arduino or ESP-IDF.

> **Status: early development.** The design is in [docs/DESIGN.md](docs/DESIGN.md). The core (tree, get/set/patch, shapes, schema) is being implemented first; transports, subscriptions and persistence follow. The API will change before 1.0.

```cpp
#include <TesserAPI.h>

tesser::Api api;
struct Lamp { bool on = false; int brightness = 128; } lamp;

void setup() {
    auto& l = api.object("lamp");
    l.value("on", lamp.on);
    l.value("brightness", lamp.brightness).range(0, 255);
    l.action("toggle", [] { lamp.on = !lamp.on; });
    l.value("temperature", [] { return readTemperature(); });   // read-only
}
```

Every field declared this way is readable, writable (when bound to a variable or given a setter), validated, and described in the schema. No routing code, no JSON handling, no per-field type checks.

## Querying the tree

Every subtree is addressable. These examples use HTTP; the same requests work over the [message envelope](docs/DESIGN.md#101-message-envelope-nowtp-serial-websocket) on NowTP and serial.

| Request | Result |
|---|---|
| `GET /api/` | the whole tree: `{"lamp":{"on":false,"brightness":128,"temperature":21.5}}` |
| `GET /api/lamp?keys=on,brightness` | only those children |
| `GET /api/?depth=1` | one level, deeper objects as `{}` |
| `GET /api/lamp?view=schema` | types, writability, ranges, actions |
| `GET /api/` with body `{"lamp":{"on":null}}` | a sparse *shape*: exactly the fields you name |
| `PUT /api/lamp/brightness` body `200` | set one value |
| `POST /api/lamp` body `{"on":true,"brightness":10}` | patch several values: all are validated before any is applied |
| `POST /api/lamp/toggle` | call an action |

## Composition

A component describes itself once and can be mounted any number of times:

```cpp
void describe(tesser::Object& o, Motor& m) {
    o.value("speed", [&m] { return m.speed(); }, [&m](double v) { m.setSpeed(v); });
    o.value("invert", m.invert);
    o.action("stop", [&m] { m.stop(); });
}

api.mount("left", leftMotor);    // /left/speed, /left/invert, /left/stop
api.mount("right", rightMotor);
```

## Transports

| Transport | Framing | Status |
|---|---|---|
| Serial (`LineTransport`) | one JSON envelope per line | in progress |
| HTTP (`esp_http_server`, also usable under PsychicHttp) | REST mapping, streamed chunked responses | planned |
| NowTP (ESP-NOW) | one JSON envelope per message, reliable unicast | planned |
| WebSocket | JSON envelope, subscriptions | planned |

## Requirements

- ESP-IDF ≥ 5.1 or Arduino-ESP32 ≥ 3.0, C++17
- [ArduinoJson](https://arduinojson.org) 7 (used to parse requests; responses are streamed without building a document)
- Targets small chips: classic ESP32 without PSRAM and ESP32-C3 are the reference boards

## License

MIT

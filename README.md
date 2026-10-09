# TesserAPI

Describe your ESP32 firmware's state and operations **once**, as a tree of values and actions. Serve it as JSON over HTTP, over ESP-NOW (with [NowTP](https://github.com/leokeba/NowTP)), or over serial, from Arduino or ESP-IDF.

> **Status: early development.** The design is in [docs/DESIGN.md](docs/DESIGN.md). Everything in the design works and is tested on hardware: the core (tree, get/set/patch, shapes, schema, lists), subscriptions and events, persistence, access control, the serial, HTTP, WebSocket and NowTP transports, and gateways that mount other nodes. The API will change before 1.0.

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

## Live updates

Message transports (serial, WebSocket, NowTP) can subscribe to any subtree, and get sparse change notifications and events:

```cpp
auto& button = api.object("button");
auto& pressed = button.event("pressed");
api.value("temperature", temperature).watch();   // sampled: notices plain variable changes
api.startTask();                                   // or call api.poll() from loop()

pressed.emit(3);                                   // → {"op":"event","path":"/button/pressed","body":3}
```

```json
→ {"id":1,"op":"sub","path":"/","keys":"temperature","interval":500}
← {"id":1,"status":"ok","body":{"temperature":21.5}}
← {"op":"change","path":"/","body":{"temperature":22.0}}
```

## Lists

```cpp
std::vector<Remote> remotes;
api.list("remotes", remotes);    // GET /remotes, /remotes/1/host; PUT /remotes [...] replaces the list
```

## Gateway

A node can graft another node's tree into its own, reached over ESP-NOW:

```cpp
nowApi.remote(api, "kitchen", kitchenMac).mirror();   // GET /api/kitchen/lamp is forwarded over ESP-NOW
```

With a mirror, the remote's state also appears in local reads and subscriptions, events included.

## Persistence and access control

```cpp
tesser::NvsStorage storage;
api.object("config").persist();                 // saved to NVS 2 s after a change
api.persistence(storage);
api.load();                                       // unknown or invalid stored keys are skipped

http.setToken("s3cret");                          // Authorization: Bearer s3cret
api.authorize(tesser::authorizers::readOnlyUnlessAuthenticated());
```

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
| Serial (`LineTransport`, `StreamTransport`, `UartTransport`) | one JSON envelope per line | done |
| HTTP (`HttpServer`, on `esp_http_server`, also usable under PsychicHttp) | REST mapping, streamed chunked responses, CORS | done |
| NowTP (`NowTpTransport`, ESP-NOW) | one JSON envelope per message, reliable unicast; also a client for other nodes | done |
| WebSocket (`HttpServer::enableWebSocket`) | JSON envelope, subscriptions | done |

## Examples

- [examples/arduino/SerialApi](examples/arduino/SerialApi): the API over the serial monitor.
- [examples/arduino/HttpAndNowTP](examples/arduino/HttpAndNowTP): the same API over HTTP, WebSocket and ESP-NOW; each board mounts the boards it discovers (a gateway).
- [examples/idf/http_api](examples/idf/http_api): ESP-IDF, HTTP.

## Testing

- **Host:** 65 test cases under ASan and UBSan (`cmake -S . -B build && cmake --build build && ./build/tesser_tests`).
- **On the chip:** the same cases run on target (`test/hardware`).
- **End to end:** scripts drive two boards over serial, HTTP, WebSocket and ESP-NOW. The same conformance vectors must give the same results on every transport and through a gateway. Persistence is checked across a real reboot. See [docs/DESIGN.md §17](docs/DESIGN.md#17-testing).
- **CI** builds ESP-IDF 5.1 to latest and Arduino-ESP32 3.x for the ESP32 and the ESP32-C3. Hardware testing so far used classic ESP32 boards; the C3 is built but not yet run.

## Requirements

- ESP-IDF ≥ 5.1 or Arduino-ESP32 ≥ 3.0, C++17
- [ArduinoJson](https://arduinojson.org) 7 (used to parse requests; responses are streamed without building a document)
- Targets small chips: classic ESP32 without PSRAM and ESP32-C3 are the reference boards

## License

MIT

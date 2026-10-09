# TesserAPI design

Status: draft, written before the first release. Sections marked *(planned)* describe behaviour the design commits to but that isn't implemented yet. The rest is the specification the code follows. When the two disagree, fix one of them.

## 1. Goals

TesserAPI lets ESP32 firmware describe its state and operations **once**, as a tree of named values, actions and events. It then serves that tree as JSON over any transport: HTTP, ESP-NOW (through [NowTP](https://github.com/leokeba/NowTP)), serial, and later WebSocket.

- **One declaration per field.** Reading, writing, validation, schema, persistence and change notification all come from the same line of code. The predecessor of this library ([open-heliostat `src/lib`](https://github.com/open-heliostat/open-heliostat/tree/helionext/src/lib)) declared each field three times: a reader, an update router and a save map, each with hand-written type checks.
- **Simple things take one line.** Exposing a variable is `o.value("brightness", brightness)`. Custom behaviour is possible without writing a service class.
- **The client chooses what it reads.** Any subtree is addressable by path, and the response can be narrowed by depth, by key list, or by a sparse JSON *shape*.
- **Transport-agnostic.** The core never sees sockets, MAC addresses or HTTP status codes.
- **Fits small chips.** Targets the classic ESP32 without PSRAM and the ESP32-C3. No DOM is built for responses, memory use is bounded and predictable, and there are no exceptions or RTTI.

### Non-goals

- Automatic reflection over C++ members. Exposure is explicit.
- A general query language: no JSONPath, predicates or wildcards.
- A custom JSON parser. We use ArduinoJson to *parse*. *Writing* JSON is simple, so we stream it ourselves.
- Being an HTTP framework: routing for HTML pages, static files, uploads and so on stays in whatever server the application already uses.

## 2. Constraints

| Item | Decision |
|---|---|
| Language | C++17. No exceptions (`-fno-exceptions` is the ESP-IDF default), no RTTI (off in both ESP-IDF and Arduino-ESP32). |
| Frameworks | ESP-IDF ≥ 5.1, and Arduino-ESP32 ≥ 3.0 (which is ESP-IDF 5.1+). Arduino-ESP32 2.x is not supported: it compiles with gnu++11 by default. |
| Chips | Any ESP32-family chip. Reference targets: classic ESP32 (no PSRAM, 4 MB flash) and ESP32-C3. |
| JSON | ArduinoJson 7 to parse requests. Our own streaming writer for responses. |
| Allocation | The tree is allocated once, during setup. Per-request allocation is limited to the parsed request body, plus the response buffer for transports that need one. Everything has a configured upper bound. |
| Platform code | Only in `src/tesser/platform*` and `src/tesser/transports/*`. The core is host-compiled and host-tested. |

## 3. Concepts

```
 Application ── declares ──▶  Tree (Object / Value / Action / Event / Custom nodes)
                                    ▲
                                    │ Request{op, path, query, body, client}
 Transport ─── parses ─────▶  Api::handle()  ─── streams JSON ──▶ Reply ──▶ Transport
 (HTTP, NowTP, serial, WS)          │
                                    └── (planned) change tracking ─▶ subscriptions ─▶ notifications
```

- **`Api`**: the root of the tree. Handles requests, and owns the lock, the configuration and (later) subscriptions.
- **Node**: an element of the tree. Every node has a name (a string literal that is never copied) and a kind:
  - **Object**: named children.
  - **Value**: a typed scalar bound to a variable, or to a getter and an optional setter.
  - **Action**: a callable with zero or one typed argument and an optional return value.
  - **Event**: something that happens and is pushed to subscribers *(planned)*.
  - **Custom**: an escape hatch. User code writes JSON directly and receives a `JsonVariantConst` to apply.
- **`Request`**: what a transport hands to the core: an operation, a path, query options, a parsed body, and a client identity.
- **`Reply`**: what the core writes the response into. The transport implements it: it decides how a status is encoded and where the body bytes go.
- **Transport**: turns its own framing into a `Request`, and the `Reply` into its own framing.

"Stateful" and "stateless" are not types. A service with only actions is stateless, and one with values is stateful. Both are just subtrees.

## 4. Declaring an API

```cpp
#include <TesserAPI.h>

tesser::Api api;

struct Lamp { bool on = false; int brightness = 128; } lamp;

void setup() {
    auto& l = api.object("lamp");
    l.value("on", lamp.on);                                    // read/write, bound to a variable
    l.value("brightness", lamp.brightness).range(0, 255);      // validated on write
    l.action("toggle", [] { lamp.on = !lamp.on; });            // no argument, no result
    l.value("temperature", [] { return readTemp(); });         // getter only: read-only
    l.value("version", "1.2.0");                               // constant: read-only
}
```

### 4.1 Values

| Declaration | Readable | Writable |
|---|---|---|
| `value(name, T& var)` | yes | yes, unless `.readOnly()` |
| `value(name, const T& var)` | yes | no |
| `value(name, getter)` | yes | no |
| `value(name, getter, setter)` | yes | yes |
| `value(name, "literal")` | yes | no |

The getter has the form `T()`. The setter has the form `void(T)`, or `bool(T)` / `tesser::Status(T)` to reject a value at apply time. `T` is deduced from the getter's return type.

**Supported `T`:** `bool`, all integer types up to 64 bits, `float`, `double`, `std::string`, `char[N]` (a fixed buffer, NUL-terminated), Arduino `String` (when `ARDUINO` is defined), and `const char*` (read-only).

**Modifiers** (chainable on the returned node):

| Modifier | Effect |
|---|---|
| `.range(min, max)` | Numeric bounds, checked on write and reported in the schema. |
| `.readOnly()` | Rejects writes. |
| `.persist()` | Included in the persisted snapshot *(planned)*. |
| `.describe("text")` | Schema description. Compiled out with `TESSER_NO_DESCRIPTIONS`. |

Integers are range-checked against `T` before any user range: writing `300` to a `uint8_t` is `invalid_value`, never a wrap-around. Integers are accepted for floating-point values. Nothing else is coerced: a string `"12"` is not a number.

### 4.2 Actions

```cpp
o.action("reboot", [] { esp_restart(); });                       // no arguments
o.action("moveTo", [](float deg) { motor.moveTo(deg); });        // one typed argument
o.action("count",  [] { return counter; });                      // returns a value
o.action("addRemote", [](JsonVariantConst v) { ... return tesser::Status::Ok; });  // raw argument
o.action("calibrate", [](tesser::Call& call) {                   // deferred completion
    auto pending = call.defer();
    startCalibration([pending](float r) mutable { pending.reply(r); });
});
```

- The argument is the request body:
  - no argument: the body is ignored (it should be absent or `null`)
  - one typed argument: the body must convert as described for values
  - `JsonVariantConst`: the raw body
- An action may return `void`, a supported value type, or `tesser::Status`.
- More than one argument is not supported. Use an object body with a raw argument, or a nested object of values plus an action (see §6.3).

### 4.3 Composition

Any class can describe itself into an object. `mount` creates a child object and calls `describe(Object&, T&)`, found by argument-dependent lookup:

```cpp
void describe(tesser::Object& o, Motor& m) {
    auto& c = o.object("control");
    c.value("speed", [&m] { return m.getSpeed(); }, [&m](double v) { m.setSpeed(v); });
    c.value("direction", [&m] { return m.getDirection(); }, [&m](bool d) { m.setDirection(d); });
    auto& cfg = o.object("config");
    cfg.value("minVal", [&m] { return m.getMin(); }, [&m](double v) { m.setMin(v); }).persist();
    cfg.value("invert", m.invert).persist();
}

api.mount("motor1", motor1);   // /motor1/control/speed ...
api.mount("motor2", motor2);
```

That replaces the roughly 140 lines of `DCMotorService` in the predecessor.

### 4.4 Names and the tree's lifetime

- Names are `const char*` and are never copied. Use string literals or strings that outlive the `Api`.
- A name contains only `[A-Za-z0-9_.-]`. Names are unique among siblings, and adding a duplicate fails, which is reported by `api.errors()` and on the log.
- The tree is built during setup. Adding nodes after transports have started is allowed only while holding the API lock (§9).

## 5. Requests

```cpp
struct Request {
    Op op;                  // Get, Set (planned: Subscribe, Unsubscribe)
    Str path;               // "/lamp/brightness"
    Query query;            // depth, keys, exclude, view
    JsonVariantConst body;  // Set: the value or patch. Get: an optional shape
    Client client;          // transport id, address, authenticated flag
};
```

### 5.1 Paths

- `/` or the empty path is the root.
- A trailing slash is ignored: `/lamp` and `/lamp/` are the same resource. An empty segment (`//`) is `bad_request`.
- A path that leaves the tree, for example `/lamp/on/x`, is `not_found`. The error names the longest prefix that resolved.
- No percent-decoding inside the core, since names can't contain characters that need it. HTTP decodes the URL before handing over the path.

### 5.2 Query options (Get)

| Option | Values | Meaning |
|---|---|---|
| `depth` | `0`–`255`, default unlimited | Levels of objects to expand below the target. An object beyond the limit renders as `{}`. `depth=0` on an object gives `{}`. Values are always rendered. |
| `keys` | comma-separated names | Only these direct children of the target. An unknown name is `not_found`. |
| `exclude` | comma-separated names | All direct children except these. |
| `view` | `value` (default), `schema` | The representation to return (§7). |

`keys` and `exclude` apply only at the target level. Nested selection uses a shape (§5.3). `keys` together with `exclude` is `bad_request`. With `depth`, the target itself is level 0.

### 5.3 Shapes (Get with a body)

A Get request may carry a body that is a sparse copy of the tree, the **shape**:

```json
GET /motor1   body: {"control": {"speed": null}, "config": true}
→ {"control": {"speed": 0.5}, "config": {"minVal": 0.1, "invert": false}}
```

- A key whose value is `null` or `true` includes that child in full, subject to `depth`.
- A key whose value is an object recurses with that object as the sub-shape.
- Any other value, or a name that doesn't exist, is `bad_request` or `not_found` respectively.
- A shape replaces `keys` and `exclude`, and sending both is `bad_request`.

This is the predecessor's "nested GET" feature, made well-defined. Over HTTP the shape goes in the GET body, which `esp_http_server` accepts. Message transports put it in the envelope's `body`.

## 6. Operations

### 6.1 Get

This returns the target rendered in the requested view, after applying `depth` and then either `keys`/`exclude` or the shape. Actions and events never appear in the `value` view, and custom nodes render whatever their writer writes.

Rendering walks only the requested branch and streams it straight to the reply, so no JSON document is built. `depth`, `keys` and the shape prune the walk itself.

### 6.2 Set on a value or an action

- **Value:** the body is the new value. The response is the value read back after the write.
- **Action:** the body is the argument. The response is the return value, `null` for `void` or `Status::Ok`, or the deferred result.
- **Object:** see §6.3.

`PUT /lamp/brightness 200` → `200`. `POST /lamp/toggle` → `null`.

### 6.3 Set on an object: patches

The body must be a JSON object, and it is applied as a merge patch:

```json
POST /motor1  {"control": {"speed": 0.8, "direction": true}, "config": {"invert": true}}
→ {"control": {"speed": 0.8, "direction": true}, "config": {"invert": true}}
```

The patch is processed in three passes:

1. **Validate everything.** Every key must exist. Values must convert and pass `range`, and must not be read-only. Action arguments must convert. Custom nodes run their optional validator. If any check fails, **nothing is applied**, and the error names the first failing path.
2. **Apply values** in document order.
3. **Call actions** in document order, after all values are applied. So `{"position": 10, "go": null}` sets the position first and then starts the move.

The response is the patched keys read back, which is the patch used as a shape. An action's key renders as `null`; call an action directly to get its return value. Actions finish before the response starts, so a failing action can still turn the whole reply into an error.

**Limits:**
- A setter that returns `false`, an action that returns a failure status, or a custom node that fails at apply time stops the patch. Earlier writes remain applied: only pass 1 is atomic. The error says so with `"partial": true`.
- Deferred actions can't be part of a patch (`bad_request`). Call them directly.

This mirrors the predecessor's model, where a POST body mixed values and triggers, but with validation done before anything is applied.

### 6.4 Subscribe and Unsubscribe *(planned)*

See §11.

## 7. Representations

**Value view:** plain JSON with the same shape as the tree. Objects become objects, values become scalars, actions and events are left out.

**Schema view:**

```json
GET /lamp?view=schema
{
  "type": "object",
  "children": {
    "on":          {"type": "boolean", "writable": true},
    "brightness":  {"type": "integer", "writable": true, "min": 0, "max": 255},
    "temperature": {"type": "number"},
    "toggle":      {"type": "action"},
    "moveTo":      {"type": "action", "arg": "number"},
    "pressed":     {"type": "event"}
  }
}
```

- **Types:** `object`, `boolean`, `integer`, `number`, `string`, `action`, `event`, `custom`.
- **Optional keys:** `writable` (only when true), `min`, `max`, `persist`, `description`, `arg` and `returns` (actions), `maxLength` (`char[N]`).
- `depth`, `keys` and the shape apply to `children` the same way.
- A schema leaf is always an object containing `"type"`. Children are always under `"children"`, so a child named `type` can't be confused with a descriptor.
- Schema output is compiled out with `TESSER_NO_SCHEMA` when flash is tight.

## 8. Status codes

Statuses are transport-neutral. Each transport encodes them its own way.

| Status | Meaning | HTTP |
|---|---|---|
| `ok` | Success | 200 |
| `bad_request` | Malformed request, envelope, body JSON or query | 400 |
| `not_found` | The path or a patch key doesn't exist | 404 |
| `invalid_value` | Type mismatch, out of range, string too long | 422 |
| `read_only` | Write to a read-only value | 405 |
| `not_allowed` | The operation isn't valid on this node, e.g. Set on an event | 405 |
| `unauthorized` | Rejected by the authorizer (§13) | 403 |
| `too_large` | The request body or response exceeds the configured limit | 413 |
| `busy` | Queue full, or too many deferred calls | 503 |
| `timeout` | A deferred call didn't complete in time | 504 |
| `internal` | Bug or out of memory | 500 |

The error body is the same on every transport:

```json
{"error": "invalid_value", "path": "/lamp/brightness", "message": "expected integer in [0, 255]"}
```

`message` is compiled out with `TESSER_NO_MESSAGES`.

## 9. Execution and concurrency

**The core is single-threaded.** `Api` holds one recursive mutex. `Api::handle()` takes it, and so do the notification and persistence routines. On ESP32 several tasks touch the API: the HTTP server task, the NowTP task, `loop()`, and control tasks. The rules are:

- **Inline mode** (the default): a transport calls `api.handle()` from its own task, and the handler runs there under the lock. This is the lowest-latency, lowest-memory option.
- **Queued mode** *(planned)*: transports post requests into a bounded queue, which is drained by `api.poll()` (from `loop()`) or by `api.startTask()`. Use it when handlers must run in the application's context, for example when they touch code that isn't thread-safe. A full queue answers `busy`.
- **Application code** that mutates bound state from another task while transports are running should hold `tesser::Lock lock(api);`. Aligned reads and writes of 32-bit-or-smaller variables are atomic on ESP32, so simple flags and counters are safe without it. Multi-field invariants and strings are not.
- **Handlers must not block.** Long work belongs in a deferred action (§4.2) or a task, which reports completion through `pending.reply()` or through an event.

**Deferred completion:**
- `call.defer()` asks the transport to keep the reply open. Message transports (NowTP, serial, WebSocket) support this naturally.
- The HTTP server transport parks the request and blocks only that connection's handler, up to `deferTimeoutMs`. It then answers `timeout`. ESP-IDF's async request API can replace this later.
- `pending.reply()` may be called from any task.
- A `Pending` object that is destroyed without a reply answers `internal`, so a forgotten reply can't hang the client.

## 10. Transports

### 10.1 Message envelope (NowTP, serial, WebSocket)

These transports are message-based and two-way, so they share one JSON envelope.

```json
→ {"id": 7, "op": "get", "path": "/lamp", "depth": 1, "keys": "on,brightness"}
→ {"id": 8, "op": "set", "path": "/lamp/brightness", "body": 200}
→ {"id": 9, "op": "get", "path": "/motor1", "body": {"control": true}}          // shape
← {"id": 7, "status": "ok", "body": {"on": true, "brightness": 128}}
← {"id": 8, "status": "invalid_value", "body": {"error": "invalid_value", "path": "/lamp/brightness", "message": "..."}}
← {"op": "change", "path": "/sensors", "body": {"temperature": 22.5}}           // planned, no id
← {"op": "event",  "path": "/button/pressed", "body": null}                      // planned, no id
```

- **`id`:** any JSON scalar, echoed verbatim. It may be omitted, in which case the response also has no `id`.
- **`op`:** `get` or `set` (planned: `sub`, `unsub`, `ping`). The keys `path`, `depth`, `keys`, `exclude`, `view` and `body` map one-to-one onto `Request`.
- **Unknown envelope keys** are ignored, for forward compatibility.
- **Response:** the response is written as a stream: `{"id":…,"status":"…","body":` followed by the body and then `}`. Buffered transports can still replace it with an error if the body overflows the limit.
- **Framing** is the transport's job:
  - Serial: one envelope per line (`\n`). Non-JSON lines from the log are ignored by clients.
  - NowTP: one envelope per message.
  - WebSocket: one per text frame.

### 10.2 HTTP (`esp_http_server`)

```cpp
tesser::HttpServer http(api);
http.begin(80, "/api");          // starts its own server, or:
http.attach(handle, "/api");     // registers a wildcard URI on an existing httpd_handle_t
```

`attach` works with any server built on `esp_http_server`, including PsychicHttp, so it coexists with an existing web UI.

| HTTP | Operation |
|---|---|
| `GET /api/<path>?depth=&keys=&exclude=&view=` | get. The body, if present, is a shape. |
| `PUT`, `PATCH` or `POST /api/<path>` | set. The body is JSON. |
| `OPTIONS` | CORS preflight, when CORS is enabled |

- Responses are streamed with chunked encoding through a 512-byte buffer, so there is no limit on response size.
- Request bodies are capped by `maxRequestBody`.
- The status is mapped as in §8.
- The handler runs in the httpd task (inline mode).

### 10.3 NowTP

```cpp
tesser::NowTpTransport now(api, transport, /*port*/ 84);
```

- **Requests:** one envelope per NowTP message on the configured port. Requests sent to the broadcast address are answered only for `get`. `set` sent by broadcast is rejected.
- **Responses:** reliable unicast to the sender's MAC.
- **Response size:** buffered in full (NowTP has no streaming), up to `min(maxResponse, nowtp maxMessageSize)`. Anything larger gets a `too_large` reply. Clients on NowTP should use `depth`, `keys` and shapes.
- **Notifications** *(planned)*:
  - `change` notifications use **latest-only** mode, because a newer state supersedes an older one.
  - `event` notifications use **reliable** mode.
- **Client identity:** the MAC address. NowTP's "peer lost" discovery event drops that client's subscriptions.
- **Security:** ESP-NOW frames aren't authenticated. Writes over NowTP should be restricted with an allowlist of MACs or encrypted peers (§13).
- **Threading:** handlers run in NowTP's task (`runTask = true`) or in `transport.poll()`, under the API lock.
- **Optional:** the adapter is compiled only when NowTP is available. The core never includes NowTP.

### 10.4 Serial

`tesser::LineTransport` is platform-independent: it is fed bytes, and it calls a write function for each response line. There are thin adapters for an Arduino `Stream` and an ESP-IDF UART. This is the debugging transport, and the harness for end-to-end tests on hardware.

### 10.5 WebSocket *(planned)*

`esp_http_server`'s WebSocket support (`CONFIG_HTTPD_WS_SUPPORT`, enabled in Arduino-ESP32 3.x) on the same server as the HTTP transport, using the same envelope as §10.1. This is where browser subscriptions live.

## 11. Change tracking and subscriptions *(planned)*

**Tracking changes:**
- Each node has a 16-bit "changed at" generation, and the API has a global generation counter.
- A value is marked changed:
  - automatically, when a set goes through the API
  - explicitly, with `node.changed()` or `api.changed("/path")`
  - by polling, with `.watch(intervalMs)`: the runtime samples the value and compares a 32-bit hash, at a cost of 4 bytes per watched leaf.
- Bound plain variables can't announce their own changes, so either `changed()` or `watch()` is needed for push updates.

**Subscribing:**

```json
→ {"id": 3, "op": "sub", "path": "/sensors", "keys": "temperature,humidity", "interval": 500}
← {"id": 3, "status": "ok", "body": {"temperature": 22.5, "humidity": 48}}    // initial snapshot
← {"op": "change", "path": "/sensors", "body": {"temperature": 22.7}}         // later, only changed leaves
```

- A change notification is a **merge patch** rooted at the subscription path, containing only the leaves whose generation is newer than the subscription's last flush. Clients apply it exactly like a `set` body, so one format serves writes, notifications and persistence.
- **Rate limit:** at most one notification per subscription per `interval` ms. Intermediate states are coalesced, which suits NowTP's latest-only mode.
- **Events** are never coalesced. Each client has a bounded queue (default 8). On overflow the oldest event is dropped and the next delivery carries `"overflow": true`.
- **Limits:** the maximum number of subscriptions per client and in total is configured. A slow client never causes unbounded allocation.
- **Cleanup:** subscriptions die with their client: WebSocket close, NowTP peer lost, or an explicit `unsub`.

## 12. Persistence *(planned)*

- Values and objects marked `.persist()` are saved as a single JSON patch, which is the value view filtered to persisted leaves.
- Storage backends: NVS (one blob, available on both frameworks), or a file on LittleFS. Both implement `tesser::Storage`.
- **Saving:** a save is debounced (default 2 s after the last change to a persisted leaf) and happens in `poll()` or in the API task, never in a request handler.
- **Loading:** at startup, the stored patch is applied through the normal set path, so validation runs. Unknown keys are skipped with a warning, and values that fail validation keep their defaults. This is how renamed or removed fields stay harmless.

## 13. Security *(planned)*

- `api.authorize(fn)`: `fn(const Client&, Op, const Node&) -> bool` is consulted for every get, set and subscribe.
- The `Client` carries the transport kind, the address (IP or MAC), and whether the transport authenticated it.
- **NowTP adapter options:** `allow` (a list of MACs) and `requireEncryption`. Unless configured otherwise, `set` from unknown peers is rejected.
- **HTTP:** a bearer-token check is provided as a ready-made authorizer. TLS is the application's choice of server.
- Hiding nodes from the schema is not access control: the authorizer runs on every access.

## 14. Remote trees and the client *(planned)*

- **Client:** `tesser::Client` issues envelope requests over a message transport and matches responses by `id`:
  ```cpp
  client.get(mac, "/lamp", [](tesser::Status s, JsonVariantConst body) { ... });
  ```
- **Remote mount:** `api.mount("kitchen", tesser::remote(client, kitchenMac))` grafts a remote node's tree into the local one. Get and set on `/kitchen/...` are forwarded with the path rewritten and complete through deferral. A gateway thereby exposes ESP-NOW nodes over HTTP.
- **Caveat:** on a gateway, Wi-Fi station mode and ESP-NOW share one channel.

## 15. Memory budget

Targets to be measured on a classic ESP32 and a C3, and enforced in CI:

| Item | Target |
|---|---|
| Flash, core + HTTP transport | ≤ 40 KB above the application's baseline, ArduinoJson included |
| RAM per value leaf | ≤ 32 B with a bound variable, ≤ 48 B with a getter and setter |
| RAM per object node | ≤ 24 B |
| Request handling | Request body document (bounded by `maxRequestBody`) + 512 B streaming buffer (HTTP), or the response buffer (NowTP, bounded by `maxResponse`) |
| Stack | Rendering depth bounded by `maxDepth` (default 16) |

Configuration (`tesser::Config`, at runtime):

| Option | Default |
|---|---|
| `maxRequestBody` | 4096 |
| `maxResponse` (buffered transports) | 8192 |
| `maxDepth` | 16 |
| `deferTimeoutMs` | 10000 |
| `maxPending` | 4 |

Compile-time switches: `TESSER_NO_SCHEMA`, `TESSER_NO_DESCRIPTIONS`, `TESSER_NO_MESSAGES`.

## 16. Packaging

The layout follows NowTP:
- `CMakeLists.txt` is an ESP-IDF component under `idf.py`, and a host library plus tests otherwise.
- `idf_component.yml` depends on `bblanchon/arduinojson`.
- `library.properties` and `library.json` for Arduino and PlatformIO.

Optional transports (NowTP) compile only when their dependency is present: `__has_include` on Arduino and PlatformIO, and a check of the build components on ESP-IDF.

## 17. Testing

1. **Host tests** (CMake + ctest, ASan and UBSan, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` on the library). They cover the core, the envelope codec and the line transport, and run against NowTP's simulated network for the NowTP adapter.
2. **On-target tests** (`test/hardware`, ESP-IDF): the same core cases on the chip, plus heap and stack measurements.
3. **End-to-end over serial:** a Python script (run with `uv`) drives a test firmware through `LineTransport` and checks the responses.
4. **Transport conformance:** the same request vectors go through every transport and must produce equivalent responses.
5. **CI:** host tests, plus ESP-IDF 5.1 / 5.4 / 5.5 / latest × ESP32 / ESP32-C3, plus Arduino-ESP32 3.x × ESP32 / ESP32-C3.

## 18. Roadmap

| Phase | Content | Status |
|---|---|---|
| 1 | Core: tree, values, actions, custom nodes, get / set / patch / shape / schema, streaming writer, envelope, line transport, host + on-target tests | in progress |
| 2 | HTTP and NowTP transports, conformance tests, footprint measurements | |
| 3 | Change tracking, subscriptions, events, WebSocket | |
| 4 | Persistence, authorizer, NowTP allowlist | |
| 5 | Client, remote mount (gateway), queued execution mode, lists of objects | |

## 19. Open questions

- **Lists of described objects** (the predecessor's remotes list). Arrays are `custom` nodes for now. A first-class list needs element-relative bindings.
- **Enum values:** expose them as strings, with the allowed values in the schema.
- **Binary envelope for NowTP:** only if measurements show the JSON overhead matters at 250-byte frames.
- **Schema hash in NowTP discovery metadata,** so clients can cache schemas.

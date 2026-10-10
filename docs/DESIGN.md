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

**Enums** are exposed as strings: `o.value("mode", mode, {"off", "auto", "manual"})` names the enumerators in order of their underlying values. Writes accept only those names, and the schema lists them under `"enum"`. An enumerator without a name reads as its number.

**Modifiers** (chainable on the returned node):

| Modifier | Effect |
|---|---|
| `.range(min, max)` | Numeric bounds, checked on write and reported in the schema. |
| `.readOnly()` | Rejects writes. |
| `.persist()` | Included in the persisted snapshot (§12). |
| `.watch()` | Sampled for changes while anyone is subscribed (§11). |
| `.secret()` | A password or a key: written and persisted like any value, but always read as `null` (below). |
| `.doc("text")` | Schema description. Compiled out with `TESSER_NO_DESCRIPTIONS`. |
| `.label()`, `.unit()`, `.step()`, `.ui()` | Presentation metadata for user interfaces (§7.1). Every node type has them, like `.doc()`. |
| `.readAccess(level)`, `.writeAccess(level)` | The access level a client needs to read or write the node and its subtree (§13). Every node type has them. |

**Secrets:** a `.secret()` value renders as `null` in every read, set reply, patch reply and change notification, and the schema marks it `"secret": true`. Only persistence (§12) sees its real value. Clients write it like any value; a user interface shows an empty password field and sends only what the user types.

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
- `.range(min, max)` bounds a numeric argument. It is checked before the action runs, inside patches too, and reported in the schema.
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

### 4.4 Lists

```cpp
struct Remote { std::string host; int port = 80; };
void describe(tesser::Object& o, Remote& r) { o.value("host", r.host); o.value("port", r.port).range(1, 65535); }

std::vector<Remote> remotes;
api.list("remotes", remotes).maxSize(8);                                // uses describe(), like mount()
api.list("levels", levels, [](tesser::Object& o, int& v) { o.value("v", v); });   // or a lambda
```

- **Cost:** a list is one node. Elements are described on demand into temporary objects, built for the request and freed after it, so a list costs nothing at rest however long it is. `T` must be default-constructible.
- **Reading:** `GET /remotes` renders an array of objects; `/remotes/1` and `/remotes/1/port` address elements and fields by index (`/remotes/01` and out-of-range indexes are `not_found`).
- **Writing:** set a field (`PUT /remotes/1/port 8080`) or patch an element (`{"port": 1}`), and actions inside elements work too.
- **Replacing:** setting the list itself to an array replaces it. The array's length is the new size (capped by `maxSize`, default 32). Each array item is a patch for the element at that index; existing elements keep the fields the patch omits, new ones start from defaults. Every item is validated before anything changes. The same applies inside a parent patch.
- **Schema:** `{"type": "list", "maxSize": n, "items": <element schema>}`.
- **Subscriptions and persistence** treat the list as one value: a write anywhere in it marks the list changed, change notifications carry the whole list, and `.persist()` saves it whole. Subscribing below a list is `not_allowed`, since its elements are temporary.

### 4.5 Names and the tree's lifetime

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
- A path that leaves the tree, for example `/lamp/nope/x`, is `not_found`. The error's `path` is the prefix up to the first segment that didn't resolve (`/lamp/nope`).
- No percent-decoding inside the core, since names can't contain characters that need it. HTTP decodes the URL before handing over the path.

### 5.2 Query options (Get)

| Option | Values | Meaning |
|---|---|---|
| `depth` | `0`–`255`, default unlimited | Levels of objects to expand below the target. An object beyond the limit renders as `{}`. `depth=0` on an object gives `{}`. Values are always rendered. |
| `keys` | comma-separated names | Only these direct children of the target. An unknown name is `not_found`. |
| `exclude` | comma-separated names | All direct children except these. |
| `view` | `value` (default), `schema`, `hash` | The representation to return (§7). |
| `remotes` | `true` (default), `false` | Include mirrored remote nodes' copies (§14). |

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

### 6.4 Subscribe and Unsubscribe

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
- **Optional keys:** `writable` (only when true), `min`, `max` (values and action arguments), `persist`, `description`, `arg` and `returns` (actions), `maxLength` (`char[N]`), `enum`, `secret` (§4.1), `access` (the node's own write level when it isn't `public`, §13), and the presentation keys of §7.1.
- `depth`, `keys` and the shape apply to `children` the same way.
- A schema leaf is always an object containing `"type"`. Children are always under `"children"`, so a child named `type` can't be confused with a descriptor.
- Schema output is compiled out with `TESSER_NO_SCHEMA` when flash is tight.

**Hash view:** `GET /lamp?view=hash` → `"1a2b3c4d"`, the FNV-1a hash of the target's schema as 8 hex digits, so a client can keep a cached schema while the hash holds.
- It covers the whole schema the client may read (§13.1): `depth` doesn't apply, and `keys`, `exclude` or a shape are `bad_request`.
- The root's hash for a client with full access is `Api::schemaHash()`, the one advertised in discovery metadata (§10.3).
- Below a remote node, the request is forwarded like any read, so the remote computes its own hash.

### 7.1 Presentation metadata

A schema is enough to build a user interface: types, ranges, enums, writability, actions and events say what controls a node needs, and children keep their declaration order. A few modifiers add what the data model can't say. TesserAPI only stores and emits them; renderers such as TesserUI give them meaning.

```cpp
api.label("Heliostat 3");                                   // the root's label names the device
auto& m = api.object("motor").label("Motor");
m.value("speed", speed).range(0, 100).label("Speed").unit("%").step(5).ui("widget", "knob");
m.action("moveTo", [](float deg) { ... }).range(0, 360).unit("°").ui("confirm", "Move the mirror?");
m.value("pidKp", kp).ui("advanced");
```

```json
"speed": {"type": "integer", "writable": true, "min": 0, "max": 100,
          "label": "Speed", "unit": "%", "step": 5, "ui": {"widget": "knob"}}
```

| Modifier | Schema key | Meaning |
|---|---|---|
| `.label("text")` | `label` | Human-readable name. Without one, renderers derive a name from the node's name. |
| `.unit("text")` | `unit` | Unit of a value, or of an action's argument. |
| `.step(n)` | `step` | Input granularity. A hint: writes are neither rounded nor rejected for it. |
| `.ui("key")` | `ui: {"key": true}` | A flag hint. |
| `.ui("key", "text")`, `.ui("key", n)` | `ui: {"key": ...}` | A string or number hint. Setting a key again replaces it. |

- **Vocabulary:** the keys under `ui` are defined by the renderers (TesserUI's specification), not by TesserAPI. Clients ignore keys they don't know.
- **Cost:** all of it is string literals and one optional allocation per annotated node: 16 bytes, plus 16 bytes per `ui` hint (32-bit targets, before heap overhead). Nodes without presentation metadata pay nothing.
- **Hash:** presentation metadata is part of the schema, so it changes `schemaHash()`.
- `TESSER_NO_UI` compiles it out: the modifiers remain and do nothing.

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
- **Queued mode** (`api.config().queued = true`): transports post requests into a bounded queue (`maxQueued`, default 8, then `busy`), which `api.poll()` drains from `loop()` (or `api.startTask()` from its task). Use it when handlers must run in the application's context, for example when they touch code that isn't thread-safe.
  - **How a request is queued:** the request is copied and its reply detached: message transports answer later, and HTTP parks the connection's handler up to `deferTimeoutMs`. Replies that can't be detached are handled inline.
  - **Deferred actions** work from the queue.
  - **Cleanup:** queued requests of a client that goes away (`dropSubscriber`) are discarded.
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
← {"op": "change", "path": "/sensors", "body": {"temperature": 22.5}}           // notification, no id
← {"op": "event",  "path": "/button/pressed", "body": null}                      // notification, no id
```

- **`id`:** any JSON scalar, echoed verbatim. It may be omitted, in which case the response also has no `id`.
- **`op`:** `get`, `set`, `sub` or `unsub`. The keys `path`, `depth`, `keys`, `exclude`, `view`, `remotes`, `interval`, `events`, `snapshot` and `body` map one-to-one onto `Request`.
- **Unknown envelope keys** are ignored, for forward compatibility.
- **Response:** the response is written as a stream: `{"id":…,"status":"…","body":` followed by the body and then `}`.
  - **Buffered** (NowTP, and every deferred or queued reply): the envelope is sent whole, up to `maxResponse`. A larger body is replaced with a `too_large` error.
  - **Streamed** (serial and WebSocket, for replies sent while the request is handled): `EnvelopeReply::stream()` sends the envelope in 1 KB pieces as it is rendered, so its size isn't limited and memory stays bounded. An error found before the first piece is out still replaces the reply. After that, a failed piece stops the rendering, and the transport ends the message as it can.
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
| `GET /api/<path>?depth=&keys=&exclude=&view=&shape=` | get. A shape comes either in the body or URL-encoded in `shape` (browsers can't send a GET body); both at once is `bad_request`. |
| `PUT`, `PATCH` or `POST /api/<path>` | set. The body is JSON. |
| `OPTIONS` | CORS preflight, when CORS is enabled |

- Responses up to 512 bytes go out with a `Content-Length`. Larger ones are streamed with chunked encoding through the same 512-byte buffer, so there is no limit on response size.
- Request bodies are capped by `maxRequestBody`.
- The status is mapped as in §8.
- The handler runs in the httpd task (inline mode).
- `begin()` starts a dedicated server with wildcard URI matching and an 8 KB stack. `attach()` uses 4 handler slots (5 with CORS, 6 with WebSocket) on an existing server, which must use `httpd_uri_match_wildcard` and should have at least 8 KB of stack. Measured peak use is about 4.9 KB: recursive rendering, newlib's float formatting and lwIP all run on it.
- `enableCors(origin)` adds `Access-Control-Allow-*` headers and answers `OPTIONS` preflights with 204.
- The client's IP address is passed to the core in `Client`.

### 10.3 NowTP

```cpp
tesser::NowTpTransport now(api, transport, /*port*/ 84);
```

- **Requests:** one envelope per NowTP message on the configured port (default 84). `set` requests must arrive in NowTP's reliable mode, which is unicast-only, so a broadcast or unreliable `set` is answered `not_allowed`. A broadcast `get` is answered by every node.
- **Responses:** reliable unicast to the sender's MAC.
- **Response size:** buffered in full (NowTP has no streaming), up to `min(maxResponse, nowtp maxMessageSize)`. Anything larger gets a `too_large` reply. Clients on NowTP should use `depth`, `keys` and shapes.
- **Notifications** use reliable mode, like responses (§11 explains why not latest-only). `sub` requests, like `set`, must arrive reliably.
- **Client identity:** the MAC address. NowTP's "peer lost" discovery event drops that client's subscriptions.
- **Security:** ESP-NOW frames aren't authenticated. Writes over NowTP should be restricted with an allowlist of MACs or encrypted peers (§13).
- **Threading:** NowTP runs its receive callbacks while holding its own lock, so the adapter never handles a request there. It copies each message into a bounded queue (default 8, then `busy`), and a worker task (or `poll()`) handles it under the API lock. The lock order is therefore always API, then NowTP, and replies sent from any API context can't deadlock.
- **Client:** the same endpoint sends requests to other nodes (`get`, `set`, or `endpoint().request()`) and matches responses by `id`, with a timeout per call (default 3 s) and at most 8 calls in flight. Handlers run in the worker task. `peerLost(mac)` fails a lost peer's calls at once.
- **Discovery:** `advertise()` puts `{"tesser": <port>, "schema": "<schemaHash>"}` into NowTP discovery metadata, and `parseAdvertisement()` reads it, so clients can tell TesserAPI nodes apart and keep a cached schema while its hash (`Api::schemaHash()`, FNV-1a of the full schema) is unchanged. `peerEvent()` takes NowTP's peer events, and `mountPeers()` mounts the TesserAPI peers it reports (§14.1).
- **Re-advertising:** after the first `advertise()`, the advertisement follows the tree. A global schema revision (`schemaRevision()`) counts the changes that can alter a schema: a node added, a modifier, a remote node's online state or advertised hash. Temporary objects (list elements and prototypes) and value changes don't count. Once the revision has stayed unchanged for 1 s, the worker recomputes `schemaHash()` and updates the metadata if the hash differs, so a gateway that mounts a burst of peers re-advertises once. The logic is `DatagramEndpoint::autoAdvertise()`, host-tested with the in-memory link.
- **Generic core:** all of this lives in `tesser::DatagramEndpoint`, which is platform-independent and host-tested with an in-memory link. Another datagram transport (UDP, LoRa, ...) only needs a send function and a call to `receive()`.
- **Optional:** the adapter is compiled only when NowTP is available. The core never includes NowTP.

### 10.4 Serial

`tesser::LineTransport` is platform-independent: it is fed bytes, and it calls a write function for each response line. There are thin adapters for an Arduino `Stream` (`StreamTransport`, polled from `loop()`) and an ESP-IDF UART (`UartTransport`, with its own reader task). This is the debugging transport, and the harness for end-to-end tests on hardware.

- Leading control and non-ASCII bytes are skipped, since a UART picks them up while the peer resets.
- When the UART is also the console, log output from other tasks can interleave with a reply on the same line. Use a dedicated UART, or lower the log level once the transport is serving.
- Responses stream (§10.1): one line however long. The output lock is held from the first piece to the end of the line, so notifications and deferred replies from other tasks wait for it.

### 10.5 WebSocket

`http.enableWebSocket("/ws")` serves the §10.1 envelope over WebSocket on the HTTP transport's server. This is where browser subscriptions live.
- Needs `CONFIG_HTTPD_WS_SUPPORT`, which is enabled in Arduino-ESP32 3.x and must be turned on in menuconfig on ESP-IDF.
- One text message per envelope. Each connection is a client (`Subscriber`).
- **Large replies** stream (§10.1) as a fragmented message: a text frame, then continuation frames, the last one final. Browsers reassemble it into one message.
- **Ordering:** notifications, deferred replies and replies that fit in one piece are written from the httpd task through `httpd_queue_work`, so writes from other tasks never interleave. A fragmented reply is written directly while the httpd task handles its request, so no queued frame can come in between, but it can overtake notifications queued just before the request.
- **Cleanup:** when `begin()` owns the server, `close_fn` drops a closed connection's subscriptions at once. With `attach()`, a closed connection is noticed on the next send to it.
- **Handshake:** a connection's authentication (§13) is decided from its handshake. ESP-IDF 6.1 stopped calling a WebSocket handler with the handshake's GET, so `HttpServer` reads the handshake in the pre-handshake callback, which needs `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`. The component's Kconfig selects it; older ESP-IDF versions and Arduino-ESP32 3.x still call the handler. When neither path saw the handshake, the server logs a warning once and the connection is unauthenticated.

## 11. Change tracking and subscriptions

**Tracking changes:**
- A global 16-bit generation counter is stamped on a node when it changes (compared with wrap-around; 0 means "never changed"). It costs no extra memory: the field fits in the node's padding.
- A node is marked changed:
  - automatically, when a set or patch goes through the API
  - explicitly, with `node.changed()` or `api.changed("/path")`. On an object, `changed()` marks every value below it. Both are thread-safe and cheap.
  - by sampling, with `.watch()`: while anyone is subscribed, `Api::poll()` renders each watched value every `Config::watchIntervalMs` (200 ms) and compares a 32-bit hash, kept in the node's metadata. A new subscription first samples the watched values below its node, so its snapshot is the baseline: a change right after subscribing is notified on the next sample.
- Bound plain variables can't announce their own changes, so either `changed()` or `watch()` is needed for push updates.

**Delivering:** `Api::poll()` sends pending change notifications. Call it from `loop()`, or let `api.startTask(periodMs)` (ESP, default 20 ms) do it. It does nothing while nobody is subscribed.

**Subscribing** (message transports: serial, NowTP, WebSocket; plain HTTP answers `not_allowed`):

```json
→ {"id": 3, "op": "sub", "path": "/sensors", "keys": "temperature,humidity", "interval": 500}
← {"id": 3, "status": "ok", "body": {"temperature": 22.5, "humidity": 48}}    // initial snapshot
← {"op": "change", "path": "/sensors", "body": {"temperature": 22.7}}         // later, only changed leaves
→ {"id": 4, "op": "unsub", "path": "/sensors"}
← {"id": 4, "status": "ok", "body": 1}                                       // subscriptions removed
```

**Options:**

| Option | Meaning |
|---|---|
| `keys`, `exclude`, `depth` | Same as for get, applied to both the snapshot and the notifications. Shapes are not supported. |
| `interval` | At most one change notification per `interval` ms (default 0: every `poll()`). Intermediate states are coalesced. |
| `events` | Also deliver events under the path (default `true`). |
| `snapshot` | Reply with the current state (default `true`). With `false` the reply body is `null`, for clients that read large subtrees in pieces with gets. |

**Notifications:**
- A change notification is a **sparse merge patch** rooted at the subscription path, holding only the values changed since the last notification. Clients apply it exactly like a `set` body, so one format serves writes, notifications and persistence. For a subscription to a single value, the body is the value.
- Change notifications are deltas, so they are sent **reliably**. NowTP's latest-only mode would lose a superseded delta's values.
- **Failure:** if a notification can't be queued, the subscription isn't advanced. The next one carries the missed changes merged with newer ones, plus `"overflow": true`. The same flag follows when a transport reports a queued notification lost, and tells the client to re-read.

**Events:** an event (`o.event("pressed")`, then `node.emit(value)`) is never coalesced. It goes out at once, reliably, to every subscriber whose path covers it and that didn't ask for `"events": false`. It reaches a client once even when several of its subscriptions match:

```json
← {"op": "event", "path": "/button/pressed", "body": 3}
```

**Limits and cleanup:**
- At most `maxSubscriptions` (16) subscriptions in total, and `maxSubscriptionsPerClient` (4) per client. Beyond that, `busy`.
- Notifications go through each transport's own bounded queue: NowTP's transmit queue, `httpd_queue_work` for WebSocket. A slow client never causes unbounded allocation.
- Subscriptions die with their client: WebSocket close, NowTP peer lost, a `LineTransport` destroyed, or an explicit `unsub`.

**Client side:** `DatagramEndpoint` (NowTP) sends `sub` requests like any other, with `interval` and `events` in its `Query`. Incoming notifications go to `onNotification(handler)`; they are never answered, so two nodes subscribed to each other can't ping-pong.

## 12. Persistence

```cpp
tesser::NvsStorage storage;               // or FileStorage("/littlefs/state")
api.object("config").persist();           // everything below it
api.value("calibration", cal).readOnly().persist();
api.persistence(storage, 2000);           // debounce: save 2 s after the last change
api.load();                               // once the tree is declared
```

- **What is saved:** values, custom nodes and lists marked `.persist()`, or below an object marked `.persist()`, in the same sparse format as patches and change notifications: the value view filtered to persisted nodes, secrets included.
- **Records:** the state is stored as one record per top-level node that holds persisted state, under the node's name: `config` holds `{"brightness": 7, "tuning": {"gain": 2}}`, and a persisted value at the root (`calibration`) holds just its value. A change rewrites only the records it touched, so a setting changed often doesn't rewrite the rest of the state. `api.persistedState()` returns the whole state as one document, the records under their names.
- **Storage backends:** `tesser::Storage` has three methods: `load(key, out)`, `save(key, data)` and `erase()`, which removes every record (a factory reset; the running state keeps its values until the next boot).
  - `NvsStorage(namespace)`: one NVS blob per record, in its own namespace (default `tesser`). NVS keys hold 15 characters, so longer names keep their first 7 and get a hash. The application initializes NVS.
  - `FileStorage(dir)`: one file per record, `<dir>/<key>.json`, through stdio, so any VFS mount on ESP and the host. The directory is created on the first save. It writes to a temporary file and renames it, so a power cut leaves either the old or the new record.
  - `MemoryStorage`: tests.
- **Saving:** `Api::poll()` notices changes to persisted values through their generations, at most every 50 ms. Once `debounceMs` has passed without a new change, it writes the records whose values changed since the last save. Changes to values that aren't persisted never cause a save. `api.save()` writes every record immediately.
- **Loading** is lenient: the stored state goes through each value's normal validation, but whatever no longer fits is skipped with a warning and the value keeps its default. That covers renamed or removed keys, values now out of range, a key that became an object, and a value no longer persisted. So schema changes never break a boot.
- **Read-only values:** values marked `readOnly()` for the API (calibration data written by an action, a boot counter) are restored too. Getter-only values can't be.
- **Restoring** a backup: `api.restore(json, &skipped)` applies a document like `persistedState()`'s, leniently like a boot load, so a backup from an older firmware restores whatever still fits. The paths it skipped are listed in `skipped`. Unlike a boot load, what it applies is marked changed, so subscribers hear about it, and every record is saved at once. It returns `bad_request` if the document doesn't parse, and `internal` if saving failed.

## 13. Security

### 13.1 Access levels

Every client has an access level, and every node may require one:

```cpp
api.object("net").writeAccess(tesser::Access::Admin);      // anyone reads, admins write
api.object("users").readAccess(tesser::Access::Admin);     // only admins read (and write)
api.authenticate([](std::string_view token) { return users.check(token); });   // token → level
```

- **Levels** are ordered: `public` < `user` < `admin` (`tesser::Access`).
- **A node's requirement** is the highest of its own and its ancestors'. Writing needs at least the read level: `readAccess()` raises the write level with it.
- **Reads** (get, subscribe) of a node the client can't read are `unauthorized`. Inside a read of a parent, such nodes are **left out**: of values, of schemas, of `keys` and shapes (naming one is `unauthorized`), of change notifications, and of events. A client never learns what it can't read.
- **Writes** (set, actions, patches) need the write level of every node they touch; a patch touching one node the client can't write is refused whole, before anything is applied, like any validation error.
- **Schema:** a node whose own write level isn't `public` says so with `"access": "user"` or `"admin"`, so a user interface can disable what its client can't change. Renderers inherit it down the subtree. Schemas differ by level (hidden nodes), so `schemaHash()`, computed with full access, is the admin's.
- **Cost:** two bytes in the node's optional metadata, only for nodes that declare a level. Without any, everything is `public` and nothing is filtered.

**A client's level** (`Client::level()`) comes from its transport:
- A transport that knows levels sets `Client::access`.
- A transport that only knows "authenticated" (`Client::authenticated`, below) gives an authenticated client `admin` and any other `public`. That is the case of serial lines, trusted NowTP peers and `LocalClient`.
- HTTP and WebSocket clients present a token. The transport's own token (`HttpServer::setToken`) gives `admin`. Any other token, or none (an empty token), goes to the API's token check, `api.authenticate(fn)`, whose answer is the client's level; without a check, `public`. A WebSocket connection's level is decided once, from its handshake.
- **Gateways:** a gateway checks levels of its own clients before forwarding. The remote node sees the gateway as its client (a trusted peer is `admin`), not the browser behind it.

### 13.2 Authorizer and authentication

- **Authorizer:** `api.authorize(fn)`, where `fn(const Client&, Op, const Node&) -> bool`, is consulted for the target of every get, set and subscribe, and for every node a patch touches (during validation, so a refused patch changes nothing). A refusal is `unauthorized` (HTTP 403).
- **Default:** without an authorizer, everything is allowed. Access levels (§13.1) apply independently: a request must pass both.
- **Ready-made authorizers:** `authorizers::readOnlyUnlessAuthenticated()` (anyone reads and subscribes; writes and actions need authentication) and `authorizers::authenticatedOnly()`.
- **What makes a client authenticated** (`Client::authenticated`) is the transport's business:

| Transport | Authenticated when |
|---|---|
| HTTP | `HttpServer::setToken(token)` is set and the request carries `Authorization: Bearer <token>` or `?token=<token>` (`admin`), or the API's token check gives its token a level above `public` (§13.1) |
| WebSocket | Same checks, on the handshake |
| NowTP | The sender is in `NowTpTransport::trustPeers({...})`, or `DatagramEndpoint::trust(fn)` accepts it. ESP-NOW frames carry no proof of origin, so this trusts MAC addresses; combine it with encrypted NowTP peers when that matters. |
| Serial | Always (physical access), unless `LineTransport::setAuthenticated(false)` |

- `Client` also carries the transport kind and the address (IP or MAC), for authorizers that need more.
- Hiding nodes from the schema is not access control: the authorizer runs on every access.
- TLS is the application's choice of server.

## 14. Remote trees and the client

**Client.** `DatagramEndpoint` (and `NowTpTransport` on top of it) sends requests to other nodes and matches responses by `id`:

```cpp
nowApi.get(mac, "/lamp", [](tesser::Status s, JsonVariantConst body) { ... });
nowApi.endpoint().request(addr, tesser::Op::Subscribe, "/sensors", "", done, query);
nowApi.endpoint().onNotification([](const tesser::PeerAddress& from, JsonObjectConst msg) { ... });
```

**Remote mount (gateway).** A remote node grafts another node's tree into the local one:

```cpp
nowApi.remote(api, "kitchen", kitchenMac);              // or api.remote("kitchen", endpoint, address, "/")
nowApi.remote(api, "light", kitchenMac, "/lamp").mirror(200);
```

- **Forwarding:** get and set on `/kitchen/...` are forwarded with the path rewritten, and complete through a deferred reply (`maxPending` applies). Error paths in the remote's reply are mapped back under the local path (`/kitchen/nope`). A silent node answers `timeout` after the node's `timeout()` (default: the endpoint's 3 s). Transports that can't wait get `not_allowed`.
- **Writes** must target the remote subtree: a patch of the parent can't include a remote node.
- **Without a mirror**, a remote node renders as `null` in a parent read, and as `{"type": "remote"}` in the schema; reads below it are always forwarded live, and subscriptions are forwarded (below).
- **`.mirror(intervalMs, refreshMs)`** keeps a local copy through a subscription. The copy is renewed every `refreshMs`, in case the remote node restarted.
  - Parent reads include the copy, respecting `depth`.
  - Local subscribers get the remote's changes as changes of the remote node, and its events under the local path (`/kitchen/button/pressed`).
  - Subscribing to a mirrored remote node itself is a local subscription to the copy.
- **Forwarded subscriptions:** a subscription below a remote node, or to an unmirrored remote node itself, is forwarded. This is how a browser on the gateway follows a NowTP node's state without a mirror.
  - **Shared upstream:** local subscribers of the same path share one upstream subscription to that path, which holds the whole subtree (no keys, unlimited depth). The gateway keeps its state as a copy, merged with each notification.
  - **Snapshots:** the first subscriber's reply waits for the upstream snapshot (a deferred reply, so `maxPending` applies; transports that can't wait get `not_allowed`). Later ones are answered from the copy at once.
  - **Filtering:** each local subscription's `keys`, `exclude` and `depth` are applied by the gateway, to its snapshot and to every relayed change. Unknown keys are not reported, since the gateway doesn't know the remote's tree. `interval` and `remotes` are taken from the first subscriber; changes are relayed as they arrive.
  - **Events** under an upstream subscription's path come out under the local path, like a mirror's, to every local subscriber they concern.
  - **Lifetime:** the upstream subscription ends with its last local subscriber (unsubscribe, or the client going away). It is renewed every `refreshMs` (default 30 s; every 2 s after a failed renewal) in case the remote restarted. If the renewed state differs from the copy, every subscriber gets the whole state as one change.
  - **Limit:** two remote nodes of one gateway that reach the same remote path through the same endpoint share that path's subscription on the remote, so unsubscribing one ends both. Mount a peer once.
- **No loops:** the `remotes: false` request option leaves mirrored remote nodes out of a read or subscription. Mirrors always subscribe with it, so a mirrored copy never contains copies of copies, and two gateways mounting each other stay flat. Requests below a remote node are forwarded live and may cross several hops.
- **Lifetime:** the API must outlive its transports. A remote node and its endpoint may be destroyed in either order: a destroyed endpoint detaches its remote nodes.
- **Channel:** on a gateway, Wi-Fi station mode and ESP-NOW share one channel, so every node must be on the router's channel.

### 14.1 Discovered peers

A gateway can mount the nodes it discovers instead of declaring them:

```cpp
radio.onPeerEvent([](nowtp::PeerEvent e, const nowtp::PeerInfo& p) { nowApi.peerEvent(e, p); });
nowApi.mountPeers(api.object("peers"));   // also mounts the peers already discovered
nowApi.advertise();                       // on every node, once its tree is complete; kept current after that
```

- **Mounting:** each peer that advertises TesserAPI on the transport's port becomes a remote node under the parent, named after its NowTP discovery name. Characters a name can't hold become `-`. If the name is empty or taken, the last three address bytes are appended (`tesser-lamp-75f303`). `mountPeers(parent, intervalMs)` also mirrors them. A peer already mounted under the parent isn't mounted again. Remote nodes of the peer elsewhere in the tree (declared ones) don't prevent it, but they share its online state and schema hash, including remote nodes declared after the announcement.
- **Nodes are never removed.** A lost peer stays mounted and shows offline; it comes back online as soon as it is heard from or announces itself again.
- **Remote node stub:** the parent's schema shows each remote node's state. `"online": false` appears while the peer is lost, and `"schema"` is the hash the peer advertises, so a client can cache the peer's own schema under it:

  ```json
  GET /peers?view=schema&depth=1
  {"type": "object", "children": {
     "tesser-lamp": {"type": "remote", "schema": "1a2b3c4d"},
     "heliostat-3": {"type": "remote", "online": false, "schema": "99f0e1aa"}}}
  ```

- **Watching the list:** mounting a peer, a change of its online state and a new advertised hash all mark the remote node changed. A subscriber of the parent (depth 1) gets `{"heliostat-3": null}` for an unmirrored node, which is its cue to re-read the parent's schema.
- **Generic core:** `DatagramEndpoint::mountPeers()` and `peerSeen(address, name, schemaHash)` do the work, host-tested with the in-memory link. The NowTP adapter only parses advertisements and forwards peer events. Online state comes from the endpoint: `forgetPeer()` marks a peer's remote nodes offline, and any message from the peer marks them online.

### 14.2 ApiClient

`tesser::ApiClient` is one interface for talking to an API, wherever it is. A user interface, or any other client, is written once against it:

```cpp
tesser::LocalClient local(api);                     // the API on this chip
tesser::PeerClient remote(nowApi.endpoint(), mac);  // another node, over NowTP

client.get("/lamp", [](tesser::Status s, JsonVariantConst body) { ... });
client.set("/lamp/brightness", "200");
client.subscribe("/sensors", onSnapshot, query);
client.onNotification([](JsonObjectConst envelope) { ... });   // "change" and "event"
client.process();                                   // from the UI loop: runs the callbacks
```

- **Callbacks run in `process()`,** in the caller's task, in arrival order. Responses and notifications are queued until then, so a single-threaded UI loop (LVGL) needs no locking. Handlers may send new requests.
- **Bounded:** at most `maxQueued` notifications (default 16) wait between two `process()` calls. Further ones are dropped, and the server marks the next one `"overflow": true`, which tells the client to re-read. Responses are always queued.
- **`LocalClient`** goes through `Api::handle()` like a transport, as a `Subscriber`, so subscriptions, deferred actions, queued mode and remote nodes behave as they do for remote clients. Its requests count as authenticated unless `setAuthenticated(false)`. Destroying it drops its subscriptions.
- **`PeerClient`** sends through a `DatagramEndpoint` and gets the notifications its peer sends to that endpoint. These are per peer, not per client: two clients of the same peer on one endpoint both see both clients' notifications. Destroying it unsubscribes what it subscribed to.
- **Bodies** are JSON text in, `JsonVariantConst` out. A response that arrives after its client is gone is dropped.

## 15. Memory budget

Targets to be measured on a classic ESP32 and a C3, and enforced in CI:

| Item | Target |
|---|---|
| Flash, core + HTTP transport | ≤ 40 KB above the application's baseline, ArduinoJson included (measured below: 45.5 KB) |
| RAM per value leaf | ≤ 32 B with a bound variable, ≤ 56 B with a getter and setter, + 28 B with a range or description |
| RAM per object node | ≤ 32 B |
| Request handling | Request body document (bounded by `maxRequestBody`) + 512 B streaming buffer (HTTP) or 1 KB (serial, WebSocket), or the response buffer (NowTP and deferred replies, bounded by `maxResponse`) |
| Stack | Rendering depth bounded by `maxDepth` (default 16) |

Measured on a classic ESP32 (ESP-IDF 6.1, heap overhead included, `test/hardware`):

| Item | Measured |
|---|---|
| `Api` object | 324 B (two statically allocated FreeRTOS mutexes make most of it) |
| Value bound to a variable | 28 B |
| Value with getter and setter (small captures) | 56 B |
| Object | 32 B |
| Value with a range | 64 B (28 B node + 36 B metadata) |
| List of any length | one node; elements are built per request |
| Full GET of a 13-leaf tree (227 B response) | 0.43 ms |
| Envelope parse + keyed GET + reply | 0.9 ms |
| Peak stack of an HTTP request (schema, patch, shape) | about 4.9 KB |
| Peak stack of a serial (UART task) request | about 3.5 KB |
| Serial round trip at 115200 baud | 13 ms (mostly wire time) |
| HTTP GET over Wi-Fi, new TCP connection each time | 55–85 ms median |
| NowTP request and response between two boards | 13–14 ms median, fragmented 1.5 KB responses included |
| GET through a gateway (HTTP, then NowTP to the remote node) | 85 ms median |

**Flash:** `examples/idf/http_api` against its `FOOTPRINT_BASELINE` build (the same app on bare `esp_http_server`):
- **Total:** 45.5 KB for the core, the HTTP transport and ArduinoJson. With `TESSER_NO_SCHEMA`, `TESSER_NO_MESSAGES` and `TESSER_NO_DESCRIPTIONS` it is 43.8 KB.
- **Against the target:** the 40 KB target was set before subscriptions, events, lists and authorization joined the core. 34.6 KB was measured for core + HTTP at phase 2.
- **Linked only when used:** remote nodes (and the datagram client), persistence, WebSocket and NowTP. Remote forwarding is virtual and persistence is a hook installed by `persistence()`, so applications that don't use them don't link them.

**No leaks:** running the whole host test suite a second time on the chip moves the heap by exactly the two nodes the duplicate-name test deliberately leaves unlinked.

Configuration (`tesser::Config`, at runtime):

| Option | Default |
|---|---|
| `maxRequestBody` | 4096 |
| `maxResponse` (buffered replies: NowTP, deferred and queued replies) | 8192 |
| `maxDepth` | 16 |
| `deferTimeoutMs` | 10000 |
| `maxPending` | 4 |

Compile-time switches: `TESSER_NO_SCHEMA`, `TESSER_NO_DESCRIPTIONS`, `TESSER_NO_MESSAGES`, `TESSER_NO_UI`.

## 16. Packaging

The layout follows NowTP:
- `CMakeLists.txt` is an ESP-IDF component under `idf.py`, and a host library plus tests otherwise.
- `idf_component.yml` depends on `bblanchon/arduinojson` as a public requirement (`require: public`), since TesserAPI's headers include ArduinoJson's: any component that requires TesserAPI can include `TesserAPI.h`.
- The component's `Kconfig` selects `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT` when WebSocket support is on (§10.5).
- `library.properties` and `library.json` for Arduino and PlatformIO.

Optional transports (NowTP) compile only when their dependency is present: `__has_include(<NowTP.h>)` on Arduino and PlatformIO. On ESP-IDF, the component looks for a `nowtp` component among the build components and links it, since requirements are resolved before that list is known.

## 17. Testing

1. **Host tests** (CMake + ctest, ASan and UBSan, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` on the library). They cover the core, the envelope codec and the line transport, and run against NowTP's simulated network for the NowTP adapter.
2. **On-target tests** (`test/hardware`, ESP-IDF): the same core cases on the chip, plus heap and stack measurements.
3. **End-to-end on hardware:** `test/hardware` serves a demo API over UART, HTTP and WebSocket (with Wi-Fi credentials from the gitignored `test/secrets.h`), and NowTP. It drives subscriptions, events, access control, persistence and the gateway:
   - `test/e2e_serial.py`, `test/e2e_http.py`, `test/e2e_ws.py`
   - `test/e2e_nowtp.py` and `test/e2e_gateway.py`, on two boards
   - `test/e2e_persist.py`, which reboots the board through the serial port
4. **Transport conformance:** `test/conformance.py` sends the same request vectors through serial, HTTP, WebSocket and NowTP (both directions), and every transport must give the same results.
5. **CI:** host tests, plus ESP-IDF 5.1 / 5.4 / 5.5 / latest × ESP32 / ESP32-C3 (test firmware and examples), plus Arduino-ESP32 3.x × ESP32 / ESP32-C3 (example sketches).

## 18. Roadmap

| Phase | Content | Status |
|---|---|---|
| 1 | Core: tree, values, actions, custom nodes, get / set / patch / shape / schema, streaming writer, envelope, line transport, host + on-target tests | done |
| 2 | HTTP and NowTP transports, conformance tests, footprint measurements | done |
| 3 | Change tracking, subscriptions, events, WebSocket | done |
| 4 | Persistence, authorizer, trusted NowTP peers, bearer tokens | done |
| 5 | Client, remote mount (gateway), queued execution mode, lists of objects | done |
| 6 | Groundwork for TesserUI: presentation metadata, forwarded subscriptions, discovered peers, `ApiClient` | done |
| 7 | Groundwork for [TesserKIT](https://github.com/leokeba/TesserKIT): access levels and token checks, secret values; typed object arguments, keyed lists, scalar arrays, file nodes, streamed envelope replies, `view=hash`, re-advertisement, `Storage::erase()` and `Api::restore()` | in progress: access levels, secrets, streamed replies, `view=hash`, re-advertisement, per-record persistence, `Storage::erase()` and `Api::restore()` done |

## 19. Decisions on former open questions

- **Lists of described objects:** implemented (§4.4).
- **Enum values:** strings, with the allowed values in the schema (§4.1).
- **Binary envelope for NowTP:** not needed. JSON requests fit in one 250-byte frame, and round trips measure 13–14 ms, dominated by the radio rather than by parsing. Revisit only if a measured workload says otherwise.
- **Schema hash in NowTP discovery metadata:** implemented (`advertise()`, §10.3).

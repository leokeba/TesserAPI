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
- A general query language: no JSONPath, predicates or wildcards, except one: a `*` path segment fans a Set out to every child (§6.5).
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

**Enums** are exposed as strings: `o.value("mode", mode, {"off", "auto", "manual"})` names the enumerators in order of their underlying values. Writes accept only those names, and the schema lists them under `"enum"`. An enumerator without a name reads as its number. Through functions, `o.value("state", [&] { return axis.state(); }, {"disabled", "starting", "running"})` is read-only, and `o.value("mode", getter, setter, {...})` writable; the setter takes the enum and returns `void`, `bool`, `Status` or `Check`, like other setters. Use them for state another task owns under its own lock.

**Modifiers** (chainable on the returned node):

| Modifier | Effect |
|---|---|
| `.range(min, max)` | Numeric bounds, checked on write and reported in the schema. |
| `.readOnly()` | Rejects writes. |
| `.persist()` | Included in the persisted snapshot (§12). |
| `.watch()` | Sampled for changes while anyone is subscribed (§11). |
| `.secret()` | A password or a key: written and persisted like any value, but always read as `null` (below). |
| `.validate(f)` | The application's own check, run with the type and range checks before anything is applied (below). Not reported in the schema. |
| `.doc("text")` | Schema description. Compiled out with `TESSER_NO_DESCRIPTIONS`. |
| `.label()`, `.unit()`, `.step()`, `.ui()` | Presentation metadata for user interfaces (§7.1). Every node type has them, like `.doc()`. |
| `.readAccess(level)`, `.writeAccess(level)` | The access level a client needs to read or write the node and its subtree (§13). Every node type has them. |

**Validators:** `f` takes the value as the node's type (or any type of the same kind: `std::string` or `const char*` for a string) or as a `JsonVariantConst`, and returns a `tesser::Check`, a `bool` (`false` is `invalid_value`, "rejected") or a `tesser::Status`. A validator whose type is of another kind is a declaration error and isn't installed.

```cpp
o.value("time", alarm.time).validate([](const std::string& t) { return isClockTime(t); });
o.value("password", [] { return std::string(); }, [&](const std::string& p) { hash(p); })
    .secret()
    .validate([](JsonVariantConst v) {
        size_t n = v.as<JsonString>().size();
        return n == 0 || (n >= 8 && n <= 63) ? Check::ok() : Check::fail(Status::InvalidValue, "8 to 63 characters");
    });
```

It runs wherever values are checked: in pass 1 of a patch (§6.3), so a refused value leaves the whole write unapplied; for every element of a list replacement and every field of an object argument; and in `load()` and `restore()`, which skip a refused value like any invalid one (§12). Prefer it to a setter that returns `false`, which can only refuse at apply time, after earlier writes. On a list element it may capture the element by reference: it runs while the temporary element exists. A setter stays for side effects (hashing, applying to hardware).

**Secrets:** a `.secret()` value renders as `null` in every read, set reply, patch reply and change notification, and the schema marks it `"secret": true`. Only persistence (§12) sees its real value. Clients write it like any value; a user interface shows an empty password field and sends only what the user types.

Integers are range-checked against `T` before any user range: writing `300` to a `uint8_t` is `invalid_value`, never a wrap-around. Integers are accepted for floating-point values. Nothing else is coerced: a string `"12"` is not a number.

### 4.2 Actions

```cpp
o.action("reboot", [] { esp_restart(); });                       // no arguments
o.action("moveTo", [](float deg) { motor.moveTo(deg); });        // one typed argument
o.action("count",  [] { return counter; });                      // returns a value
o.action("addRemote", [](JsonVariantConst v) { ... return tesser::Status::Ok; });  // raw argument
o.action("jog", [] {                                             // refuses with a message
    return claimed ? tesser::Check::fail(tesser::Status::Busy, "driven by tracking") : tesser::Check::ok();
});
o.action("calibrate", [](tesser::Call& call) {                   // deferred completion
    auto pending = call.defer();
    startCalibration([pending](float r) mutable { pending.reply(r); });
});
```

- The argument is the request body:
  - no argument: the body is ignored (it should be absent or `null`)
  - one typed argument: the body must convert as described for values
  - an object argument (below): the body is an object of its fields
  - `JsonVariantConst`: the raw body
- An action may return `void`, a supported value type, or `tesser::Status`.
- `.range(min, max)` bounds a numeric argument. It is checked before the action runs, inside patches too, and reported in the schema.

**Object arguments.** An action whose argument is a type with a `describe()` (member or free function, as for `mount()`, §4.3) takes an object of its fields:

```cpp
struct Target { std::string name = "New target"; float az = 180, el = 30; };
void describe(tesser::Object& o, Target& t) {
    o.value("name", t.name);
    o.value("az", t.az).range(0, 360);
    o.value("el", t.el).range(-90, 90);
}
o.action("add", [](const Target& t) { targets.add(t); });   // POST /helio/add {"name": "Sun", "az": 120}
```

- **Filling:** for each call, a default `T` is described into a temporary object, and the body is validated and applied to it like a patch (§6.3), then the action gets the `T`. Fields the body omits keep their defaults; `null` or no body gives the default `T`. So the fields' ranges, lengths, enums and nested objects all apply, and an error names the field: `{"error": "invalid_value", "path": "/helio/add/az"}`.
- **In a patch,** the argument is validated with the rest in pass 1, so a bad field leaves the whole patch unapplied.
- **Schema:** `"arg": "object"`, and `"params"` holds the object schema of the fields, for forms.
- **Cost:** nothing at rest beyond the action node: the object is built per call.

**Typed deferred actions.** An action taking an argument (a value type or an object argument) and then a `tesser::Call&` is deferred like a `Call&`-only one, with its argument converted and validated first:

```cpp
o.action("moveTo", [](float deg, tesser::Call& call) {
    auto pending = call.defer();
    motor.moveTo(deg, [pending](bool ok) mutable { ok ? pending.reply() : pending.fail(tesser::Status::Internal); });
}).range(0, 360);
```

The schema reports `arg` (and `params`) with `"deferred": true`. A deferred action that takes only `Call&` reports `"arg": "any"`, since it may read `call.arg()`.

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
- **Schema:** `{"type": "list", "writable": true, "maxSize": n, "items": <element schema>}`.
- **Read-only:** `api.list("nearby", peers).readOnly()` is a list the application fills (scan results, peers nearby). Replacing it, in a parent patch too, and writing its elements or their values are `read_only`; actions in its elements still run, and their file nodes still transfer (§4.6). Its schema has no `writable`, nor do its elements' values, so a user interface offers no add or remove controls. It is persisted and restored only with `.persist()`, like any list.
- **Subscriptions and persistence** treat the list as one value: a write anywhere in it marks the list changed, change notifications carry the whole list, and `.persist()` saves it whole. Subscribing below a list is `not_allowed`, since its elements are temporary.

**Keyed lists.** `.key("name")` names each element by one of its fields, a string value, so elements are addressed by key instead of by index:

```cpp
api.list("sources", sources).key("name");       // GET /sources/Sun/az, PUT /sources/Sun/az 120
api.list("networks", networks).key("ssid");     // whose passwords are secret()
```

- **The key** must be a non-empty string without `/`, unique in the list. A key field that isn't a string value of the element is a declaration error (`api.errors()`), and the list stays indexed.
- **Reading** is unchanged: the list renders as an array, in its order. `/sources/0` is `not_found` unless an element's key is `0`.
- **Renaming** is a write to the key field through the element, `PUT /sources/Sun/name "Sol"` or an element patch: the element keeps its other fields. The new key must be valid and not used by another element.
- **Replacing** matches items to elements by key, not by index: each item patches the element with its key (keeping the fields it omits), elements whose key no item has are removed, and items with new keys start from defaults. The array's order becomes the list's. Every item needs a valid key, once. So a client can rewrite the whole list without reading back fields it can't see, like secrets: an edited network keeps its password. A rename through the array isn't one: the new key is a new element.
- **Errors** name elements by key (`/sources/Sun/az`), or by index when the item has no valid key (`/sources/1/name`).
- **Loading** matches stored items by key too; items without a valid key are skipped.
- **Schema:** `"key": "name"` in the list's schema.
- **Cost:** a lookup by key describes each element once to read its key, so keyed lists suit tens of elements, like indexed ones.

### 4.5 Arrays

```cpp
float offsets[128];
std::vector<uint8_t> days;
api.array("offsets", offsets).range(-5, 5).persist();   // fixed size: C array or std::array
api.array("days", days).range(0, 6).maxSize(7);         // resizable: std::vector
```

- **Elements** are any value type except `bool` in a `std::vector` (which has no element references). A const container is read-only, and so is `.readOnly()`.
- **Reading:** `GET /offsets` renders a JSON array, and `/offsets/3` one element (by index, as for lists).
- **Writing:** `PUT /offsets/3 0.25` writes one element; setting the array (or including it in a patch) replaces its contents. A fixed-size array takes exactly its length; a `std::vector` any length up to `maxSize` (default 64). Every element is checked against the element type and `.range()` before anything changes, and an error names it (`/offsets/7`).
- **Schema:** `{"type": "array", "writable": true, "maxSize": 128, "fixed": true, "items": {"type": "number", "min": -5, "max": 5}}`. `fixed` appears for fixed-size arrays, whose `maxSize` is their size. An element's schema is a value's.
- **Subscriptions and persistence** treat the array as one value, like a list.
- **Cost:** one node; element nodes are built only for requests that address one.

### 4.6 Files

A file node streams binary content to and from the device: firmware images, backups, core dumps, calibration exports. Its bytes never go through JSON or the request body limit.

```cpp
auto& fw = api.object("sys").file("firmware").maxSize(0x1E0000).accept(".bin").writeAccess(tesser::Access::Admin);
fw.upload([](tesser::FileRequest& r) -> std::unique_ptr<tesser::FileSink> {
    if (!ota.begin(r.size)) { r.refuse(tesser::Status::Busy, "update in progress"); return nullptr; }
    return std::unique_ptr<tesser::FileSink>(new OtaSink());      // write() each chunk, finish() at the end
});
api.object("sys").file("coredump").download([](tesser::FileRequest& r) {   // a FileSource: size(), read()
    r.filename = "core.elf";
    return std::unique_ptr<tesser::FileSource>(new CoreDumpSource());
});
api.object("sys").file("backup").contentType("application/json").readAccess(tesser::Access::Admin).text(
    [] { return api.persistedState(); },                                      // download
    [](std::string_view text, tesser::JsonWriter& reply) { ... api.restore(text, &skipped) ... });  // upload
```

- **Handlers:** `upload(open)` and `download(open)` take a function called when a transfer starts, under the API lock, with a `FileRequest`: the client, the announced size (uploads), and a `filename`. A download may set it (default: the node's name). An upload gets the name the client gave (over HTTP, `?filename=`), URL-decoded but otherwise unchecked, or empty if none: the handler validates it. It returns the transfer's `FileSink` or `FileSource`, or null after `request.refuse(status, message)`.
- **Uploads:** the sink's `write(data, len)` gets the bytes in order, then `finish(reply)` once all of them arrived; it may write the reply body (by default `null`). A failing write ends the upload with its error. A sink destroyed without `finish()` was cut short (a failed write, the client gone, fewer bytes than announced): roll back in its destructor.
- **Downloads:** the source's `size()` (or -1 if unknown) and `read(buf, cap)`, which returns the bytes read, 0 at the end, or -1 on error.
- **Outside the lock:** writes, reads and `finish()` run without the API lock, in the transport's task, so a long upload doesn't hold up other requests. A sink that changes bound state takes the lock itself (`tesser::Lock`). Only one transfer per path runs at a time; another one is `busy`.
- **`text(get, set)`** covers small text files: a download is `get()`, and an upload is collected in memory (up to `maxSize`, 16 KB unless set) and handed to `set`, which may write the reply. The size announced is taken from the byte accountant when the upload opens, which is refused `busy` without room for it (§15).
- **Limits:** `maxSize(bytes)` caps uploads (0, the default, means no limit), checked against the announced size before the upload starts. `contentType()` is the downloads' MIME type (default `application/octet-stream`), and `accept()` the file types an upload form should offer, as in HTML (`.bin`).
- **Access:** access levels (§13.1) and the authorizer apply: a download is a read, an upload a write.
- **In the tree:** a file node has no value, so it is left out of value reads and patches, like an action. A JSON request on it is `not_allowed`: get, set, a patch that names it, a subscription. Its schema is `{"type": "file", "readable": true, "writable": true, "maxSize": 1966080, "contentType": "application/octet-stream", "accept": ".bin"}`.
- **In list elements:** a file node in a list or array element transfers like any other (`GET /api/files/entries/notes.txt/content`). The transfer resolves the element as a request does, and the handler runs under the lock while the element exists, so it may read it (capture its key by value). The element is freed before the bytes move, so the returned sink or source, and a `text()` setter, must not reference it, as with validators on list elements. Elements of a read-only list keep their transfers: a download is a read, and an upload is the handler's to accept, not a write to the list. A file browser is a read-only keyed list of entries with a `content` file node each, plus a file node elsewhere that takes new files by `filename`:

  ```cpp
  api.object("files").list("entries", entries, [](tesser::Object& o, Entry& e) {
      o.value("name", e.name);
      std::string name = e.name;                           // by value: the element is temporary
      o.file("content").download([name](tesser::FileRequest& r) { r.filename = name; return openFile(name); });
  }).key("name").readOnly();
  api.object("files").file("upload").upload([](tesser::FileRequest& r) -> std::unique_ptr<tesser::FileSink> {   // PUT /api/files/upload?filename=a.txt
      if (!validName(r.filename)) { r.refuse(tesser::Status::InvalidValue, "bad file name"); return nullptr; }
      return createFile(r.filename);
  });
  ```
- **Transports:** HTTP moves the raw bytes (§10.2); the message transports (serial, Bluetooth, WebSocket, NowTP) move them in base64 chunks, one `upload` or `download` envelope each (§10.1.1). Below a remote node, a gateway relays the transfer to the remote node (§14).
- **Abandoned transfers:** a transfer that has moved nothing for `Config::transferIdleMs` (30 s by default; 0 is never) is taken over by the next one to its path, from any client and transport: an unfinished upload is aborted, and its client gets `timeout` on its next chunk. A message transport has no connection to lose, so this is how a client that went quiet stops blocking the path.
- **Generic core:** `tesser::FileTransfer` does the work (resolution, access, size, one transfer at a time, the reply), host-tested; the HTTP transport only moves bytes, and `tesser::FileSession` drives it from envelopes.

### 4.7 Names and the tree's lifetime

- Names are `const char*` and are never copied. Use string literals or strings that outlive the `Api`. Remote nodes are the exception: `remote(std::string name, ...)` keeps a copy, for devices declared at run time (§14.1), as mounted peers do.
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
- A segment that is exactly `*` matches every child, in Set only (§6.5). Other operations answer `bad_request`. Names can't contain `*`, so it never shadows a child.
- No percent-decoding inside the core, since names can't contain characters that need it. HTTP decodes the URL before handing over the path.

### 5.2 Query options (Get)

| Option | Values | Meaning |
|---|---|---|
| `depth` | `0`–`255`, default unlimited | Levels of objects to expand below the target. An object beyond the limit renders as `{}`. `depth=0` on an object gives `{}`. Values are always rendered, lists and arrays (§4.4, §4.5) included: each is one value and renders whole, its elements' objects too, so `[]` always means empty. |
| `keys` | comma-separated names | Only these direct children of the target. An unknown name is `not_found`. |
| `exclude` | comma-separated names | All direct children except these. |
| `view` | `value` (default), `schema`, `hash`, `info` | The representation to return (§7; `info`: §10.1.2). |
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
- **Action:** the body is the argument. The response is the return value, `null` for `void`, `Status::Ok` or `Check::ok()`, or the deferred result. A failing `Status` or `Check` is the error; a `Check`'s message (a string that outlives the call, as for setters) goes in the error's `message`.
- **Object:** see §6.3.

`PUT /lamp/brightness 200` → `200`. `POST /lamp/toggle` → `null`.

### 6.3 Set on an object: patches

The body must be a JSON object, and it is applied as a merge patch:

```json
POST /motor1  {"control": {"speed": 0.8, "direction": true}, "config": {"invert": true}}
→ {"control": {"speed": 0.8, "direction": true}, "config": {"invert": true}}
```

The patch is processed in three passes:

1. **Validate everything.** Every key must exist. Values must convert and pass `range` and their validator, and must not be read-only. Action arguments must convert. Custom nodes run their optional validator. If any check fails, **nothing is applied**, and the error names the first failing path.
2. **Apply values** in document order.
3. **Call actions** in document order, after all values are applied. So `{"position": 10, "go": null}` sets the position first and then starts the move.

The response is the patched keys read back, which is the patch used as a shape. An action's key renders as `null`; call an action directly to get its return value. Actions finish before the response starts, so a failing action can still turn the whole reply into an error.

**Limits:**
- A setter that returns `false`, an action that returns a failure status, or a custom node that fails at apply time stops the patch. Earlier writes remain applied: only pass 1 is atomic. The error says so with `"partial": true`. A value's `.validate()` refuses in pass 1 instead.
- Deferred actions can't be part of a patch (`bad_request`). Call them directly.

This mirrors the predecessor's model, where a POST body mixed values and triggers, but with validation done before anything is applied.

### 6.4 Subscribe and Unsubscribe

See §11.

### 6.5 Set on many nodes: `*`

A Set whose path has a `*` segment writes to every child at that point, as if each had been named:

```json
POST /peers/*/sequencer/play  "intro"
→ {"spinner-1": null, "spinner-2": null}
```

- **Matches:** the `*` matches every child of an object, every element of a list (by key for keyed lists, by index otherwise) and every element of an array (by index). Children the client can't read (§13) aren't matched. A `*` below a value or an action is `not_found`.
- **Independent sets:** each match runs as a Set of its own, with the rest of the path and the same body: a value, an action's argument or a patch. Each is validated and authorized on its own, and the sets aren't atomic across matches: a match that fails doesn't undo the others.
- **Missing paths are skipped:** a match where the rest of the path doesn't resolve (a peer without `/sequencer`) is left out of the reply. A match where it resolves but the write fails is reported, including a patch key that doesn't exist. If nothing is left, the reply is `not_found` with the message `no match`.
- **Several `*`** in one path expand one after the other, each nested level replying like a match.
- **Remote nodes:** a match that is a remote node (§14) is forwarded like any request below it, and all of them are in flight together. The reply waits until every match has answered or timed out. Each waiting match takes a pending slot (`maxPending`), and one past the limit fails with `busy`; the fan-out itself takes none. A remote node whose peer is offline (§14.1) isn't sent to: it fails at once with `timeout` and the message `peer offline`. A transport that can't wait gets `not_allowed` for each match that would wait. A `*` below a remote node is forwarded as is, and the remote expands it.
- **Deferred actions** may be targets: the reply waits for each match's deferred result, as for remote nodes.

**Reply.** When every match succeeds, the status is `ok` and the body has each match's result under its name, in tree order: the value read back, the action's return value, or the patched keys.

When any match fails, the reply is an error with the first failed match's status (in tree order). The standard error body (§8) gets three more keys: `failed`, each failed match's error body by name; `ok`, the successful matches' results by name, present when there are any; and `partial: true` when some matches succeeded. The message names the count and the first failure, ready to show:

```json
{"error": "timeout", "path": "/peers/*/sequencer/play",
 "message": "1 of 3 failed: spinner-2: remote node didn't answer",
 "partial": true,
 "failed": {"spinner-2": {"error": "timeout", "path": "/peers/spinner-2", "message": "remote node didn't answer"}},
 "ok": {"spinner-1": null, "spinner-3": null}}
```

Each match's result is buffered, up to `maxResponse`, until the last one answers. A reply that had to wait is bounded by `maxResponse` too, like any deferred reply.

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

- **Types:** `object`, `boolean`, `integer`, `number`, `string`, `action`, `event`, `custom`, `list` (§4.4), `array` (§4.5), `remote` (§14).
- **Optional keys:** `writable` (only when true; values, arrays, lists and custom nodes), `min`, `max` (values and action arguments), `persist`, `description`, `arg`, `params` (an object argument's schema, §4.2), `returns` and `deferred` (actions), `maxLength` (`char[N]`), `enum`, `secret` (§4.1), `access` (the node's own write level when it isn't `public`, §13), and the presentation keys of §7.1.
- `depth`, `keys` and the shape apply to `children` the same way.
- A schema leaf is always an object containing `"type"`. Children are always under `"children"`, so a child named `type` can't be confused with a descriptor.
- Schema output is compiled out with `TESSER_NO_SCHEMA` when flash is tight.

**Hash view:** `GET /lamp?view=hash` → `"1a2b3c4d"`, the FNV-1a hash of the target's schema as 8 hex digits, so a client can keep a cached schema while the hash holds.
- It covers the whole schema the client may read (§13.1): `depth` doesn't apply, and `keys`, `exclude` or a shape are `bad_request`.
- It covers the structure, not the state of remote nodes: their `online` and `schema` keys (§14.1) are left out of the hash. They change with peers, and subscribers of the parent hear about them as changes. Were they hashed, two gateways mounting each other would change each other's hash on every advertisement, forever.
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
{"error": "busy", "reason": "low_memory", "path": "/peers/lamp", "message": "not enough memory for the response"}
```

`message` is for people: its wording may change, and it is compiled out with `TESSER_NO_MESSAGES`. A client that acts on an error reads `error` and, where one is given, `reason` (§10.1.2).

## 9. Execution and concurrency

**The core is single-threaded.** `Api` holds one recursive mutex. `Api::handle()` takes it, and so do the notification and persistence routines. On ESP32 several tasks touch the API: the HTTP server task, the NowTP task, `loop()`, and control tasks. The rules are:

- **Inline mode** (the default): a transport calls `api.handle()` from its own task, and the handler runs there under the lock. This is the lowest-latency, lowest-memory option.
- **Queued mode** (`api.config().queued = true`): transports post requests into a bounded queue (`maxQueued`, default 8, then `busy`), which `api.poll()` drains from `loop()` (or `api.startTask()` from its task). Use it when handlers must run in the application's context, for example when they touch code that isn't thread-safe.
  - **How a request is queued:** the request is copied and its reply detached: message transports answer later, and HTTP parks the connection's handler up to `deferTimeoutMs`. Replies that can't be detached are handled inline.
  - **Deferred actions** work from the queue.
  - **Cleanup:** queued requests of a client that goes away (`dropSubscriber`) are discarded. `poll()` takes a request out of the queue under the API lock and runs it before letting go, and `dropSubscriber()` takes that lock after discarding: a request already taken out is over before `dropSubscriber()` returns, so it never runs against a client destroyed meanwhile.
- **Application code** that mutates bound state from another task while transports are running should hold `tesser::Lock lock(api);`. Aligned reads and writes of 32-bit-or-smaller variables are atomic on ESP32, so simple flags and counters are safe without it. Multi-field invariants and strings are not.
- **Handlers must not block.** Long work belongs in a deferred action (§4.2) or a task, which reports completion through `pending.reply()` or through an event.

**Deferred completion:**
- `call.defer()` asks the transport to keep the reply open. Message transports (NowTP, serial, WebSocket) support this naturally.
- The HTTP server transport turns the request async (`httpd_req_async_handler_begin`, ESP-IDF 5.3 and later, so Arduino-ESP32 3.1 and later) and goes on serving others; the reply is written by the httpd task when it comes, or `timeout` after `deferTimeoutMs` (checked every 500 ms). With an older ESP-IDF it parks the request, blocking the httpd task up to `deferTimeoutMs`. Forwarded requests (§14) and `*` fan-outs wait the same way, so one slow peer doesn't stall the web UI, its WebSocket connections and streams. A request below a remote node may wait on the peer before it is even forwarded (the probe that asks whether the path is a file, a relayed transfer): it is served whole on a worker task (`tesser_remote`, 6 KB of stack, at most two at once; beyond, in the httpd task as before), so httpd never waits on a peer. Measured on DEVKIT-V3 and R1: a GET to a silent peer took 252 ms to fail (NowTP's `send_failed`) while local GETs answered within 84 ms; before, they waited for it. Streamed subscriptions (`?sub`) below a remote node still wait in the httpd task.
- `pending.reply()` may be called from any task.
- A `Pending` object that is destroyed without a reply answers `internal`, so a forgotten reply can't hang the client.
- **Slow work off the lock:** an action that would hold the API lock for long (a PBKDF2 key derivation at sign-in, 280 ms) defers and hands the work to `tesser::background(fn)`, the library's background task (one task, created on first use, 6 KB of stack by default, `setBackgroundStack()`; at most 8 jobs waiting), which runs it without the lock and answers through the `Pending`:

  ```cpp
  api.object("auth").action("login", [](tesser::Call& call) {
      tesser::Pending p = call.defer();
      std::string user = call.arg()["user"] | "", password = call.arg()["password"] | "";
      if (!tesser::background([p, user, password]() mutable { p.reply(checkPassword(user, password)); })) {
          p.fail(tesser::Status::Busy, "busy");
      }
  });
  ```
  What the work touches of the tree it reads under `tesser::Lock`, briefly.

**Replies and the lock.** A reply is rendered under the API lock, but not sent under it: a 30 KB schema over Bluetooth or a UART takes seconds, during which `poll()`, every other transport and the datagram queues would wait.
- **Streamed replies** (serial, Bluetooth, WebSocket envelopes, HTTP bodies) keep what is rendered, up to a window (2 KB; 1 KB on lines, serial and Bluetooth, which drain slowly while the next request may need the heap), instead of sending it at once (`Reply::wantsDrain()`, `drain()`). Between an object's children the handler lets go of the lock and sends what is waiting (only for a get, so a subscription's snapshot stays one with its start; never below a list element, whose nodes the application's data backs; and only when the lock is held once, not by an outer caller). The rest goes when the request is done, after the lock. A reply that outgrows the window with no such point in reach (a long list) sends under the lock as before. So a large reply may show values that changed while it was sent.
- **Lines** keep their output for a streamed reply from its first piece to its last without holding a lock: notifications and deferred replies that come meanwhile wait (notifications up to 2 KB, then they are missed and the next says `overflow`) and follow the reply, so nothing lands in the middle of its line.
- **Bluetooth** waits for room in NimBLE's pool (10 ms at a time, 2 s at most per piece) only while sending, so never under the API lock.

**Remote nodes and their endpoints.** An endpoint calls its remote nodes (§14) without its own lock, since they call back into the endpoint, the API and the application: ticks in `process()`, notifications from the peer, a token's answer. It pins each node under its lock for the length of the call. A pinned node isn't evicted (§14.1), and a node's destructor waits for the pins of other tasks to go, so a node is never destroyed under a call. Hence a node the application destroys itself must not be destroyed while holding the API lock (the call may be waiting for it) or from within a call into it. What touches only a node's routes (a round trip's time) is done under the endpoint's lock instead.

**Lock order.** Mutexes carry a rank (`LockRank`); a task takes a ranked mutex only while every ranked one it holds is lower: an endpoint's subscribers (10), the API (20), a remote node's copies (30), an endpoint's state (80), then leaves (90: routes, streams, link state, request queues, a line's output) and the byte accountant (99). Host builds check it (`TESSER_LOCK_CHECK`, unless `NDEBUG`): a violation logs both locks and aborts, or calls `setLockOrderHandler()`'s. On target it is compiled out.

## 10. Transports

### 10.1 Message envelope (NowTP, UDP, serial, WebSocket)

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
- **`op`:** `get`, `set`, `sub` or `unsub`; `upload` and `download` for file nodes (§10.1.1); on serial and Bluetooth lines, NowTP and UDP also `auth` (§10.4, §13.1). The keys `path`, `depth`, `keys`, `exclude`, `view`, `remotes`, `interval`, `changes`, `events`, `snapshot` and `body` map one-to-one onto `Request`.
- **A ceiling:** any of these ops but `auth` may end in `@public`, `@user` or `@admin` (`"op": "set@user"`): the node then acts at the lower of the client's level and that one (`Query::ceiling`). Gateways use it for their clients (§13.1). It is in the op, not a key of its own, so that a node that predates it refuses the request (`bad_request`) rather than ignore the limit. A subscription made with one carries it in its notifications (`{"op": "change", "path": "/x", "body": {...}, "as": "user"}`, and the same for events), and only an `unsub` with the same one ends it: one client can hold subscriptions to a path at several levels, and tell their notifications apart.
- **Unknown envelope keys** are ignored, for forward compatibility, unless the envelope is strict (`"v"`, §10.1.2).
- **Response:** the response is written as a stream: `{"id":…,"status":"…","body":` followed by the body and then `}`.
  - **Buffered** (NowTP, and every deferred or queued reply): the envelope is sent whole, up to `maxResponse`. A larger body is replaced with a `too_large` error.
  - **Streamed** (serial and WebSocket, for replies sent while the request is handled): `EnvelopeReply::stream()` sends the envelope in 1 KB pieces as it is rendered, so its size isn't limited and memory stays bounded. An error found before the first piece is out still replaces the reply. After that, a failed piece stops the rendering, and the transport ends the message as it can.
- **Framing** is the transport's job:
  - Serial: one envelope per line (`\n`). Non-JSON lines from the log are ignored by clients.
  - NowTP and UDP: one envelope per message.
  - WebSocket: one per text frame.

#### 10.1.1 File transfers

File nodes (§4.6) transfer over these transports too. The client sends one envelope per chunk and gives each one's `offset`; the bytes are base64:

```json
→ {"id": 1, "op": "download", "path": "/sys/backup"}
← {"id": 1, "status": "ok", "body": {"size": 5000, "filename": "backup", "contentType": "application/json", "chunk": 4096, "offset": 0, "data": "eyJjZmciOnsi…", "end": false}}
→ {"id": 2, "op": "download", "path": "/sys/backup", "offset": 4096}
← {"id": 2, "status": "ok", "body": {"offset": 4096, "data": "…", "end": true}}

→ {"id": 3, "op": "upload", "path": "/sys/firmware", "size": 6000, "filename": "fw.bin", "data": "6QMCIAA…"}
← {"id": 3, "status": "ok", "body": {"received": 2880, "chunk": 2880}}
→ {"id": 4, "op": "upload", "path": "/sys/firmware", "offset": 2880, "data": "…"}
← {"id": 4, "status": "ok", "body": {"received": 5760}}
→ {"id": 5, "op": "upload", "path": "/sys/firmware", "offset": 5760, "data": "…"}
← {"id": 5, "status": "ok", "body": null}                    // all 6000 bytes: the upload's reply
```

- **Opening:** a download starts at `offset` 0 (or none). An upload starts with its `size`, at offset 0, and may give a `filename` (`FileRequest::filename`) and its first `data`; later upload envelopes leave `size` out, so a client may open without data, learn the device's `chunk`, then send from offset 0. Either ends the client's previous transfer (an unfinished upload is aborted). Resolution, access, `maxSize`, the handler and one transfer per path work as over HTTP, with the same errors; a path that isn't a file node is `not_allowed`.
- **Downloads:** each reply holds the chunk at `offset`, and `end` is true on the last one. A file of unknown size may end with an empty chunk. The opening reply adds `size` (left out when unknown), `filename`, `contentType` and `chunk`, the largest chunk the device sends; `length` asks for smaller ones. The next request's `offset` is where the last chunk ended. The last chunk's own `offset` sends that chunk again, for a client whose reply was lost, even after the end; with a smaller `length`, only its start (a gateway short of memory asks so, §14), and the rest comes first in the next chunk, whose `offset` is where that start ended. Any other offset is `bad_request`, with the expected one in the message.
- **Uploads:** each reply gives the bytes `received`, and the opening one also the largest `chunk` the device takes. The next chunk starts at `received`, which can be short of the chunk's end: a node short of memory takes as much of a chunk as it holds then (§15), and the client sends the rest with the next. The bytes of a chunk before `received` are skipped, so a chunk sent again is acknowledged, not written twice. An offset past `received` is `bad_request`. The chunk that completes `size` gets the upload's reply instead (the sink's `finish()`, or its error), so the client knows which reply ends it. After that the upload is over: that last reply can't be asked again.
- **Checksums** (feature `crc32`): an upload envelope may carry `"crc32": <n>`, the CRC-32 (IEEE 802.3, as zlib's `crc32()`) of its decoded data, checked before anything is written; a mismatch is `bad_request` with reason `checksum`, and the transfer stays open for the chunk to be sent again. A download request with `"crc32": true` gets the chunk's CRC-32 in its reply, after `data`. A serial line can lose bytes and still be base64 (a multiple of four lost), which would otherwise be written as it came. A gateway checks a relayed upload's own, and computes a relayed download's from the peer's text, without asking the peer (it may not know the member).
- **Chunk sizes:** a download chunk is at most 4 KB, and smaller when `maxResponse` couldn't hold its base64 with room for the rest of the envelope (`(maxResponse − 256) × 3/4`), since NowTP buffers replies. An upload envelope must fit the transport's request limit: the device's `chunk` is `(maxRequestBody − 256) × 3/4`, at most 4 KB: 2880 bytes by default. Base64 adds a third. A download reply may carry less than `chunk` or `length`: each chunk is sized when it is asked for, to what the heap holds then (§15), so a node short of memory sends smaller chunks rather than `busy`, and the client goes on from where the reply's data ends.
- **One transfer per client:** each client (a serial or Bluetooth line, a WebSocket connection, a NowTP peer) has one `FileSession`. Its transfer ends with the client: a Bluetooth disconnection (`reset()`), a closed WebSocket, a lost NowTP peer, a line's level falling with `auth` (the transfer was opened at the higher level). A NowTP peer's session is also dropped after `transferIdleMs` without a request. Otherwise, an abandoned transfer is taken over (§4.6).
- **Threading:** as over HTTP, the handler runs under the API lock and the chunks outside it, in the transport's task, in queued mode too. A download chunk is read whole, and kept, before its reply goes out: up to 4 KB, plus its base64.
- **Below a remote node,** the transfer is relayed to the remote node (§14), and each reply is deferred until the remote's comes.
- **Generic core:** `tesser::FileSession`, platform-independent and host-tested; the transports only hand it these envelopes. `DatagramEndpoint::request(to, envelope, done)` sends them from a client.

#### 10.1.2 Protocol version, strict requests and error reasons

Nodes of different releases meet in one fleet, so a client learns what a node understands before relying on it, and acts on errors by code rather than by their English.

- **Version:** the envelope protocol has a number, `kProtocolVersion`: **2** for this release; nodes that predate it count as **1**. Protocol 2 has everything up to now: ceilings (§13.1), reasons, strict requests and `view=info` (below), as well as streamed HTTP subscriptions (§10.2), `auth` on lines (§10.4), file envelopes (§10.1.1), events-only subscriptions, `snapshot: false` and `*` fan-out. A client that finds protocol 2 needn't probe for any of them.
- **`view=info`** (`GET /?view=info`, or `{"op": "get", "path": "/", "view": "info"}`) answers `{"protocol": 2, "features": ["ceiling", "reasons", "strict", "info", "crc32", "limits", "schema", "messages"], "hash": "1a2b3c4d", "id": "c8f09ef4a67c", "limits": {"request": 4096, "response": 8192}}`: the version, the features by name, the hash `view=hash` gives at that path (left out with `TESSER_NO_SCHEMA`), the device id when known, and (feature `limits`) the largest request text the node takes (`maxRequestBody`) and the largest reply it sends this client (`maxResponse`, within its link's and, on a message transport, the client's own limit, §14.1). `schema` and `messages` are left out of builds without them; later features get names here before a new version. A client reads it first, in place of `view=hash`; a node that predates it answers `bad_request` ("view must be ..."): protocol 1. Below a remote node it is forwarded, so a client learns a peer's version through its gateway.
- **Advertisements** carry the version and the largest message the node takes on that link: `{"tesser": 84, "schema": "1a2b3c4d", "v": 2, "max": 16384}` over NowTP (§10.3), `"v": 2, "max": 16384` in UDP's (§10.7). A gateway keeps it per peer (`RemoteNode::protocol()`, 1 for an advertisement without one, 0 while unknown) and shows it in the stub (`"protocol": 2`, §14.1).
- **Strict requests:** an envelope with `"v": 2` or more (any op, `auth`, `upload` and `download` included) is refused, `bad_request` with reason `unknown_option`, if a member isn't one the node knows, rather than having the option ignored. Without `"v"` (or with 1) unknown members are ignored as before. The library marks what it sends with options as strict (`buildRequestEnvelope()` whenever the query has one, and every relayed file envelope), so from protocol 2 on, an option a node doesn't know fails rather than silently giving a different result. A plain get or set isn't marked.
- **Nodes at protocol 1** ignore `"v"`. What a gateway or client sends them that could matter:
  - A ceiling is in the op, so they refuse it (§13.1): fail closed.
  - `remotes: false`, a mirror's loop protection, came with remote nodes themselves: a node that can mirror honours it.
  - `snapshot: false`, `changes: false` and a download's `length` came later. A node that ignores them gives a larger reply, never a wrong one; gateways don't send the first two upstream.
- **Reasons:** an error body may carry a `reason` (`Reason`, `toString()`, `parseReason()`), next to its status, for what a client does next. An unknown reason (a newer node's) is acted on by the status alone. They don't depend on `TESSER_NO_MESSAGES`, and a gateway relays a remote node's as it wrote them; a `*` fan-out's error has its first failure's.

| Reason | With | Meaning, and what a client does |
|---|---|---|
| `low_memory` | `busy`, `internal` | Not enough heap for the reply, a remote's state or a queued request: try again, or ask for less (pieces) |
| `pending_limit` | `busy` | Too many calls waiting for remote nodes or deferred actions (`maxPending`): try again soon |
| `queue_full` | `busy` | A request queue is full (queued mode, a datagram endpoint): try again soon |
| `unreachable` | `busy` | The gateway couldn't send to the remote node: try again soon |
| `subscription_limit` | `busy` | No more subscriptions or HTTP streams for this client or node: poll instead |
| `transfer_busy` | `busy` | Another file transfer holds the path or the remote node |
| `peer_offline` | `timeout` | The remote node is known to be offline; nothing was sent |
| `no_answer` | `timeout` | The remote node or a deferred action didn't answer in time |
| `idle` | `timeout` | A file transfer was idle too long and was dropped |
| `cannot_wait` | `not_allowed` | This transport can't wait for a deferred reply, or deliver notifications |
| `not_authorized` | `unauthorized` | The client's level or the authorizer refuses it |
| `ceiling_unsupported` | `unauthorized` | A remote node too old to limit a gateway's client: only admins reach it (§13.1) |
| `unknown_option` | `bad_request` | A strict request named an option this node doesn't know |
| `send_failed` | `timeout` | The link couldn't deliver to the remote node, now offline (§10.3) |
| `auth_failed` | `timeout` | The remote node holds another pairing key, or none (§10.3) |
| `rejected` | `internal` | The remote node refused the message: no listener on the port, or too large (§10.3) |
| `line_overrun` | `busy` | Bytes of the request were lost on its line (a UART overrun, §10.4): send it again |
| `checksum` | `bad_request` | A file chunk's data doesn't match its `crc32` (§10.1.1): send it again |

### 10.2 HTTP (`esp_http_server`)

```cpp
tesser::HttpServer http(api);
http.begin(80, "/api");          // starts its own server, or:
http.attach(handle, "/api");     // registers a wildcard URI on an existing httpd_handle_t
```

`attach` works with any server built on `esp_http_server`, including PsychicHttp, so it coexists with an existing web UI.

| HTTP | Operation |
|---|---|
| `GET /api/<path>?depth=&keys=&exclude=&view=&remotes=&shape=` | get. A shape comes either in the body or URL-encoded in `shape` (browsers can't send a GET body); both at once is `bad_request`. |
| `GET /api/<path>?sub&interval=&changes=&events=&snapshot=&depth=&keys=&exclude=&remotes=` | streamed subscription: the response stays open, one notification per line (below). |
| `PUT`, `PATCH` or `POST /api/<path>` | set. The body is JSON. |
| `GET /api/<file>` (no `view`, or `view=value`) | download a file node (§4.6): the raw bytes. |
| `PUT` or `POST /api/<file>?filename=` | upload to a file node: the body is the raw bytes, with any `Content-Type`. `filename`, optional, is the name the client gives the file (`FileRequest::filename`). The reply is JSON. |
| `OPTIONS` | CORS preflight, when CORS is enabled |

- Responses up to 512 bytes go out with a `Content-Length`. Larger ones are streamed with chunked encoding through the same 512-byte buffer, so there is no limit on response size.
- Request bodies are capped by `maxRequestBody`, except uploads to file nodes, which are capped by the node's `maxSize`.
- **File transfers** stream in 2 KB pieces from the heap. A download of known size goes out with a `Content-Length` (so clients can show progress) and a `Content-Disposition: attachment; filename="..."`; one of unknown size is chunked. In that `filename`, quotes, backslashes, control and non-ASCII bytes become `_`; a non-ASCII name also comes as `filename*=UTF-8''<percent-encoded>` (RFC 6266), which browsers prefer. A refused or failed upload's remaining body is read and dropped up to 256 KB, so the client, which sends it all before reading, gets the error; beyond that the connection is closed. With CORS, file responses carry the headers too. Below a remote node, the server first asks the remote whether the path is a file node, and a relayed transfer holds the httpd task until it ends (§14).
- The status is mapped as in §8.
- The handler runs in the httpd task (inline mode).
- `begin()` starts a dedicated server with wildcard URI matching and an 8 KB stack. `attach()` uses 4 handler slots (5 with CORS, 6 with WebSocket) on an existing server, which must use `httpd_uri_match_wildcard` and should have at least 8 KB of stack. Measured peak use is about 4.9 KB: recursive rendering, newlib's float formatting and lwIP all run on it.
- `enableCors(origin)` adds `Access-Control-Allow-*` headers and answers `OPTIONS` preflights with 204.
- The client's IP address is passed to the core in `Client`.
- **Streamed subscriptions:** `GET` with `?sub` (also `sub=`, `sub=1` or `sub=true`) subscribes to the path and keeps the response open, for clients that can't use WebSocket, such as an HTTPS page reaching a device by address.
  - **Request:** the other query parameters have the envelope's names and meanings (§10.1, §11). Booleans are `true`, `false`, `1` or `0`. `?sub` with another method is `bad_request`. Authentication is as for any request, and the client's level is fixed when the stream opens: it filters the notifications as on WebSocket.
  - **Response:** `200`, `Content-Type: application/x-ndjson`, chunked, `Cache-Control: no-store`, CORS headers when enabled. Each line is one notification envelope, exactly as on WebSocket: `{"op":"event",...}` or `{"op":"change",...}`, with `"overflow": true` after lost notifications. With a snapshot (the default, as for `sub`), the first line is `{"op":"change","path":<path>,"body":<snapshot>}`, the snapshot bounded by `maxResponse`. Without one, the head goes out at once and nothing follows until something happens.
  - **Keep-alive:** a bare `"\n"` once nothing has gone out for `setStreamKeepAlive(ms)` (default 15 s; checked every third of it, so a stream is silent for at most 25 s). Clients skip empty lines, and can take 40 s of silence as a dead stream.
  - **Refusals** are ordinary responses with the JSON error body, sent before any stream byte: `busy` beyond `setMaxStreams(n)` streams (default 2) or the subscription budget (§11), `not_found`, `unauthorized`, and so on. Below a remote node (forwarded, §14), the handler waits for the remote's snapshot, up to `Config::deferTimeoutMs`, as for a forwarded `GET`, and the stream opens with it; a remote that doesn't answer gives `timeout`. A client should take anything but a `200` with `application/x-ndjson` as "no stream here", which also covers older firmware answering the value as JSON.
  - **Lifetime:** one subscription per stream, which ends when the connection does; there is no `unsub`. Each stream is a `Subscriber` of its own (`StreamSubscriber`, platform-independent and host-tested), so `maxSubscriptionsPerClient` applies per stream.
  - **Sockets:** a stream holds one of the server's sockets (`max_open_sockets`, 7 by default) and no handler. The handler returns once the head is out, leaving the socket open. Lines go out from the httpd task through `httpd_queue_work`, like WebSocket frames, so they never block it or interleave with other writes. The stream is the socket's session context, so a closed socket drops its subscription at once, with `attach()` too. A failed write closes the socket. Each write refreshes the socket's LRU counter, so with `lru_purge_enable` idle keep-alive connections are purged before an active stream. A purged stream is closed like any other, and the client reopens it.
  - ESP-IDF's asynchronous request API would also work, but it needs 5.3, and it doesn't watch the socket for a close.
- **mDNS:** `http.advertise("Heliostat 3", "heliostat-3")` announces a `_tesser._tcp` service on the server's port, with the API's base path, WebSocket path and device id in its TXT record (`api=/api`, `ws=/ws`, `id=c8f09ef4a67c`, §14.1), and the hostname `heliostat-3.local`. Browsers can't browse mDNS, but they resolve `.local` names, and TesserUI's bridge lists these services. It needs the mdns component: Arduino-ESP32 ships it; on ESP-IDF the application adds `espressif/mdns`, which the component then links. If the application started mDNS itself, its hostname is kept unless one is given.

### 10.3 NowTP

```cpp
tesser::NowTpTransport now(api, transport, /*port*/ 84);
```

- **Requests:** one envelope per NowTP message on the configured port (default 84). `set` requests must arrive in NowTP's reliable mode, which is unicast-only, so a broadcast or unreliable `set` is answered `not_allowed`. A broadcast `get` is answered by every node.
- **Responses:** reliable unicast to the sender's MAC.
- **Response size:** buffered in full (NowTP has no streaming), up to `min(maxResponse, nowtp maxMessageSize)`. Anything larger gets a `too_large` reply. Clients on NowTP should use `depth`, `keys` and shapes. Files move in chunks that fit (§10.1.1).
- **Files:** `upload` envelopes, like `set`, must arrive reliably. Each peer has a file session, dropped when the peer is lost (below) or after `transferIdleMs` without a request.
- **Notifications** use reliable mode, like responses (§11 explains why not latest-only). `sub` requests, like `set`, must arrive reliably.
- **Client identity:** the MAC address. NowTP's "peer lost" discovery event drops that client's subscriptions (below).
- **Security:** ESP-NOW frames aren't authenticated. Writes over NowTP should be restricted with an allowlist of MACs or encrypted peers (§13). A peer may also present a token with `auth`, as over UDP (§10.7).
- **Threading:** NowTP runs its receive callbacks while holding its own lock, so the adapter never handles a request there. It queues each message (a bounded queue, default 8, then `busy`), and a worker task (or `poll()`) handles it under the API lock. A message NowTP reassembled from several frames is taken as NowTP built it (`Message::buffer`, NowTP 8320a40 and later; `DatagramEndpoint::receiveOwned()`), so a large one isn't on the heap twice; a single frame's data, and every message with an older NowTP, is copied (§15). A reliable message the endpoint can't take (no room above the heap floor, or its incoming budget or queue full) is refused to NowTP (`Message::refused`, NowTP f09a571 and later; `DatagramEndpoint::linkRefuses()`): its sender is told busy and sends it again, and NowTP acknowledges a message only once the handler took it. With an older NowTP it is answered as below. The lock order is therefore always API, then NowTP, and replies sent from any API context can't deadlock.
- **Client:** the same endpoint sends requests to other nodes (`get`, `set`, or `endpoint().request()`) and matches responses by `id`, with a timeout per call (default 3 s) and at most 8 calls in flight. Handlers run in the worker task. `peerLost(mac)` fails a lost peer's calls at once, unless the peer was heard lately (below).
- **Peer lost:** NowTP loses a peer when it misses its discovery announcements for `peerTimeoutMs` (7 s), whatever else comes from it (NowTP before 7586949). Announcements are broadcasts, without retries: on a busy channel, a short heap or a weak signal, a gateway relaying a firmware image to DEVKIT-R1 was lost mid-upload while its chunks kept coming, and R1 dropped the transfer ("no upload in progress at this path", from DEVKIT-V3; reproduced by silencing the Gledopto's announcements). So the adapter hands NowTP's peer lost to `DatagramEndpoint::linkLost(peer)`, not `forgetPeer()`: a peer the endpoint heard from within `kLossGraceMs` (5 s) only goes offline (its routes, so remote nodes take another for new requests). Its calls, subscriptions, token and file transfer stay, its next message cancels the loss (`lossPending(peer)` until then), and once it has been silent for the grace it is forgotten as by `forgetPeer()` (`PeerLoss::Lost`). A peer already silent that long is forgotten at once. A relay keeps its route while the loss waits (§14.3).
- **Failed deliveries:** every reliable message goes with a NowTP completion handler, which hands its outcome to the endpoint (`DatagramEndpoint::sendFailed(peer, callId, failure)`, from NowTP's task; it only queues), so a call doesn't wait out its 3 s:
  - `SendFailed` or `Timeout` (no acknowledgement after NowTP's retries): the peer is unreachable for now. Its calls fail at once with `timeout` and reason `send_failed`, its routes go offline (a remote node takes its next route, §14.3), and `onPeerLost()` reports `PeerLoss::Failed` if it was online. What it holds here as a client (its subscriptions, its file transfer, its token) stays: one message lost on a busy link isn't the peer gone, and a lost reply to a relayed chunk used to end the gateway's transfer on the remote mid-upload ("no upload in progress", measured on DEVKIT-R1, 172 KB into a relayed firmware image). A peer really gone ends them when NowTP reports it lost, or when they idle out (§4.6, §8).
  - `AuthFailed` (the peer holds another pairing key, or none): the same, with `PeerLoss::AuthFailed` and reason `auth_failed`, so an application can say "wrong pairing key" rather than "offline". `onPeerLost()` reports it even for a peer never heard (its frames don't authenticate either), once, until the peer is heard again.
  - `Rejected` (the peer has no listener on the port, or the message is too large): only the request that carried it fails, with `internal` and reason `rejected` (its id is read from the envelope, `callIdOf()`); a refused reply or notification changes nothing. The peer stays online.
  - `QueueFull`, returned by `send()` itself (NowTP keeps a queue per destination), is a full buffer, not a failure: the request isn't sent (`busy`), and a streamed set waits for the next `process()`.
  - Any other link can do the same from its own completion callback.
- **Discovery:** `advertise()` puts `{"tesser": <port>, "schema": "<schemaHash>", "v": 2, "max": <maxMessage>}` (`v`: the protocol, §10.1.2; `max`: the endpoint's `Options::maxMessage`, the largest message it takes, §14.1) into NowTP discovery metadata, and `parseAdvertisement()` reads it, so clients can tell TesserAPI nodes apart and keep a cached schema while its hash (`Api::schemaHash()`, FNV-1a of the full schema) is unchanged. `peerEvent()` takes NowTP's peer events, and `mountPeers()` mounts the TesserAPI peers it reports (§14.1).
- **Re-advertising:** after the first `advertise()`, the advertisement follows the tree. A global schema revision (`schemaRevision()`) counts the changes that can alter a schema hash: a node added (a peer mounted, too) or a modifier. Temporary objects (list elements and prototypes), value changes and remote nodes' state (online, advertised hash) don't count. Once the revision has stayed unchanged for 1 s, the worker recomputes `schemaHash()` and updates the metadata if the hash differs, so a gateway that mounts a burst of peers re-advertises once. The logic is `DatagramEndpoint::autoAdvertise()`, host-tested with the in-memory link.
- **Generic core:** all of this lives in `tesser::DatagramEndpoint`, which is platform-independent and host-tested with an in-memory link. Another datagram transport (UDP, §10.7; LoRa, ...) only needs a send function and a call to `receive()`.
- **Optional:** the adapter is compiled only when NowTP is available. The core never includes NowTP.

### 10.4 Serial

`tesser::LineTransport` is platform-independent: it is fed bytes, and it calls a write function for each response line. There are thin adapters for an Arduino `Stream` (`StreamTransport`, polled from `loop()`) and an ESP-IDF UART (`UartTransport`, with its own reader task). This is the debugging transport, and the harness for end-to-end tests on hardware.

- Leading control and non-ASCII bytes are skipped, since a UART picks them up while the peer resets.
- **Lost bytes:** `UartTransport` installs its driver with an event queue (unless one is installed already). A hardware FIFO overflow (128 bytes, about 11 ms at 115200 baud: a flash write elsewhere stalls the UART interrupt unless `CONFIG_UART_ISR_IN_IRAM` is set), a full receive buffer (`rxBuffer`, 2 KB), or a frame or parity error marks the line being read (`LineTransport::lineDamaged()`, a flag: nothing is allocated). At its end, the line is answered `busy` with reason `line_overrun` under its id rather than parsed, since what is left may still parse, wrongly: seen on DEVKIT-V3 running TesserKIT before this, about one firmware upload in two over serial got a chunk answered "data must be base64". The input isn't flushed, so the line's end still comes; if it was lost too, the line is answered after 200 ms of quiet. `overruns()` counts them. Clients that send file data should add `crc32` (§10.1.1), for losses that leave valid base64.
- When the UART is also the console, log output from other tasks can interleave with a reply on the same line. Use a dedicated UART, or lower the log level once the transport is serving.
- Responses stream (§10.1): one line however long. The output lock is held from the first piece to the end of the line, so notifications and deferred replies from other tasks wait for it.
- **Tokens:** a line is authenticated by default (physical access, §13.2). One that isn't (`setAuthenticated(false)`, Bluetooth) is at the level the API's token check gives its token, checked for every request like an HTTP request's: the empty token at first, so a device whose check opens up to anyone (no admin account yet) is as usable over the line as over HTTP. The client presents a token with an `auth` envelope, which holds until the next one or `reset()` (a Bluetooth disconnection):
  ```json
  → {"id": 1, "op": "auth", "token": "3f9c…"}
  ← {"id": 1, "status": "ok", "body": {"access": "admin"}}
  ```
  The reply gives the line's level now, `public` for a token the check refuses. An empty or missing token signs out. When the level drops, the line's subscriptions and file transfer are dropped too, since they keep the level they were made at; a client that signs in keeps its subscriptions at their lower level and subscribes again to see more. An authenticated line accepts `auth` and stays `admin`.
- **Files** (§10.1.1): the line is one client, with one transfer at a time. An upload chunk's line must fit the line limit (`maxRequestBody` + 256), which the device's `chunk` does.

### 10.5 WebSocket

`http.enableWebSocket("/ws")` serves the §10.1 envelope over WebSocket on the HTTP transport's server. This is where browser subscriptions live.
- Needs `CONFIG_HTTPD_WS_SUPPORT`, which is enabled in Arduino-ESP32 3.x and must be turned on in menuconfig on ESP-IDF.
- One text message per envelope. Each connection is a client (`Subscriber`).
- **Large replies** stream (§10.1) as a fragmented message: a text frame, then continuation frames, the last one final. Browsers reassemble it into one message.
- **Ordering:** notifications, deferred replies and replies that fit in one piece are written from the httpd task through `httpd_queue_work`, so writes from other tasks never interleave. A fragmented reply is written directly while the httpd task handles its request, so no queued frame can come in between, but it can overtake notifications queued just before the request.
- **Cleanup:** when `begin()` owns the server, `close_fn` drops a closed connection's subscriptions and file transfer at once. With `attach()`, a closed connection is noticed on the next send to it.
- **Files** (§10.1.1): one transfer per connection. An upload chunk's message must fit `maxRequestBody`, which the device's `chunk` does.
- **Handshake:** a connection's authentication (§13) is decided from its handshake. ESP-IDF 6.1 stopped calling a WebSocket handler with the handshake's GET, so `HttpServer` reads the handshake in the pre-handshake callback, which needs `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT`. The component's Kconfig selects it; older ESP-IDF versions and Arduino-ESP32 3.x still call the handler. When neither path saw the handshake, the server logs a warning once and the connection is unauthenticated.

### 10.6 Bluetooth LE

```cpp
#include <tesser/transports/ble_transport.h>   // not in TesserAPI.h: it pulls in a BLE stack
tesser::BleTransport ble(api);

// Arduino (BLEDevice: Bluedroid on the ESP32, NimBLE on the C3 and S3)
ble.begin("heliostat-3");    // in setup(): BLE stack (unless started), service, advertising
ble.poll();                  // in loop(): requests are handled there

// ESP-IDF (NimBLE: CONFIG_BT_NIMBLE_ENABLED)
nvs_flash_init();            // NimBLE keeps its state there
ble.begin("heliostat-3");    // controller, host task, service, advertising, and a request task
```

The serial lines of §10.4 over the **Nordic UART Service**, so browsers (Web Bluetooth) and generic BLE terminals speak it:

| Characteristic | UUID | Use |
|---|---|---|
| RX | `6e400002-b5a3-f393-e0a9-e50e24dcca9e` | The client writes request lines, in any pieces; a write longer than the MTU is a long write (up to 512 bytes). |
| TX | `6e400003-b5a3-f393-e0a9-e50e24dcca9e` | Replies and notifications, in pieces of the MTU (minus 3): notifications or indications, see below. |

- **One `LineTransport`**, so one client at a time: advertising stops while it is connected. On disconnection, its subscriptions, token and file transfer are dropped and advertising starts again.
- **Runtime control**, for a device's Bluetooth settings:
  - `stop()` drops the client and stops advertising; `start()` advertises again. The stack stays up. `enabled()` says which.
  - `end()` stops the BLE stack and gives its memory back: the host, the controller and the request task. Measured on an ESP32 with NimBLE (TesserKIT's full example): 53 KB of heap free with Bluetooth on, 113 KB after `end()`, more than at boot with Bluetooth never started, since the deinit also frees what NimBLE reserved; on/off cycles come back within 150 bytes. Only the transport object itself stays. A connected client is dropped as by a disconnection, and `begin()` starts it all again. On ESP-IDF, `end()` doesn't wait for the request task, which exits between requests (or carries on, if `begin()` comes first), so a setter or action may call it, even one a Bluetooth client called. On Arduino it is `BLEDevice::deinit(false)`, which allows a new init; call it in `poll()`'s task. The Arduino library doesn't free the old server's service and characteristic objects, so each cycle leaks them.
  - `setName(name)` changes the GAP name and the scan response. Advertising restarts with it; a connected client sees it after it leaves. The scan response holds 29 bytes of it.
  - `connected()`, `peerAddress()` (`"aa:bb:cc:dd:ee:ff"`, empty when none) and `mtu()` (the ATT MTU, 0 when none) describe the client.
  - `onConnection(fn)` is called with `true` when a client connects and `false` when it leaves, in the task that handles requests (`poll()` on Arduino), never in the BLE stack's, so it may use the API.
- **Threading:** the BLE stack's task only copies what was written into a bounded inbox (8 KB) and sets flags; requests are handled in `poll()`'s task (Arduino) or the transport's own task (ESP-IDF). It must stay so: a send under the API lock may wait for something the BLE task delivers (a confirmation, a free buffer).
- **Pacing, ESP-IDF:** TX offers notifications and indications, and sends what the client subscribed to (browsers take notifications). NimBLE queues an accepted notification until the link carries it, and refuses one when its buffer pool is full; the transport then waits 10 ms and tries again, which paces a long reply. Indications go one at a time, each confirmed. Measured from an ESP32 to a Mac (MTU 517): about 60 ms per request, 12.8 KB/s for long replies.
- **Pacing, Arduino:** indications only, since the Arduino library drops the notifications its queue can't take; each confirmation paces the next. About 120 ms per request, 6.3 KB/s.
- Replies stream (§10.1), so their size isn't limited.
- **Authentication:** anyone in range can connect, so the line is not authenticated by default (§13.2): its level is the token check's for the token sent with `auth` (§10.4), or for none. A disconnection forgets the token. `setAuthenticated(true)` is for a bench or a link the application secures itself (pairing, bonding).
- **Flash:** NimBLE adds about 400 KB (an ESP-IDF example is 510 KB). Bluedroid on Arduino's ESP32 makes a sketch about 1.7 MB: it needs a larger app partition than the default 1.25 MB (`huge_app`).
- **ESP-IDF component:** `bt` is a private requirement; the transport compiles to nothing without `CONFIG_BT_NIMBLE_ENABLED`.

### 10.7 UDP

```cpp
tesser::UdpTransport udp(&api);
udp.begin();                              // port 8484 on every interface, once the network is up
udp.advertise();                          // name and schema hash, every 5 s
udp.mountPeers(api.object("peers"));      // the LAN's TesserAPI nodes, as remote nodes (§14.1)
udp.setPeerAccess(tesser::Access::User);  // anyone on the LAN, short of admin nodes
udp.broadcastSet(tesser::PeerAddress(), "/sequencer/play", "true");   // every node at once
```

The §10.1 envelopes between nodes and clients on a LAN, Ethernet or Wi-Fi, through the same `DatagramEndpoint` as NowTP: serving, client calls, remote nodes and forwarding, forwarded subscriptions, file transfers, `*` fan-out (§6.5) and discovered peers all work as over NowTP. Two things are new: broadcast writes, and a least level for peers with tokens sent in `auth` envelopes.

- **Addresses:** IPv4 and port, a 6-byte `PeerAddress`: the address bytes in order, then the port, most significant byte first. `udpAddress(192, 168, 1, 59, 8484)`, `parseUdpAddress("192.168.1.59:8484", defaultPort, out)` and `formatUdpAddress(a)` convert. A request's `Client::address` holds the same 6 bytes. An empty `PeerAddress` means every local network (below).
- **Datagrams:** each starts with a 20-byte header, multi-byte fields little-endian:

  | Offset | Size | Field |
  |---|---|---|
  | 0 | 1 | Magic `0x54` (`T`) |
  | 1 | 1 | Version (1) × 16 + type: 0 data, 1 acknowledgement, 2 advertisement |
  | 2 | 1 | Flags. Data: `0x1` reliable, `0x2` ack-request, `0x4` broadcast. Advertisement: `0x1` query, `0x2` goodbye |
  | 3 | 1 | Data: fragment index. Acknowledgement: 0 complete, 1 partial, 2 rejected |
  | 4 | 4 | Sender's nonce: random, new at each `begin()` |
  | 8 | 4 | Message id: per nonce, one more per message. An acknowledgement repeats the acknowledged message's |
  | 12 | 1 | Data: fragment count, 1 to 64 |
  | 13 | 1 | Reserved, 0 |
  | 14 | 2 | Data: fragment size `S` |
  | 16 | 4 | Data: message length |

  A message (one envelope, at most `maxMessage`, 16 KB) is cut into fragments of `S` bytes (`fragmentSize`, 1400), the last one 1 to `S`; fragment `i` is at offset `i × S`. A datagram is at most 1420 bytes, under the 1472 bytes of UDP payload an Ethernet frame holds, since ESP-IDF's lwIP doesn't reassemble IP fragments by default (`CONFIG_LWIP_IP4_REASSEMBLY`). UDP's checksum covers the rest. A partial acknowledgement carries the fragments received as a 64-bit mask (bit `i`: fragment `i`). A reliable message's fragments go only as far as the heap holds the network stack's copies of them above the floor (lwIP's, then the radio's: twice a datagram each), at least one, the last asking for an acknowledgement; the rest go as that acknowledgement says they are missing, so a large message is paced on a tight heap rather than all in flight at once. Measured on DEVKIT-V3 before this: when a peer mirroring it came back after a restart, the gateway's free heap dipped to 4.8 KB; host-side, the snapshot it sent the peer, all at once, held the message and every fragment's copy together (7 KB for 3 KB, before the radio's copies). An advertisement carries `{"name": "...", "schema": "<8 hex digits>", "id": "<MAC, 12 hex digits>", "v": 2, "max": 16384}` (`v`: the protocol, §10.1.2; `max`: the largest message the node takes, §14.1), or nothing (a bare query, a goodbye).
- **Reliable delivery** (`Delivery::Reliable`: requests, replies, notifications), modelled on NowTP's (its PROTOCOL.md):
  1. The sender sends every fragment, with ack-request on the last.
  2. The receiver answers an ack-request with complete or partial, and sends complete unasked when the message completes.
  3. On partial, the sender sends the missing fragments again at once. Without an answer it sends what is unacknowledged again after `ackTimeoutMs` (100 ms), doubling up to 1 s.
  4. After `sendTimeoutMs` (3 s) the message fails, and the peer is forgotten (`forgetPeer()`): its pending calls fail, its subscriptions and file transfer are dropped, its remote nodes show offline until it is heard from. A client that left without unsubscribing goes this way, at its next notification.
  - **Repeats:** a receiver remembers, per peer and nonce, the 64 message ids behind the newest it completed; a repeat is acknowledged, not delivered, and an older id counts as a repeat. So a request whose acknowledgement was lost runs once.
  - **Order:** not guaranteed, as over NowTP.
  - **Bounds:** 16 reliable messages waiting for acknowledgement (`maxOutgoing`; then the send fails, and a notification is marked missed), 32 KB of messages being reassembled (`reassemblyBytes`; the oldest is evicted), 16 peers remembered (`maxPeers`).
  - **Restarts:** a peer heard with a new nonce has restarted, and is forgotten as above, so a client that restarts leaves no subscriptions behind. A node's own broadcasts, when the stack loops them back, carry its own nonce and are ignored.
- **Broadcast writes:** `broadcastSet(to, path, bodyJson)` sends one set request to every node behind `to`, a broadcast address (`192.168.1.255:8484`, any directed broadcast), or the empty address for every local network: the transport sends it to the broadcast address of each interface that is up (Ethernet, the station, the soft-AP: address | ~netmask). 255.255.255.255 would leave by the default interface only.
  - **Once:** a broadcast is sent `broadcastCopies` times (2), with the same id; receivers apply the first copy that arrives and drop the others as repeats, so an action runs once.
  - **No replies:** the request has no `id`, and a broadcast request without one is never answered, errors included.
  - **The sender** doesn't apply it (its own copy is ignored): a node that should act too acts locally.
  - **Set only:** `DatagramEndpoint::Options::broadcastSet`, on for UDP, lets a set that arrived by broadcast through; `sub` and `upload` must still arrive reliably.
  - **Access:** the request is at its sender's level, like any request from that address (below).
  - **Replies, when wanted:** `endpoint().broadcast(to, op, path, body, onReply, windowMs)` gives the request an id. Every node answers it by reliable unicast, `onReply(from, status, body)` runs for each reply in the window (default: the call timeout), and once more with an empty address and `Status::Timeout` when it closes.
  - **Timing:** on Ethernet the nodes get the datagram within a millisecond or so of each other. On Wi-Fi, the access point holds group frames until its next DTIM beacon (about 240 ms later on the test network) and sends them once, without retries: the stations get them together, but late, and some are lost. Measured between two ESP32 devkits through a home access point: one copy lost 8% of broadcasts at 4 per second and 45% at 10 per second; with two copies, 140 of 140 arrived at either rate, each applied once. Broadcasts from a Mac to the same access point were lost in bursts that two copies didn't always cover.
- **Discovery** (§14.1): after `advertise()`, an advertisement goes to every local network every `advertiseMs` (5 s), and at once when the schema hash changes (after the 1 s quiet period of §10.3). `begin()` and `mountPeers()` broadcast a query, which advertising nodes answer by unicast, so a node finds the others at once; a node that advertises sends its own advertisement with the query. `end()` broadcasts a goodbye: peers show it offline at once. A peer that advertised and is then silent for `peerTimeoutMs` (15 s) is lost. The name is `setName()`'s, by default `tesser-<last two MAC bytes>`; the `id` is the Wi-Fi station MAC, the address NowTP knows it by, so a gateway reaching a node both ways mounts it once (§14.1). Queries and goodbyes go `broadcastCopies` times too.
- **Access:** datagrams carry no proof of origin, and anyone on a LAN can use any address. A peer is `admin` if the endpoint's `trust(fn)` accepts it; otherwise its level is the higher of `setPeerAccess(level)` (default `public`) and the token check's level for its token. It presents a token with an `auth` envelope, as on a Bluetooth line (§10.4): the reply gives the level, and a lower level drops its subscriptions and file transfer. The token is kept per address (the last 8 peers, `maxTokens`) until the next `auth`, or until the peer is forgotten (a restart, a failed delivery, a goodbye, `end()`). Tokens travel in clear: use UDP on a LAN you trust.
- **Runtime control:** `begin(port)` and `end()` at any time, `end()` then `begin(newPort)` to move. `end()` says goodbye, closes the socket and forgets every peer (remote nodes show offline, calls fail). It is safe from a request handler, one that came over UDP included: it doesn't wait for the worker task, which finishes the request at hand, ticks once more and exits, or carries on if `begin()` comes first. Destroy the transport after `end()`, outside the API lock: the destructor waits for the tasks. `running()` and `port()` describe it.
- **Threading:** a receive task (4 KB of stack, one priority above the worker) reads the socket and runs the link: fragments, acknowledgements, advertisements. It never takes the API lock, and hands complete messages to the endpoint's queue (16 deep). A worker task (6 KB by default) handles requests and client callbacks, retransmits and advertises, waking at least every 20 ms. `begin(port, tasks)` chooses: `Tasks::Both` (the default, `begin(port, true)`), `Tasks::None` (`begin(port, false)`: `poll()` from `loop()` reads the socket and does the work), or `Tasks::Receive`: the receive task alone, and the application's own task does the worker's job, calling `poll()` at least every 20 ms and whenever the endpoint's `onQueued()` hook fires. That saves the worker's 6 KB. Without a worker, `begin()` leaves `onQueued()` to the application (it only clears the hook its own worker had), and the peers `end()` forgets are dropped at the next `poll()`. lwIP's receive mailbox per socket is small (`CONFIG_LWIP_UDP_RECVMBOX_SIZE`, 6), which is why receiving has its own task: without it, bursts overflow the mailbox between two polls.
- **Sending:** `sendto` under a mutex, so `end()` never closes the socket under a send. Wi-Fi refuses bursts with `ENOMEM` while its buffers are full: the datagram is tried again, up to 20 times, a tick apart.
- **Measured** (two ESP32 devkits and a Mac on a home Wi-Fi network): a GET from the Mac in 19–23 ms median; file transfers in envelopes at 90–100 KB/s up and 115–130 KB/s down; a GET through a gateway (HTTP, then UDP: the file probe of §14, then the request) in 120 ms median, against 52 ms for the gateway's own GET; relayed HTTP file transfers at 40–44 KB/s up and 30–51 KB/s down.
- **Generic core:** `tesser::UdpLink`, platform-independent and host-tested over an in-memory network that loses datagrams. It takes a send function, `packet()` calls and `tick()`, so an application's own service can be tested the same way. `UdpTransport` is the socket around it: lwIP sockets and `esp_netif`, the same code on ESP-IDF (5.1 and later) and Arduino-ESP32 3.x.
- **Interfaces:** `setInterfaces(mask)` (`kEthernet`, `kWifi`, `kAp`; default all) keeps UDP to some of the device's interfaces: nothing is served, advertised, broadcast or routed on the others, so a show network on Ethernet isn't reachable over Wi-Fi. With one interface, `begin()` binds the socket to it (`SO_BINDTODEVICE`), so everything leaves by it even when Ethernet and Wi-Fi share a subnet, where lwIP would otherwise pick; an interface not there yet at `begin()` falls back to the rule for several. With several, a peer belongs to the interface whose subnet holds it (the default interface for peers beyond them): datagrams from peers of other interfaces are dropped and none are sent to them. `interfaceOf(address)` tells a peer's interface, the bound one or the only up interface whose subnet holds it, else `Any` (interfaces sharing the subnet: lwIP decides); it names the interface of UDP routes (§14.3). `begin()` again to change the binding. Checked on an ESP32 devkit (Wi-Fi only): bound to Wi-Fi, requests and broadcasts flow as before; kept to Ethernet, it neither answers nor advertises, and its peers fail over to NowTP.
- **Not covered:** IPv6, multicast groups, encryption.

## 11. Change tracking and subscriptions

**Tracking changes:**
- A global 32-bit generation counter is stamped on a node when it changes (0 means "never changed"). A subscription and the persistence code each keep the generation they last looked at, and a node changed since if its stamp lies between that and the counter, compared modulo 2^32: so no change is missed however many changes elsewhere came in between, across the counter's wrap too. A stamp unchanged for a whole lap (2^32 changes) can fall in that window once more, which costs one needless notification or save. To keep the window narrow, a subscription left undelivered for more than 2^30 changes drops what is older (its next notification says `"overflow": true`), and the persistence code's generation follows the counter while nothing persisted changes. Generations never leave the device. Cost: 4 bytes per node on 32-bit targets (a 16-bit stamp, compared with wrap-around, fitted in the node's padding, but a value changed once came back in every notification after 32,768 other changes, and persisted changes went unsaved, in about an hour of a busy device).
- A node is marked changed:
  - automatically, when a set or patch goes through the API
  - explicitly, with `node.changed()` or `api.changed("/path")`. On an object, `changed()` marks every value below it. Both are thread-safe and cheap.
  - by sampling, with `.watch()`: while anyone is subscribed, `Api::poll()` renders each watched value every `Config::watchIntervalMs` (200 ms) and compares a 32-bit hash, kept in the node's metadata. A new subscription first samples the watched values below its node, so its snapshot is the baseline: a change right after subscribing is notified on the next sample.
- Bound plain variables can't announce their own changes, so either `changed()` or `watch()` is needed for push updates.

**Delivering:** `Api::poll()` sends pending change notifications. Call it from `loop()`, or let `api.startTask(periodMs)` (ESP, default 20 ms) do it. It does nothing while nobody is subscribed.

**Subscribing** (message transports: serial, NowTP, UDP, WebSocket, Bluetooth; over plain HTTP, a streamed GET, §10.2; another HTTP `sub` answers `not_allowed`):

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
| `changes` | Deliver change notifications (default `true`). With `false` the subscription carries only events, filtered as usual by path, `depth`, `keys`, `exclude` and access: a client can follow the notifications of a whole tree (`"path": "/"`) without its changes. `interval` then has no effect, and the snapshot is still sent unless `snapshot` is `false`. It counts against the limits like any subscription. |
| `events` | Also deliver events under the path (default `true`). |
| `snapshot` | Reply with the current state (default `true`). With `false` the reply body is `null`, for clients that read large subtrees in pieces with gets. |

**Notifications:**
- A change notification is a **sparse merge patch** rooted at the subscription path, holding only the values changed since the last notification. Clients apply it exactly like a `set` body, so one format serves writes, notifications and persistence. For a subscription to a single value, the body is the value.
- Change notifications are deltas, so they are sent **reliably**. NowTP's latest-only mode would lose a superseded delta's values.
- **Failure:** if a notification can't be queued, the subscription isn't advanced. The next one carries the missed changes merged with newer ones, plus `"overflow": true`. It is tried again after 50 ms, then after waits that double up to 2 s while it keeps failing (§15). The same flag follows when a transport reports a queued notification lost, and tells the client to re-read.

**Events:** an event (`o.event("pressed")`, then `node.emit(value)`) is never coalesced. It goes out at once, reliably, to every subscriber whose path covers it and that didn't ask for `"events": false`. It reaches a client once even when several of its subscriptions match:

```json
← {"op": "event", "path": "/button/pressed", "body": 3}
```

**Limits and cleanup:**
- At most `maxSubscriptions` (16) subscriptions in total, and `maxSubscriptionsPerClient` (4) per client. Beyond that, `busy`.
- Notifications go through each transport's own bounded queue: NowTP's transmit queue, `httpd_queue_work` for WebSocket and HTTP streams. A slow client never causes unbounded allocation.
- Subscriptions die with their client: WebSocket close, an HTTP stream's socket closed, NowTP peer lost, a `LineTransport` destroyed, or an explicit `unsub`.

**Client side:** `DatagramEndpoint` (NowTP) sends `sub` requests like any other, with `interval`, `changes`, `events` and `snapshot` in its `Query`. Incoming notifications go to `onNotification(handler)`; they are never answered, so two nodes subscribed to each other can't ping-pong.

## 12. Persistence

```cpp
tesser::NvsStorage storage;               // or FileStorage("/littlefs/state")
api.object("config").persist();           // everything below it
api.value("calibration", cal).readOnly().persist();
api.persistVersion(2, migrate);           // optional: the stored shape's version (below)
api.persistence(storage, 2000);           // once the tree is declared: takes the defaults; debounce 2 s
api.load();
```

- **What is saved:** values, custom nodes and lists marked `.persist()`, or below an object marked `.persist()`, in the same sparse format as patches and change notifications: the value view filtered to persisted nodes, secrets included, **and only those that differ from their defaults**.
- **Defaults** are the values when `persistence()` is first called (and, for persisted nodes declared after it, when `load()` is), before anything is loaded: a 32-bit hash of each, 8 bytes per persisted value. So a release that changes a default reaches every device whose user never changed that value. A value set back to its default leaves its record, and a record left empty is removed (`Storage::remove(key)`; a backend without it saves `null`, which `load()` takes as no record). Declare the tree, then call `persistence()` and `load()`: a value changed before `persistence()` counts as a default and isn't saved.
- **Records:** the state is stored as one record per top-level node that holds persisted state, under the node's name: `config` holds `{"brightness": 7, "tuning": {"gain": 2}}`, and a persisted value at the root (`calibration`) holds just its value. A change rewrites only the records it touched, so a setting changed often doesn't rewrite the rest of the state. A record is measured first, then written in one allocation of its exact size reserved from the byte budget (§15); on a short heap it waits, still to be saved, rather than grow by doubling. (NVS takes a blob whole, so records aren't streamed.) `api.persistedState()` returns the whole state as one document, the records under their names, with `"@version"` when there is one: a backup. Unlike the records it holds every persisted value, defaults included, so restoring it gives exactly the backed-up state, whatever changed since (a value set back to its default is set back).
- **Versions and migration:** `api.persistVersion(version, migrate)`, before `load()`, gives the stored state's shape a version, stored with it (record `@version`; none before versions: 0). `load()` hands every record stored under another version to `migrate(record, version, value)` before applying it: the record's top-level name, the version it was stored under, and the stored document, which the migration may change in place: rename keys, drop values that equal an old default (devices that saved every default before records were sparse hold them all). Returning false drops the record. The records are then saved again, sparse, under the new version. `restore()` migrates a backup by its `"@version"` the same way. A renamed top-level node's old record isn't loaded: rename below the top level, or keep the top-level name.
- **Storage backends:** `tesser::Storage` has three methods: `load(key, out)`, `save(key, data)` and `erase()`, which removes every record (a factory reset; the running state keeps its values until the next boot).
  - `NvsStorage(namespace)`: one NVS blob per record, in its own namespace (default `tesser`). NVS keys hold 15 characters, so longer names keep their first 7 and get a hash. The application initializes NVS.
  - `FileStorage(dir)`: one file per record, `<dir>/<key>.json`, through stdio, so any VFS mount on ESP and the host. The directory is created on the first save. It writes to a temporary file and renames it, so a power cut leaves either the old or the new record. Files are read and written unbuffered (`setvbuf(_IONBF)` before any I/O), so FileStorage allocates no stdio buffer: on ESP-IDF, newlib gives a FILE a buffer the size of the filesystem's block at its first read or write (4 KB on LittleFS), which a caller reserving its memory first can't foresee. A save writes the record in one call; a load reads the file's size and fills a string allocated once; a removal checks for the file with `stat()`. Found on a Gledopto running TesserKIT with settings on LittleFS: with about 12.8 KB free and the 8 KB floor, each save's reservation, buffer included, was refused, every 60 ms, and settings changes were lost.
  - `MemoryStorage`: tests.
- **Saving:** `Api::poll()` notices changes to persisted values through their generations, at most every 50 ms. Once `debounceMs` has passed without a new change, it writes the records whose values changed since the last save. Changes to values that aren't persisted never cause a save. `api.save()` writes every record immediately.
- **Loading** is lenient: the stored state goes through each value's normal validation, but whatever no longer fits is skipped with a warning and the value keeps its default. That covers renamed or removed keys, values now out of range, a key that became an object, and a value no longer persisted. So schema changes never break a boot.
- **Read-only values:** values marked `readOnly()` for the API (calibration data written by an action, a boot counter) are restored too. Getter-only values can't be.
- **Restoring** a backup: `api.restore(json, &skipped)` applies a document like `persistedState()`'s, leniently like a boot load, so a backup from an older firmware restores whatever still fits; what equals today's defaults isn't stored. The paths it skipped are listed in `skipped`. Unlike a boot load, what it applies is marked changed, so subscribers hear about it, and every record is saved at once. It returns `bad_request` if the document doesn't parse, and `internal` if saving failed.

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
- A transport that only knows "authenticated" (`Client::authenticated`, below) gives an authenticated client `admin` and any other `public`. That is the case of trusted NowTP peers, and of serial lines unless they are unauthenticated (below). A `LocalClient` is `admin` by default; `setAccess(level)` gives it any level (§14.2).
- HTTP and WebSocket clients present a token. The transport's own token (`HttpServer::setToken`) gives `admin`. Any other token, or none (an empty token), goes to the API's token check, `api.authenticate(fn)`, whose answer is the client's level; without a check, `public`. Secrets are compared in constant time (`secretEquals(presented, expected)`: its time depends only on their lengths), so a client timing replies can't find a token a byte at a time; a token check should use it too. A WebSocket connection's level is decided once, from its handshake.
- Unauthenticated lines (Bluetooth) present a token with an `auth` envelope (§10.4), checked the same way for every request; until then, the empty token.
- Untrusted NowTP and UDP peers present a token with an `auth` envelope (§10.4), held per peer address until the next one or until the peer is forgotten (§10.7); until then, the empty token, as an HTTP request without one: `public` unless the check opens the device to anyone. Their level is at least the endpoint's `setPeerAccess(level)` (default `public`), so a LAN can let anyone use a device short of its admin nodes.
- **Gateways:** a client never gets more through a gateway than its own level on the gateway. The remote node sees the gateway as its client (a trusted peer, or one that signed in, is often `admin`), so the gateway hands the client's level on as a ceiling (§10.1): the remote acts at the lower of the gateway's level and the client's. Otherwise a gateway signed in to its peers as an admin would let an anonymous browser read and write their admin nodes.
  - **Everything forwarded** carries it: get and set below a remote node, actions, `*` fan-out, file transfers and the HTTP probe before them (§14), and subscriptions, through every transport and across several gateways (each hop passes on what it was given, lowered by its own level).
  - **Admin clients** forward without a ceiling, as before; so does a gateway's own traffic (mirrors at `admin`, probes, streamed sets). An HTTP client without a token is `public` unless the token check says otherwise, so on a gateway without authentication its clients reach remote nodes as `public`.
  - **Shared state is per level:** forwarded subscriptions share an upstream subscription only between clients at the same level, each asking the remote for that level, so each gets the changes and events the remote gives that level. A mirror's copy is what a client at the mirror's level reads (`mirror(interval, refresh, level)`, default `public`), and only clients at that level or above get it; for the others the node is unmirrored (§14).
  - **Older nodes** don't know the ceiling and refuse the op, so a request with one is never served above the client's level: its client gets `unauthorized`, "the remote node can't limit a client's access: only admins reach it through a gateway". Admins still reach them. Update the remote nodes to give lower clients access.

### 13.2 Authorizer and authentication

- **Authorizer:** `api.authorize(fn)`, where `fn(const Client&, Op, const Node&) -> bool`, is consulted for the target of every get, set and subscribe, and for every node a patch touches (during validation, so a refused patch changes nothing). A refusal is `unauthorized` (HTTP 403).
- **Default:** without an authorizer, everything is allowed. Access levels (§13.1) apply independently: a request must pass both.
- **Ready-made authorizers:** `authorizers::readOnlyUnlessAuthenticated()` (anyone reads and subscribes; writes and actions need authentication) and `authorizers::authenticatedOnly()`.
- **What makes a client authenticated** (`Client::authenticated`) is the transport's business:

| Transport | Authenticated when |
|---|---|
| HTTP | `HttpServer::setToken(token)` is set and the request carries `Authorization: Bearer <token>` or `?token=<token>` (`admin`), or the API's token check gives its token a level above `public` (§13.1) |
| WebSocket | Same checks, on the handshake |
| NowTP | The sender is in `NowTpTransport::trustPeers({...})`, or `DatagramEndpoint::trust(fn)` accepts it (`admin`), or its level is above `public`: the token check's for its token (`auth`, or the empty one), or `setPeerAccess()`. ESP-NOW frames carry no proof of origin, so trust is in MAC addresses; combine it with encrypted NowTP peers when that matters. |
| UDP | As NowTP: `trust(fn)` on the endpoint, a token sent with `auth`, or `setPeerAccess()`. Trust is in IP addresses, which anyone on the LAN can use. |
| Serial | Always (physical access), unless `LineTransport::setAuthenticated(false)`: then as Bluetooth LE |
| Bluetooth LE | When the API's token check gives the line's token (`auth`, §10.4), or the empty one, a level above `public`; always with `BleTransport::setAuthenticated(true)` |

- `Client` also carries the transport kind and the address (IP or MAC), for authorizers that need more.
- Hiding nodes from the schema is not access control: the authorizer runs on every access.
- TLS is the application's choice of server.

### 13.3 Tokens as a client

A node that writes to other nodes' protected paths over NowTP or UDP presents a token to each of them, as a browser does over HTTP:

```cpp
node->setToken(token);          // a remote node (§14): every route, kept across failovers
peerClient.setToken(token);     // a PeerClient
udp.endpoint().setPeerToken(address, token);
api.remoteHooks().onAccess([](tesser::RemoteNode& node, tesser::Status s, tesser::Access level) {
    if (s == tesser::Status::Ok && level == tesser::Access::Public) refreshToken(node);   // refused
});
```

- **Presented first:** before the next request to the peer, the endpoint sends `{"op":"auth","token":"..."}` (§10.7). Requests to the peer wait for its answer, since datagrams may overtake each other; then they go, whatever the answer.
- **Again when forgotten:** the peer forgets a token when it forgets the client (a restart, a failed delivery, a goodbye, §10.7), and the client notices most of these: whenever the client's endpoint forgets the peer (a new nonce, a goodbye, a failed delivery, NowTP's peer lost, `stop()`), the token is presented again before the next request. A peer that forgot it unseen answers `unauthorized`: the token is presented again and that request sent once more; a second `unauthorized` is the request's answer.
- **The answer:** the level the peer grants, from its token check: `grantedAccess(address)`, `node.access()`. `DatagramEndpoint::onAuth(fn(address, status, level))` and `api.remoteHooks().onAccess(fn(node, status, level))` (an Api's nodes; process-wide: `RemoteNode::onAccess()`) hear every answer, in `process()` without the API lock: `public` means refused, the cue to get a new token (TesserKIT logs in to the node's own `/sys/auth/login` with an account stored on the master). Changing the token presents the new one before the next request; an empty one stops presenting (the peer keeps the old one until it forgets the client).
- **Tokens travel in clear**, as over HTTP without TLS.

## 14. Remote trees and the client

**Client.** `DatagramEndpoint` (and `NowTpTransport` and `UdpTransport` on top of it) sends requests to other nodes and matches responses by `id`, and broadcasts them where the link can (UDP, §10.7):

```cpp
nowApi.get(mac, "/lamp", [](tesser::Status s, JsonVariantConst body) { ... });
nowApi.endpoint().request(addr, tesser::Op::Subscribe, "/sensors", "", done, query);
nowApi.endpoint().onNotification([](const tesser::PeerAddress& from, JsonObjectConst msg) { ... });
udp.endpoint().broadcast(tesser::PeerAddress(), tesser::Op::Set, "/lamp/on", "true",
                         [](const tesser::PeerAddress& from, tesser::Status s, JsonVariantConst body) { ... }, 300);
```

**Remote mount (gateway).** A remote node grafts another node's tree into the local one:

```cpp
nowApi.remote(api, "kitchen", kitchenMac);              // or api.remote("kitchen", endpoint, address, "/")
nowApi.remote(api, "light", kitchenMac, "/lamp").mirror(200);
```

- **Forwarding:** get and set on `/kitchen/...` are forwarded with the path rewritten and the client's level as a ceiling (§13.1), and complete through a deferred reply (`maxPending` applies). Error paths in the remote's reply are mapped back under the local path (`/kitchen/nope`). A silent node answers `timeout` after the node's `timeout()` (default: the endpoint's 3 s). A node known to be offline (§14.1) isn't sent to: get, set and a subscription that needs the remote fail at once with `timeout` and `peer offline`, without taking a pending slot. Transports that can't wait get `not_allowed`.
- **File transfers** to a file node below a remote node (`PUT /api/peers/lamp/sys/firmware`) are relayed to the remote as `upload` and `download` envelopes (§10.1.1), one at a time, in the remote's chunks, whatever the client's transport.
  - **Message clients** (serial, Bluetooth, WebSocket, NowTP) send their envelopes to the gateway as to any node. Each reply is deferred until the remote's comes (`maxPending` applies; transports that can't wait get `not_allowed`), so the gateway's task never waits for the remote. `received` counts the bytes the remote has. The opening reply's `chunk` is the smaller of the gateway's and the remote's, so each envelope takes one round trip; a larger one is split. Downloads come in the remote's chunks as it sent them, base64 included: the client's `length` is passed on, capped at the gateway's upload chunk (the size of a relayed upload's parts, since the gateway holds the remote's reply whole; its download chunk can be larger), and so is the `chunk` it reports.
  - **HTTP** moves raw bytes, and its requests don't say whether the path is a file. A `GET` (value view), `PUT` or `POST` below a remote node therefore first asks the remote for the path's schema, at depth 0: one round trip (about 20 ms over NowTP). The answer, file or not, is remembered for a few paths (8, the oldest going first) while the remote advertises the same schema hash (`RemoteNode::knownFile()`), so later requests to those paths skip it; a new hash (new firmware, a changed tree) asks again, and without an advertised hash it is asked every time. Errors aren't remembered. A file node is relayed; anything else is forwarded as usual, and an unreachable remote is an error at once. The relay waits for each chunk's reply, so the body is read only as fast as the remote takes it, and the client's upload progress follows the remote: measured between two ESP32 devkits over NowTP, a relayed upload runs at about 32 KB/s (downloads 37 KB/s), so a 1 MB image takes about half a minute. With ESP-IDF 5.3 and later the request turns async once the transfer is open and the relay runs on a task of its own (`tesser_relay`, 4 KB of stack, for the transfer's length), so the server, WebSocket connections included, goes on serving. With an older ESP-IDF it runs in the httpd task, which serves nothing else meanwhile; a user interface that must stay live then uploads over WebSocket envelopes (about 21 KB/s), whose relay never holds the task.
  - **One transfer per remote:** a node keeps one transfer per client, and the gateway is one client of it, so the gateway relays one transfer per remote at a time. Another, to any path of the same remote, is `busy`; transfers to different remotes run side by side. Get, set and subscriptions aren't affected. An idle relay is taken over after `transferIdleMs`, as in §4.6.
  - **Memory:** each relayed part is built once, at its exact size (the envelope's members, the data base64-encoded in place), and only with room for it and for the envelope it goes out in (the byte budget, §15); the bytes waiting to be relayed grow to their exact need the same way. A part is written by a builder straight into the buffer the link sends from (`DatagramEndpoint::requestBuilt()`; NowTP's `sendBuffer()`), and it is the builder, not a copy of the part, that the call keeps should the part go again (to a peer with a token, after `unauthorized`). A download's reply is relayed from its text (`requestBuilt()` with a raw handler): its other members are parsed alone, and its data goes on as the remote wrote it (message clients) or is decoded once (HTTP). Each part is also sized when it goes, to what the gateway's heap holds then (§15): a download's part to its reply, the gateway's copy of it and theirs in flight; an upload's part to its envelope and the link's copies (twice it); building the part checks that room again, and without it the part goes again at half the size. Measured on DEVKIT-V3 counting the link's copy once: about 6.5 KB free while relayed upload parts went out. A part refused for memory all the same, by the remote (`busy`, `low_memory`) or here (no room to build it, or to hold the reply), goes again at half the size, down to 384 bytes, and the transfer's parts stay that size; a download's part is asked for again at the same offset with a smaller `length` (§10.1.1). Measured between DEVKIT-V3 and DEVKIT-R1 running TesserKIT, R1 had 4 to 5 KB above its floor with a file open, and a 2.9 KB part's reply missed by 7 bytes. Only when even that fails is the chunk answered `busy` (`low_memory`) and dropped, and the transfer stays open: a message client sends it again from `received`; an HTTP upload ends, to be sent again. Measured on DEVKIT-V3 before this, with about 23 KB free: building a part took about three times its encoded size in growing strings, and a relayed upload aborted the gateway. Then, relaying a 2.9 KB chunk over WebSocket held about 23 KB at once (the frame 3.9 KB, its parsed envelope with a copy of the data 4 KB, the decoded chunk 2.9 KB, the outgoing envelope 4 KB and NowTP's copy of it 4 KB, plus the call's copy for a resend), so 29 KB free at rest wasn't enough; without the data in the envelope's document, the copies and the kept text, about 15.5 KB. A relayed download needed twice its reply's text to parse it: more than 18 KB above the floor for a 4 KB reply, 8 KB less from its text (host-measured, `relay_download_on_a_short_heap`).
  - **Errors** from the remote come back under the local path, as for get and set. A chunk whose reply hasn't come within the node's `timeout()` is sent again, twice at most (the envelopes can be repeated, §10.1.1), then the transfer fails with `timeout`. A remote that is offline (§14.1) fails at once with `timeout` and `peer offline`. Any error but `bad_request` (a wrong offset, which the client can correct) ends the relay.
  - **Abandoned uploads:** envelopes have no abort, so an upload that ends early on the gateway (its client gone, an error, a takeover) stays open on the remote until it has been idle for the remote's `transferIdleMs`, or until the gateway's next transfer to it. Its sink is then destroyed without `finish()`: an update isn't committed.
  - **The remote's handler** must open a transfer within the node's `timeout()`: an update that erases its whole partition when it opens needs a longer one.
  - **Generic core:** `tesser::FileRelay`, made by `RemoteNode::relayFile()`, host-tested. `FileTransfer` drives it waiting for each reply (HTTP); `FileSession` drives it without waiting.
- **Writes** must target the remote subtree: a patch of the parent can't include a remote node. A `*` (§6.5) can match remote nodes, and the matches are forwarded together.
- **Without a mirror**, a remote node renders as `null` in a parent read, and as `{"type": "remote"}` in the schema; reads below it are always forwarded live, and subscriptions are forwarded (below).
- **`.mirror(intervalMs, refreshMs, level)`** keeps a local copy through a subscription. The copy is renewed every `refreshMs`, in case the remote node restarted.
  - **Level:** the copy is what a client at `level` reads on the remote (default `public`: the mirror subscribes with that ceiling, §13.1), and only clients at `level` or above get it. `Access::Admin` copies whatever the remote gives the gateway, for admins only. For a client below `level` the node is unmirrored: `null` in parent reads, and its subscriptions to the node are forwarded.
  - Parent reads include the copy, respecting `depth`.
  - Local subscribers get the remote's changes as changes of the remote node, and its events under the local path (`/kitchen/button/pressed`).
  - Subscribing to a mirrored remote node itself is a local subscription to the copy, for a client the copy is for.
- **Forwarded subscriptions:** a subscription below a remote node, or to an unmirrored remote node itself, is forwarded. This is how a browser on the gateway follows a NowTP node's state without a mirror.
  - **Shared upstream:** local subscribers of the same path and level share one upstream subscription to that path, made with their level as its ceiling (§13.1); subscribers at another level have their own. It asks for what they need together, no more: the deepest `depth`; the union of their `keys` when all have keys, else every child but those all of them `exclude`. The gateway keeps that state as a copy, merged with each notification. A whole tree, asked for a UI showing a few values, would cost the remote, the link and the gateway's heap.
  - **Wider, narrower:** a subscriber that needs more than the copy holds waits while the upstream subscription is renewed wider, then gets its snapshot from the new copy; one the copy covers is answered at once. When a subscriber leaves and the others need less, it is renewed narrower at once. A renewal that only changes what the copy holds tells the other subscribers nothing. `upstreamSpec(localPath, keys, exclude, depth, level)` shows what is asked for.
  - **Snapshots:** the first subscriber's reply waits for the upstream snapshot (a deferred reply, so `maxPending` applies; transports that can't wait get `not_allowed`). Later ones are answered from the copy at once.
  - **Filtering:** each local subscription's `keys`, `exclude` and `depth` are applied by the gateway, to its snapshot and to every relayed change. Unknown keys are not reported, since the gateway doesn't know the remote's tree. `interval` and `remotes` are taken from the first subscriber (`interval` only from one that wants changes); changes are relayed as they arrive, except to `"changes": false` subscribers.
  - **Events** under an upstream subscription's path come out under the local path, like a mirror's. The remote sends an event once for each level the gateway's subscriptions have (`"as"`, §10.1), and the gateway passes each on at its level: to the forwarded subscribers at that level it concerns, and, from the mirror's level, to the local subscriptions above the node that may read the copy.
  - **Lifetime:** the upstream subscription ends with its last local subscriber (unsubscribe, or the client going away). It is renewed every `refreshMs` (default 30 s; every 2 s after a failed renewal) in case the remote restarted. If the renewed state differs from the copy, every subscriber that wants changes gets the whole state as one change.
  - **Limit:** two remote nodes of one gateway that reach the same remote path through the same endpoint, at the same level, share that path's subscription on the remote, so unsubscribing one ends both. Mount a peer once.
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

- **Mounting:** each peer that advertises TesserAPI on the transport's port becomes a remote node under the parent, named after its advertised name (NowTP: its discovery name). Characters a name can't hold become `-`. If the name is empty or taken, the last three bytes of the device's MAC (or, without one, of its address) are appended (`tesser-lamp-75f303`). `mountPeers(parent, intervalMs)` also mirrors them; each call adds a parent, so one endpoint can mount its peers in several places (calling it again for a parent only changes the interval). A peer already mounted under the parent isn't mounted again. Remote nodes of the peer elsewhere in the tree (declared ones) don't prevent it, but they share its online state and schema hash, including remote nodes declared after the announcement.
- **One node per device:** a parent mounts each device once, whatever the transport. A NowTP peer is its MAC; a UDP peer advertises its Wi-Fi station MAC (§10.7), the address NowTP uses. The first transport to announce a device mounts it; another transport's announcement of the same device adds a route to that node (§14.3), so requests can go either way. That keeps a `*` fan-out (§6.5) from reaching a device twice. (A node that runs ESP-NOW from its soft-AP interface is reached from its AP MAC, so it would be mounted twice.)
- **Declared devices:** a node declared with an address (`peers.remote("far-lamp", udp.endpoint(), udpAddress(10, 0, 2, 7, 8484))`; pass the name as a `std::string` when it is made at run time, and the node keeps a copy; for a device on another subnet whose advertisements don't arrive) is the same device as a mounted one: when that address advertises, the node takes the device's MAC, and the device's other transports add their routes to it rather than mounting it again. `RemoteNode::byDevice(parent, mac)` finds the node of a device (under the API lock).
- **A node's own id:** each node shows its device at the root of its schema, `GET /?view=schema` → `{"type": "object", "id": "c8f09ef4a67c", ...}`, the same value its stub shows on a gateway, outside the hash; and `HttpServer::advertise()` puts `id=<12 hex digits>` in its mDNS TXT record. It is the Wi-Fi station MAC, or what `Api::setDeviceId()` set (a node whose peers know it by another MAC). So a client can tell when a node it reaches directly and one it reaches through a gateway are the same device.
- **Nodes stay** unless the cap below needs the room. A lost peer stays mounted and shows offline; it comes back online as soon as it is heard from or announces itself again. A node also probes its offline routes with a get at depth 0, so a device that doesn't advertise comes back when it answers: first 5 s after a route is lost (`probe(ms)`, 0 to stop), then with the wait doubling up to a minute, so a device that is gone for good costs a datagram or so a minute (a reliable one over UDP: about six with its retransmissions).
- **What peers can make a node hold is bounded** (`DatagramEndpoint::Options`, 0 for no limit), since over UDP a peer is whatever address and device id it announces:
  - `maxMounted` (16): nodes an endpoint mounts for announced peers. At the cap, a new peer evicts the mounted node heard from longest ago among those nothing uses (offline, reached through this endpoint only, without a subscription at or below it, a call in flight, a relayed transfer or a task calling it, §9); with none, it isn't mounted (`Stats::unmounted`; `evicted` counts the others). Nodes the application declares are neither counted nor evicted. An evicted node is unlinked under the lock that walks without the API lock take (`Api::owner()`), then destroyed, so such a walk sees it whole or not at all; the schema revision changes, and a subscriber of its parent learns it is gone when it reads again.
  - `maxRoutesPerNode` (4): routes announcements add to one node through an endpoint (a device announced from many addresses). At the cap, the offline route heard longest ago is pointed at the new address; with none offline, the address isn't added.
  - `maxFileSessions` (4): peers with a file transfer open at once. A session with nothing in progress (a download past its last chunk) makes room; otherwise the new transfer is answered `busy` (`transfer_busy`, "too many transfers in progress").
  - A peer holds a subscriber only while it has a subscription (one refused leaves nothing), and the announcements remembered for routes added later are as many as the peers known (32).
- **Message limits per peer:** each node advertises the largest message it takes on a link (`"max"`, its endpoint's `Options::maxMessage`, §10.3, §10.7), and a node sends each peer no more (`DatagramEndpoint::peerLimit()`: the least of what the peer advertises, what the link knows of it, NowTP's `peerMaxMessageSize()` with NowTP deadc54 and later, and the link's own limit; none known, no limit). Replies to a peer stay within it (`replyLimitFor()`): a download chunk shrinks to fit (`Reply::limit()`), and a larger reply is `too_large`. A request larger than it is refused at once rather than sent to be lost or refused (`too_large`, "the remote node takes messages of up to N bytes", for a forwarded one; `Stats::tooLarge` counts the others), and a relayed upload goes in parts it takes. A relayed download asks for no more than the gateway's own link takes. Before this, a fleet had to share one limit: a gateway with a 16 KB NowTP limit could answer a peer configured for 4 KB with replies that never arrived.
- **Remote node stub:** the parent's schema shows each remote node's state. `"online": false` appears while the peer is lost, `"route"` names the route in use when the node has several (`"udp/ethernet"`, `"nowtp"`, §14.3), `"schema"` is the hash the peer advertises, so a client can cache the peer's own schema under it, `"id"` is the device's MAC (12 hex digits) when known, so a client can refer to a node by device rather than by name, and `"protocol"` is the version it advertises (§10.1.2; 1 for an advertisement that predates versions, absent while unknown). None of these is part of the hash (§7):

  ```json
  GET /peers?view=schema&depth=1
  {"type": "object", "children": {
     "tesser-lamp": {"type": "remote", "route": "udp", "schema": "1a2b3c4d", "id": "10521c75f37c", "protocol": 2},
     "heliostat-3": {"type": "remote", "online": false, "schema": "99f0e1aa", "id": "8cbfea8ff250"}}}
  ```

- **Watching the list:** mounting a peer, a change of its online state or route and a new advertised hash all mark the remote node changed. Only the mount changes the gateway's own schema hash (§7). A subscriber of the parent (depth 1) gets `{"heliostat-3": null}` for an unmirrored node, which is its cue to re-read the parent's schema.
- **Generic core:** `DatagramEndpoint::mountPeers()` and `peerSeen(address, name, schemaHash, device)` do the work, host-tested with the in-memory link. The NowTP adapter only parses advertisements and forwards peer events; `UdpLink` reads its own (§10.7). Online state comes from the endpoint: `forgetPeer()` marks the peer's routes offline, and any message from the peer marks them online; a node is online while one of its routes is.

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
- **`LocalClient`** goes through `Api::handle()` like a transport, as a `Subscriber`, so subscriptions, deferred actions, queued mode and remote nodes behave as they do for remote clients. Its requests count as authenticated, with `admin` access, unless `setAuthenticated(false)` (`public`). `setAccess(level)` makes its requests and subscriptions act at that level, like an HTTP client whose token has it (authenticated unless `public`): a bridge to an outside protocol such as MQTT uses `Access::User` so that its clients can't reach admin settings, and its notifications leave out what a user can't read. Destroying it drops its subscriptions.
- **`PeerClient`** sends through a `DatagramEndpoint` and gets the notifications its peer sends to that endpoint. These are per peer, not per client: two clients of the same peer on one endpoint both see both clients' notifications. Destroying it unsubscribes what it subscribed to.
- **Bodies** are JSON text in, `JsonVariantConst` out. A response that arrives after its client is gone is dropped.

### 14.3 Routes and streamed sets

A device can be reachable several ways, over Ethernet (UDP) and ESP-NOW (NowTP) say. Its remote node then holds one route per way, and a stream of writes to it keeps its order whichever way it goes:

```cpp
tesser::RemoteNode::setDefaultRouteOrder({tesser::TransportKind::Udp, tesser::TransportKind::NowTP});
udp.mountPeers(api.object("peers"));
nowApi.mountPeers(api.object("peers"));          // the same devices: routes, not new nodes

tesser::RemoteNode* node = tesser::RemoteNode::byDevice(peers, mac);
node->stream("/outputs", "{\"3\":{\"on\":true,\"frequency\":440,\"duty\":78}}");   // never waits
```

- **Routes:** a route is an endpoint and the device's address on it, with its own online state, the time it was last heard (`heardMs`) and the smoothed round trip of the calls made on it (`rttMs`). `mountPeers()` on several endpoints under one parent gives a device's node one route per endpoint (§14.1); `addRoute(endpoint, address)` adds one by hand. `routes()` lists them in the order they are tried, and `route(r)` gives the one in use.
- **Order:** requests go by the first online route in the order; with none online, by the first. The order is a list of `RouteKind`s, a transport and optionally the interface the route leaves by: `udp/ethernet`, `udp/wifi` (the station), `udp/ap`, or plain `udp` for any interface (a `TransportKind` converts). `api.remoteHooks().setRouteOrder()` sets it for an Api's nodes (process-wide: `RemoteNode::setDefaultRouteOrder()`), `node.routeOrder()` for one (an empty list falls back to the default). Routes the list doesn't cover come after, in the order they were added; with no order at all, the first transport to announce the device is preferred. A new order takes effect at once. `parseRouteOrder("udp/ethernet, nowtp", order)` and `formatRouteOrder(order)` convert the text form; `toString(TransportKind)` and `toString(NetInterface)` name the parts.
- **Interfaces:** a route's interface (`Route::iface`) is the one its endpoint says reaches the address (`DatagramEndpoint::classifyInterfaces()`; UdpTransport's `interfaceOf()`, §10.7), taken when the route is added and each time it comes back online. A device on two subnets, say Ethernet and Wi-Fi, announces itself from both addresses with one MAC and gets a UDP route for each, both on the one endpoint. When the device's interfaces share a subnet, a route's interface is unknown (plain `udp` covers it): to be sure UDP leaves by Ethernet, keep it to Ethernet (`setInterfaces()`, §10.7). `Route::name()` gives `"udp/ethernet"`, or `"udp"` when unknown.
- **Failover:** a route goes offline when its endpoint forgets the peer: over UDP when a message isn't acknowledged within `sendTimeoutMs` (3 s), or the device says goodbye or restarts; over NowTP when discovery reports it lost. Requests then take the next route, and come back to the preferred one as soon as it is heard again: an advertisement, any message, or the probe of §14.1. Calls in flight on a lost route fail with `timeout` (forwarded requests aren't sent again: a set may have been applied); streamed sets go again on the next route (below).
- **File transfers stay:** a relayed transfer (§14) goes by the route it opened on, start to end, whatever the node's route in use becomes: the device keeps a transfer per client, and the gateway by another link is another client, which has none. Measured on DEVKIT-V3 relaying a firmware image to DEVKIT-R1 before this: the last chunk went by the other route mid-transfer and got `bad_request` ("no upload in progress at this path"), and R1 dropped the image unfinished. If that route goes offline or away, the next part fails at once with `timeout`, except while its peer's loss waits out the grace after a link's loss (`DatagramEndpoint::lossPending()`, §10.3): the peer was heard within the last 5 s, and its next reply brings the route back (`peer_offline`, "the route the transfer started on is offline", or "peer offline" with no route online), which ends the relay; the client starts again, by the route in use then. The device drops its side when its link loses the gateway, or once idle for `transferIdleMs`.
- **Subscriptions follow:** on a switch, the mirror and forwarded subscriptions subscribe again on the new route, at once; when the switch is back to a preferred route while the other still works, the old subscriptions are dropped. Notifications are taken from the route in use only, so an event isn't delivered twice.
- **State:** `online()` is true while one route is. The schema stub shows `"route"` when there are several (§14.1). A state hook, `api.remoteHooks().onStateChange(fn)`, runs in an endpoint's `process()`, without the API lock, when one of that Api's nodes is created or gets a route and when its online state or route in use changes, so an application can apply stored settings to a device when it first appears and report failovers.
- **Hooks per Api:** `Api::remoteHooks()` holds an Api's state and access hooks (`onStateChange()`, `onAccess()`, several of each, each returning an id for `remove()`) and its route order (`setRouteOrder()`), so two applications in one program, or two TesserKIT devices, don't replace each other's. A node finds its Api on its first tick (once a second until it is in one); until then, and beside the Api's, the process-wide `RemoteNode::onStateChange()`, `onAccess()` and `setDefaultRouteOrder()` apply, one of each for every node, which a second caller replaces. A node's own order (`routeOrder()`) comes first, then its Api's, then the process-wide one. Before this, only the process-wide ones existed (review of 2026-10-14).
- **Streamed sets:** `node.stream(path, bodyJson)` writes below the node without waiting, from any task, under the API lock or not. It returns at once: `ok` (queued), `timeout` (the node is offline), `busy` (the queue is full), `bad_request` (not JSON).
  - **In order:** at most one streamed set per node is in flight; the others wait in a queue of `streamLimit()` sets (8). So they are applied in the order they were made, over UDP and NowTP, which don't keep order, and across a route switch, without anything new on the receiving side.
  - **Merged:** a set to a path that is already queued is merged into the queued one, as a patch (§6.3): objects merge, anything else replaces. `{"3":{...}}` and then `{"5":{...}}` to `/outputs` go as one patch, and the newest value of each key wins. A set to a path above or below a queued one isn't merged across it. Streams converge on the latest state: they don't replay every set. A note on and its note off made while a set is in flight go as the note off alone, so a note shorter than a round trip (or than a retransmission, during a loss) never sounds. Streams are for state: two action calls merge into one.
  - **Again:** a set whose reply doesn't come within twice the node's timeout (longer than the link takes to give up, so it can't still arrive later), or that the device answers `busy`, goes again, with the sets queued for its path merged on top, at most three times; on the next route if its route was lost.
  - **Dropped:** a set the device refuses (an error reply) is dropped; so is everything queued once the node is offline, or a set out of attempts. `onStreamError(fn)` hears of each (`path`, status, the device's error body), in an endpoint's `process()`. `streamStats()` counts sets queued, merged, sent and failed; `streaming()` those queued or in flight.
  - **Cost:** a set in flight takes one of the endpoint's calls (`Options::maxCalls`, 8): raise it to the number of nodes streamed to at once. A set that finds no call free, or no room on the link, waits for the endpoint's next `process()`. Streams don't take the API's pending slots. A lost datagram holds a node's queue until the link sends it again (100 ms over UDP); meanwhile the queue keeps only the latest state.
- **Generic core:** routes and streams live in `RemoteNode`, host-tested with UDP and NowTP links that lose datagrams.

### 14.4 Driving a node: the writer and its loss

A node whose outputs another node drives (a master playing MIDI to its slaves) needs to know who writes, and when that writer is gone, to undo what it did.

```cpp
api.object("outputs").value("3", out3, [&](const Output& o) {
    const tesser::Client* c = api.currentClient();   // who is writing, during the request
    owner3 = c ? tesser::PeerAddress::fromBytes(c->device, c->deviceLength) : tesser::PeerAddress();
    out3 = o;
});
tesser::watchDevices({&udp.endpoint(), &nowApi.endpoint()}, [&](const tesser::PeerAddress& peer,
                                                               const tesser::PeerAddress& device) {
    tesser::Lock lock(api);
    releaseOutputsOf(device);                         // gone from every transport
});
```

- **The writer:** `Api::currentClient()` is the client of the request being handled (null outside one), for setters, actions and validators. Besides its transport and address, `Client::device` holds the MAC of the device it is, when the transport knows it: a NowTP address, or the `id` a UDP peer advertises (§10.7).
- **Peer lost:** `DatagramEndpoint::onPeerLost(fn(peer, device, reason))` runs in `process()`, without the API lock, when a peer the endpoint had heard from is forgotten. `reason` (`PeerLoss`, `toString()` names it) says why: `failed` (a delivery wasn't acknowledged; over NowTP the peer is reported but not forgotten, §10.3), `goodbye`, `restarted` (UDP: a new nonce; the peer is back at once, but what it held is gone), `silent`, `stopped` (this side's link), `lost` (NowTP's peer lost, after the grace for a peer heard lately, §10.3; or `forgetPeer()` without a reason), `auth_failed` (NowTP couldn't authenticate with it: another pairing key, §10.3; reported even for a peer never heard). `device` is its MAC when known. A peer is reported once, until it is heard again. A handler without the reason (`fn(peer, device)`) works too. `deviceOnline(mac)` says whether the endpoint has heard the device since it last lost it, and `deviceOf(address)` gives a peer's device.
- **Across transports:** a master reached over UDP and NowTP that fails over from one to the other is still there. `tesser::watchDevices(endpoints, fn)` takes the endpoints' `onPeerLost()` and calls `fn` only once the device is gone from all of them (a peer without a known device: when that peer is), and on every restart, though the device is back at once: a master that reboots mid-note must not leave its notes on. A device lost on two endpoints at the same moment may be reported twice. NowTP has no session nonce, so a master that reboots quickly is noticed as a restart over UDP only.
- **Silence needs advertisements:** a quiet client sends nothing, so silence only counts for peers that advertise: the master calls `UdpLink::advertise()` (with `setDeviceId()`, for the device to be known over UDP), or is in NowTP discovery. Goodbyes, restarts and failed deliveries are noticed either way.

## 15. Memory budget

Targets to be measured on a classic ESP32 and a C3, and enforced in CI:

| Item | Target |
|---|---|
| Flash, core + HTTP transport | ≤ 40 KB above the application's baseline, ArduinoJson included (measured below: 45.5 KB) |
| RAM per value leaf | ≤ 32 B with a bound variable, ≤ 60 B with a getter and setter, + 28 B with a range or description |
| RAM per object node | ≤ 36 B |
| Request handling | Request body document (bounded by `maxRequestBody`) + a streamed reply's window, up to 2 KB (1 KB on serial and Bluetooth lines; HTTP, WebSocket; it waits there to be sent without the API lock, §9), or the response buffer (NowTP and deferred replies, bounded by `maxResponse`) |
| Stack | Rendering depth bounded by `maxDepth` (default 16). JSON parsing: request bodies nest at most `maxDepth` + 1 levels (one more for an envelope); responses, notifications and stored records up to 65, the most a node's JSON writer produces, since a schema nests two or three levels per tree level and the writing peer's `maxDepth` may differ. On Xtensa with `-Og` (ESP-IDF's default), each tree level costs about 230 B of stack to render a schema or apply a patch, and each JSON level about 96 B to parse (6.2 KB at 65) |

**Large buffers on a fragmented heap.** Without PSRAM, an ESP32 running Wi-Fi, NowTP, UDP and Bluetooth can have 20 KB free in fragments, and an allocation that fails aborts the device (no exceptions). So:

- A buffered reply (NowTP, UDP, deferred replies) grows by half again at a time (by just what it needs when the half doesn't fit: a reply of nearly its final size isn't refused for growing past it), to exactly that capacity (`reserveExactly()`: libstdc++'s `reserve()` on a string that has allocated takes at least twice its old capacity, more than was counted), and before growing past 1 KB it asks for the largest free block (`largestFreeBlock()`, `heap_caps_get_largest_free_block()` on target). It must hold the new buffer, one copy of the message further down (a link's own buffer, NowTP's), and 1 KB of headroom. Otherwise the reply is refused with `busy` and "not enough memory for the response", which the client can try again. Measured on DEVKIT-R1 before this: a 4.8 KB subscription snapshot over UDP aborted the device when the link copied it.
- Every reply that buffers (envelopes, deferred HTTP replies, fan-out matches, `LocalClient`, `StringReply`) writes through a guarded `StringSink`, so a reply that doesn't fit answers `busy` wherever it is built. Other buffers (stored records, request envelopes) aren't guarded: cut short, they would be wrong rather than refused.
- Change notifications are rendered straight into their envelope (the body isn't built apart and copied), behind the same check. A notification that doesn't fit now is put off, and its changes go with a later flush, merged with newer ones: after 50 ms, then waits that double up to 2 s while it still doesn't fit, back to every flush once one goes. Measured on DEVKIT-R1 before this: a 5-6 KB notification was built and refused every 10 ms, each `poll()`. A relayed one (a forwarded subscription) that doesn't fit is dropped and marked missed, so the next carries `"overflow": true`.
- The datagram links take a reply without copying it: `DatagramEndpoint::SendFn` takes the message by value, and UdpLink keeps the very buffer for retransmissions. A link that sends from a malloc()ed buffer it takes over registers `setOwnedSend(fn, slack)`: replies, deferred ones included, are then built in such a buffer (`Bytes`, grown through the accountant, with `slack` bytes spare after the envelope) and handed over whole (`EnvelopeReply::sendOwned()`). NowTP does (`sendBuffer()`, NowTP ce6b645 and later, 32 bytes of slack for its 21-byte trailer), so a NowTP reply exists once, in the buffer NowTP sends from; with an older NowTP it is copied as before. A notification is still copied once per subscriber. Receiving, UdpLink hands its reassembled message to the endpoint without a copy (`receive(from, std::string&&, ...)`), and drops a message whose reassembly buffer the heap can't give (the sender sends it again).
- A gateway relays a remote's `ok` reply as the remote wrote it (`requestRaw()`, the body found in the envelope by `jsonMember()`): it isn't parsed into a document and rewritten, which held a large body twice over. Errors are parsed as before, to map their paths. A forwarded subscription compares its renewed state with its copy in place, not as two serialized strings.
- **A floor for the heap as a whole:** a guarded buffer growing past 256 bytes must also leave `heapFloor()` free, after its growth and one more copy of it (the link's) (`setHeapFloor(bytes)`; 8 KB on target by default, off on the host), since a heap can run out in many small blocks as surely as in one large one: measured on DEVKIT-V3 as a gateway streaming a peer's whole tree to two clients under load, the lowest free heap fell to 3.5 KB and a 481-byte growth aborted. Buffers under 256 bytes (error replies) are never refused, so a client always hears back.
- **Message transports' own buffers:** a serial or Bluetooth line grows through the accountant (`growFor()`); a line the heap can't hold is read to its end and answered `busy` (`low_memory`, "not enough memory for the line") under its id (the line's first 64 bytes are kept for it, as for a line too long), since a client waits for the reply with its id, and a line over 512 bytes gives its buffer back once handled, so an upload's lines don't keep 4 KB from the download after them. A WebSocket frame reserves its size before it is read (above the floor, in one block with 256 bytes to spare rather than a reservation's 1 KB: its bytes are already in the network stack's buffers, and it goes once handled), and is answered `busy` (`low_memory`, without an id: it is in the payload) otherwise; the payload is then read and dropped in 128-byte pieces, so the connection stays. Measured on DEVKIT-V3 as a gateway before this: 11 KB free with no block over 4.6 KB refused a 3.9 KB frame, and the client's connection was closed mid-upload. Going out, a frame over 1 KB waits, up to 500 ms, for the heap to hold twice its size above the floor (`heapAbove()`), while the frames before it are acknowledged, since the stack keeps its copies until then (TCP's segments, then the radio's buffers): relaying 3.9 KB frames before this, a send took V3's free heap down by 3.7 to 6.5 KB (to 5.7 KB from 9.4, and to 5.6 KB from at least 12.1). A file download over HTTP waits the same way before each 2 KB piece, whatever the piece's size: sent as fast as the socket took them, a 100 KB download from V3's files held its free heap at 4 to 6 KB for 2.5 s and fell to 1.4 KB, where TesserKIT's next unchecked allocation aborted it; paced, the same download ran at 24 KB/s instead of about 40, and the lowest free heap of the whole files suite was 5.3 KB. Notifications waiting behind a streamed line reply grow the same way, and are missed (`overflow`) without room.
- **Chunks sized to the room:** a file chunk is sized when it is asked for, to what the heap holds then (`heapRoom()`: the free heap above the floor, less what other reservations are about to take, and the largest block), rather than refused at its full size (`FileSession::chunkInRoom()`). Each data byte is counted with what it holds at once on its way through the node: a download chunk read here, 5 bytes (the chunk, its reply's base64, and the network stack's copies of that reply in flight, lwIP's and the radio's); a download chunk relayed from a peer, 16/3 (the peer's reply, the gateway's, and the stack's two); a part relayed to a peer, 4 (its envelope and the link's copies of it in flight, its frames then the radio's, about twice it); an upload chunk written here, 2 (decoded, and the sink's copy), and one relayed, 5 (decoded, and the part it goes in with the link's copies); after 1 KB for the rest of the envelope. The decoded chunk must also fit one free block with the reservation's headroom. An upload takes the start of a chunk that doesn't fit, whole base64 groups decoded from the envelope's text, and says how much in `received` (its `crc32` is still checked over the whole chunk, from the text). A shrunk chunk goes down in steps of 384 bytes, at least one, which its buffers' reservations still take or refuse: buffers of a few sizes, which the heap reuses, rather than a new size each time. Measured on DEVKIT-V3 as a gateway with the sizes free to vary: 11 KB free, but no block over 3.8 KB, refused a client's 2880-byte chunk (`busy`) for 11 s, until the client gave up. Measured on DEVKIT-V3 as a gateway running TesserKIT before this, relaying 2880-byte download chunks from DEVKIT-R1 to a WebSocket client: its lowest free heap fell from 11.4 KB to 3.7 KB, then 1.7 KB, each time inside the WebSocket frame's send (`httpd_ws_send_frame_async()`), the stack copying a 3.9 KB frame while the reply was held twice (the deferred reply, and `wsSend()`'s copy of it, which a reply now hands over instead). With both, in the same suite (a 20 KB file each way and a 1.7 MB firmware image relayed to R1), relayed download chunks came down to about 1 to 1.3 KB and the gateway's lowest free heap was 9.4 to 10 KB, set inside NowTP's `sendBuffer()`, in two runs of three; in the third, 6.9 KB, outside the library's own steps, while one relayed part's reply took 3.2 s.
- **A transfer's state** (a session's, about 400 bytes; a relay's, 512) and a reply detached to wait for a remote node are reserved before they are made, and refused `busy` (`low_memory`) without room.
- **File envelopes:** an `upload` envelope is parsed without its `data` (an ArduinoJson filter, `uploadFilter()`), which is read from the message's text and decoded once; a strict (`"v": 2`) upload checks its members from the text too (`jsonKeys()`). A download's `data` is base64-encoded straight into the reply (`JsonWriter::base64()`), not built in a string first. The chunk read from the file and an upload's decoded data are reserved before they are allocated, and answered `busy` (`low_memory`, "not enough memory for the chunk") without room; a refused upload chunk leaves the transfer open, to be sent again. Measured on DEVKIT-V3 before this: serving a 8 KB download chunk over serial aborted it on an 11 KB string.
- **Received messages:** a link that only lends its buffer (NowTP's callback) has a message copied only when the endpoint's incoming budget, the heap floor and the largest block allow it (below), and a large message (over 1 KB) is parsed only with room for twice its size above the floor, held while it is handled. A message refused either way isn't left to time out. Over a link that can refuse it (NowTP f09a571 and later, §10.3), a reliable one is refused there and its sender sends it again. Otherwise a request is answered `busy` (`queue_full` when the endpoint's incoming budget is full, `low_memory` when the heap is short), and a reply fails its call at once with the same, read from the message's `id` alone (a relay asks for a smaller part on `low_memory`, §14). A remote's state goes into a gateway's copy (a forwarded subscription, a mirror) only with room for twice its text; otherwise the subscribers waiting for it hear `busy`, "not enough memory for the remote's state", and the subscription is tried again. Measured on DEVKIT-V3: a 4.8 KB NowTP reply, copied out of NowTP's buffer, was what aborted it once the rest was guarded.
- **A mirror's copy** (§14) is parsed once, from the snapshot reply's text (`requestRaw()`), straight into the copy, the old one cleared first, and only with room for three times that text in the byte budget (a document of small values takes about that); without, there is no copy until the next try, 2 s later. Measured on DEVKIT-V3 mirroring DEVKIT-R1 before this, when the snapshot was parsed as a reply and then copied, held three ways at once: R1 coming back after a restart took V3's free heap about 2 KB below its floor.
- **Admission:** a forwarded subscription (§14) that needs a new or wider copy of a remote's state is refused `busy` ("not enough memory for another remote subscription") unless the free heap is twice the floor; one the shared copy already covers is answered from it as usual. UdpLink also drops a message whose reassembly would cross the floor.
- **One byte accountant** (`tesser/budget.h`): memory is bounded by bytes, not by counts of messages each up to 16 KB (16 incoming and 16 outgoing of those, and 32 KB of reassembly, were 5 to 10 times a gateway's free heap). A buffer reserves its bytes from a `ByteBudget` before it is allocated (`reserve(bytes, key)`, a move-only `Reservation`) and gives them back when freed; refused, the caller answers `busy` or drops, as above.
  - **Budgets nest:** a reservation counts against its budget, a cap per key (a peer), and every budget above it up to the root, `memoryBudget()`: no limit unless the application sets one (`setLimit()`), but it keeps the heap floor and the largest block.
  - **Checked once for everyone:** the root's heap check, under one lock, counts what other reservations are about to take (reserved, not yet `settle()`d once allocated: `unsettledBytes()`), so two tasks checking at once can't both count on the same free bytes, which they could with separate `heapAbove()` checks. Guarded buffers' growth (`growFor()`) reserves the same way.
  - **Where:**

    | Budget | Holds | Default (total, per peer) |
    |---|---|---|
    | endpoint `incoming` | messages waiting for `process()` (`Options::queueBytes`, `queueBytesPerPeer`) | 20 KB, 16 KB |
    | `udp outgoing` | reliable messages awaiting acknowledgement (`UdpLink::Options::outgoingBytes`, `outgoingBytesPerPeer`) | 24 KB, 16 KB |
    | `udp reassembly` | messages being reassembled (`reassemblyBytes`, `reassemblyBytesPerPeer`); the sender's own oldest dropped first | 24 KB, 16 KB |
    | `httpd queue` | WebSocket frames and stream chunks handed to the httpd task (`HttpServer::queueBudget()`, per connection) | 16 KB, 8 KB |
    | root | queued-mode copies of requests, events' shared messages, a gateway's copy as a change merges into it (twice its text; refused, the copy is taken again whole), guarded growth | the heap floor |

  - **Shared notifications:** an event is built once per variant (level, overflow) and handed to every subscriber it goes to as one immutable buffer (`Subscriber::notifyShared()`), counted once while any holds it; WebSocket keeps it in its frame rather than a copy. Change notifications differ per subscription and are built each.
  - **The link's message size:** an endpoint knows the largest message its link carries (`Options::maxMessage`: UdpLink's `maxMessage`, `NowTpTransport::endpointOptions(maxMessageSize)` for NowTP's `Config::maxMessageSize`, 16 KB by default). A reply larger than it (or than `Config::maxResponse`) answers `too_large`, so the client reads in pieces, and a larger notification is missed (`overflow`), rather than either being rejected by the link. Every node of a link must accept what the others send, so give them all the same size. `replyLimit()` tells it.
  - **For applications:** `tesser::reserveBytes(bytes, contiguous)` takes from the same root, so a TLS session, a download or a backup and the library's buffers can't both count on the same free heap: keep the reservation while the memory is used. `ByteBudget::used()`, `peak()` and `refused()` show each budget. `HttpServer::workStats()` counts the items handed to httpd and those that ran: a difference that stays says items are waiting, or were lost.
- `setLargestFreeBlockProbe()` and `setFreeHeapProbe()` replace the probes in tests. Note that on ESP-IDF, with exceptions off, `new (std::nothrow)` aborts like plain `new` instead of returning null: these checks come before allocating for that reason.

Measured on a classic ESP32 (ESP-IDF 6.1, heap overhead included, `test/hardware`; the node sizes were measured with 16-bit generations, and 32-bit ones add 4 bytes, from `sizeof`):

| Item | Measured |
|---|---|
| `Api` object | 324 B (two statically allocated FreeRTOS mutexes make most of it) |
| Value bound to a variable | 32 B |
| Value with getter and setter (small captures) | 60 B |
| Object | 36 B |
| Value with a range | 68 B (32 B node + 36 B metadata) |
| List of any length | one node; elements are built per request |
| Full GET of a 13-leaf tree (227 B response) | 0.43 ms |
| Envelope parse + keyed GET + reply | 0.9 ms |
| Peak stack of an HTTP request (schema, patch, shape) | about 4.9 KB |
| Peak stack of a serial (UART task) request | about 3.5 KB |
| Serial round trip at 115200 baud | 13 ms (mostly wire time) |
| HTTP GET over Wi-Fi, new TCP connection each time | 55–85 ms median |
| NowTP request and response between two boards | 13–14 ms median, fragmented 1.5 KB responses included |
| UDP request and response, Mac to board over Wi-Fi | 19–23 ms median; a 4 KB schema (three datagrams) included |
| GET through a gateway over UDP (HTTP, then UDP to the remote node) | 120 ms median, the gateway's own GET 52 ms |
| GET through a gateway (HTTP, then NowTP to the remote node) | 85 ms median |
| File transfer in envelopes (§10.1.1), 12–20 KB in the device's chunks | serial at 115200 baud: 8 KB/s each way (the wire's limit with base64); WebSocket: 60 KB/s up, 52 KB/s down; NowTP: 15 KB/s up, 20 KB/s down, each chunk relayed by an HTTP call to the other board; UDP from a Mac: 90–100 KB/s up, 115–130 KB/s down |
| UART task stack during file transfers | 2.6 KB free, 290 B less than requests alone |

**Flash:** `examples/idf/http_api` against its `FOOTPRINT_BASELINE` build (the same app on bare `esp_http_server`):
- **Total:** 45.5 KB for the core, the HTTP transport and ArduinoJson. With `TESSER_NO_SCHEMA`, `TESSER_NO_MESSAGES` and `TESSER_NO_DESCRIPTIONS` it is 43.8 KB.
- **Against the target:** the 40 KB target was set before subscriptions, events, lists and authorization joined the core. 34.6 KB was measured for core + HTTP at phase 2.
- **Linked only when used:** remote nodes (and the datagram client), persistence, WebSocket, NowTP and UDP. Remote forwarding is virtual and persistence is a hook installed by `persistence()`, so applications that don't use them don't link them.

**No leaks:** running the whole host test suite a second time on the chip moves the heap by exactly the two nodes the duplicate-name test deliberately leaves unlinked.

Configuration (`tesser::Config`, at runtime):

| Option | Default |
|---|---|
| `maxRequestBody` | 4096 |
| `maxResponse` (buffered replies: NowTP, deferred and queued replies) | 8192 |
| `maxDepth` (levels rendered; request bodies nest at most one more) | 16 |
| `deferTimeoutMs` | 10000 |
| `maxPending` | 4 |
| `transferIdleMs` (a file transfer idle this long is taken over, §4.6) | 30000 |

Compile-time switches: `TESSER_NO_SCHEMA`, `TESSER_NO_DESCRIPTIONS`, `TESSER_NO_MESSAGES`, `TESSER_NO_UI`.

## 16. Packaging

The layout follows NowTP:
- `CMakeLists.txt` is an ESP-IDF component under `idf.py`, and a host library plus tests otherwise.
- `idf_component.yml` depends on `bblanchon/arduinojson` as a public requirement (`require: public`), since TesserAPI's headers include ArduinoJson's: any component that requires TesserAPI can include `TesserAPI.h`.
- The component's `Kconfig` selects `CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT` when WebSocket support is on (§10.5).
- `library.properties` and `library.json` for Arduino and PlatformIO.

**Source files.** A remote node (§14) is split into units, each in its own file. A unit holds its state and its lock (§9), and `test/remote_unit_cases.h` tests it without a node or a link:
- `remote.cpp`: the node itself. It forwards requests, passes the peer's notifications on, presents tokens and calls the hooks.
- `remote_routes.cpp` (`detail::RouteTable`): the routes (§14.3). This covers their order, the route in use and its switches, online state, probes, and the device and token that every route shares. It holds a leaf lock and calls nothing while holding it. `route.h` has the public types (`RouteKind`, `RemoteNode::Route`).
- `remote_relay.cpp`: relayed file transfers (`FileRelay`), which use only the node's public interface.
- `remote_stream.cpp` (`detail::SetStream`): the queue of streamed sets (§14.3), its merging and retries, under its own leaf lock. The node sends what the queue gives it.

The Bluetooth transport (§10.6) is a header that `TesserAPI.h` doesn't include, so that only the firmware using it compiles a BLE stack. On ESP-IDF, its source compiles to nothing unless NimBLE is enabled.

The UDP transport (§10.7) needs only lwIP and `esp_netif`, which every ESP-IDF and Arduino-ESP32 3.x build has: it is always compiled, and linked when used. On ESP-IDF `esp_netif` is a private requirement.

Optional transports (NowTP) compile only when their dependency is present: `__has_include(<NowTP.h>)` on Arduino and PlatformIO. On ESP-IDF, the component looks for a `nowtp` component among the build components and links it, since requirements are resolved before that list is known.

## 17. Testing

1. **Host tests** (CMake + ctest, ASan and UBSan, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror` on the library). They cover the core, the envelope codec, the line transport, and the datagram endpoint and UDP link over in-memory networks (losses, repeats, broadcasts and discovery included).
   - **A heap with a size** (`tesser/testing/heap_limit.h`, header-only and standalone, for NowTP's and TesserKIT's tests too): it replaces the test program's `operator new` and `delete` and counts what they hold. An allocation past the limit aborts, as on ESP-IDF, where exceptions are off. `malloc()` isn't counted: it fails softly on target, and the library checks it. A floor rule also flags an allocation over 320 bytes that leaves less than the floor, less 64 bytes for the allocator's rounding: the byte accountant never lets a checked one do that (§15). `HeapExempt` marks what a test's client or simulated link holds, which neither counts nor aborts. `onExceeded()` reports instead of aborting; `TESSER_HEAP_DISCOVER=1 ./tesser_tests heap_` prints a backtrace for each.
   - `test/heap_cases.h` runs client-reachable scenarios at every heap size from the floor to 24 KB above it, with the library's free-heap probes reading what is left: transfers relayed through a gateway, local and forwarded subscriptions to large values, and envelopes over a serial line. Each must be refused at the smallest heap and go through at the largest, without an abort in between. The floor is 2 KB, and 4 KB where a gateway and its device share the heap. What these found: a relayed upload's first chunk copied into callbacks, a text upload held unchecked, transfer and deferred-reply state allocated unchecked, and, with libstdc++ only, a notification's closing bytes that doubled a string the sink had grown to its exact size.
2. **On-target tests** (`test/hardware`, ESP-IDF): the same core cases on the chip, plus heap and stack measurements.
3. **End-to-end on hardware:** `test/hardware` serves a demo API over UART, HTTP and WebSocket (with Wi-Fi credentials from the gitignored `test/secrets.h`), and NowTP. It drives subscriptions, events, access control, persistence and the gateway:
   - `test/e2e_serial.py`, `test/e2e_http.py`, `test/e2e_ws.py`
   - `test/e2e_nowtp.py` and `test/e2e_gateway.py`, on two boards
   - `test/e2e_udp.py`, on two boards and the machine running it, over `test/udp_client.py`, a Python implementation of the UDP link
   - `test/e2e_routes.py`, on two boards that reach each other over UDP and NowTP: streamed sets in order while one board's UDP goes off and on (§14.3)
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
| 7 | Groundwork for [TesserKIT](https://github.com/leokeba/TesserKIT): access levels and token checks, secret values; typed object arguments, keyed lists, scalar arrays, file nodes, streamed envelope replies, `view=hash`, re-advertisement, `Storage::erase()` and `Api::restore()` | done |
| 8 | Bluetooth LE transport (Arduino and ESP-IDF), for TesserUI's launcher | done |
| 9 | Fan-out writes: a `*` path segment in Set, for TesserKIT's sequencer | done |
| 10 | Streamed subscriptions over plain HTTP, for TesserUI on HTTPS pages | done |
| 11 | File transfers over the message transports (serial, Bluetooth, WebSocket, NowTP) | done |
| 12 | File transfers relayed through gateways, for TesserKIT's fleet updates | done |
| 13 | UDP transport: LAN discovery and broadcast writes, for TesserKIT's sequencer; tokens and a least level for datagram peers | done |
| 14 | Routes over several transports with failover, streamed sets in order, the writer's device and peer loss on the receiving side, for a master driving other nodes | done |
| 15 | Tokens presented as a client over NowTP and UDP, for TesserKIT's masters | done |

## 19. Decisions on former open questions

- **Lists of described objects:** implemented (§4.4).
- **Enum values:** strings, with the allowed values in the schema (§4.1).
- **Binary envelope for NowTP:** not needed. JSON requests fit in one 250-byte frame, and round trips measure 13–14 ms, dominated by the radio rather than by parsing. Revisit only if a measured workload says otherwise.
- **Schema hash in NowTP discovery metadata:** implemented (`advertise()`, §10.3).

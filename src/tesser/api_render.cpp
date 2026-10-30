#include "tesser/detail/api_handler.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/envelope.h"
#include "tesser/file.h"
#include "tesser/remote.h"

// Reading (detail::Handler): values, schemas, hashes and subscriptions.

namespace tesser {
namespace detail {

void renderValue(JsonWriter& w, const Node& n, int depth, const RenderOptions& options) {
    switch (n.type()) {
        case NodeType::Object:
            w.beginObject();
            if (depth > 0) {
                for (const Node* c = static_cast<const Object&>(n).first(); c; c = c->next()) {
                    if (!detail::hasValue(*c)) continue;
                    if (!options.remotes && c->type() == NodeType::Remote) continue;
                    if (c->readAccess() > options.access) continue;
                    w.key(c->name());
                    renderValue(w, *c, depth - 1, options);
                }
            }
            w.endObject();
            break;
        case NodeType::List: {
            // One value: whole at any depth, so [] always means empty.
            const auto& l = static_cast<const ListNode&>(n);
            int d = depth > kWholeDepth ? depth : kWholeDepth;
            w.beginArray();
            for (size_t i = 0; i < l.size(); i++) renderValue(w, *l.element(i), d, options);
            w.endArray();
            break;
        }
        case NodeType::Value: {
            const auto& v = static_cast<const ValueNode&>(n);
            if (v.isSecret() && !options.secrets) {
                w.null();
            } else {
                v.write(w);
            }
            break;
        }
        case NodeType::Custom: static_cast<const CustomNode&>(n).write(w); break;
        case NodeType::Array: static_cast<const ArrayNode&>(n).write(w); break;
        case NodeType::Remote: {
            // The copy holds what a client at the mirror's level reads:
            // nothing for one below it (docs/DESIGN.md section 14).
            const auto& r = static_cast<const RemoteNode&>(n);
            if (options.access < r.mirrorLevel()) {
                w.null();
            } else {
                r.writeCopy(w, depth);
            }
            break;
        }
        case NodeType::Action:
        case NodeType::Event:
        case NodeType::File: w.null(); break;
    }
}

// The schema this client would read, whole, without the state of remote
// nodes (online, advertised hash), which changes with peers and reaches
// subscribers as changes. Otherwise two gateways mounting each other
// would re-advertise each other's hashes forever. 8 hex digits.
void Handler::hashHex(Node& target, char (&hex)[9]) {
    HashSink sink;
    JsonWriter hw(sink);
    hashing_ = true;
    renderSchema(hw, target, api_.config().maxDepth, Filter());
    hashing_ = false;
    snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(sink.hash));
}

// view=info (docs/DESIGN.md section 10.1.2): what a client reads before
// relying on newer options: the protocol, the features, and what
// view=hash gives.
bool Handler::info(Node& target) {
    if (!req_.query.keys.empty() || !req_.query.exclude.empty() || !req_.body.isNull()) {
        replyError(Status::BadRequest, "info takes no keys, exclude or shape", base_);
        return false;
    }
    JsonWriter w(reply_.begin(Status::Ok));
    w.beginObject();
    w.key("protocol");
    w.integer(kProtocolVersion);
    w.key("features");
    w.beginArray();
    for (const char* f : {"ceiling", "reasons", "strict", "info", "crc32", "limits"}) w.string(f);
#if !defined(TESSER_NO_SCHEMA)
    w.string("schema");
#endif
#if !defined(TESSER_NO_MESSAGES)
    w.string("messages");
#endif
    w.endArray();
#if !defined(TESSER_NO_SCHEMA)
    char hex[9];
    hashHex(target, hex);
    w.key("hash");
    w.string(hex);
#endif
    std::string id = api_.deviceIdHex();
    if (!id.empty()) {
        w.key("id");
        w.string(id);
    }
    // The largest request text this node takes, and reply it sends this
    // client (docs/DESIGN.md section 10.1.2): within its link's, and the
    // client's, on a message transport.
    const Config& c = api_.config();
    w.key("limits");
    w.beginObject();
    w.key("request");
    w.uinteger(c.maxRequestBody);
    w.key("response");
    w.uinteger(reply_.limit() ? reply_.limit() : c.maxResponse);
    w.endObject();
    w.endObject();
    detail::finishBody(reply_, w, base_);
    return true;
}

// Replies with the target's representation; true if that was a success.
bool Handler::get(Node& target) {
    const Query& q = req_.query;
    bool hasShape = !req_.body.isNull();
    View view = q.view;
    if (view == View::Info) return info(target);
#if defined(TESSER_NO_SCHEMA)
    if (view != View::Value) {
        replyError(Status::NotAllowed, "schemas are compiled out (TESSER_NO_SCHEMA)", base_);
        return false;
    }
#endif
    if (view == View::Hash) {
        if (!q.keys.empty() || !q.exclude.empty() || hasShape) {
            replyError(Status::BadRequest, "a hash covers the whole schema: no keys, exclude or shape", base_);
            return false;
        }
        char hex[9];
        hashHex(target, hex);
        JsonWriter w(reply_.begin(Status::Ok));
        w.string(hex);
        detail::finishBody(reply_, w, base_);
        return true;
    }

    if (target.type() != NodeType::Object) {
        if (!q.keys.empty() || !q.exclude.empty() || hasShape) {
            replyError(Status::BadRequest, "keys, exclude and shapes apply to objects", base_);
            return false;
        }
        if (view == View::Value && target.type() == NodeType::File) {
            replyError(Status::NotAllowed, "download files over HTTP; use view=schema", base_);
            return false;
        }
        if (view == View::Value && !detail::hasValue(target)) {
            replyError(Status::NotAllowed, "no value to read; use view=schema", base_);
            return false;
        }
        JsonWriter w(reply_.begin(Status::Ok));
        renderAny(w, target, depth_, Filter(), view);
        detail::finishBody(reply_, w, base_);
        return true;
    }

    const Object& obj = static_cast<const Object&>(target);
    Filter f;
    if (!q.keys.empty() && !q.exclude.empty()) {
        replyError(Status::BadRequest, "keys and exclude are exclusive", base_);
        return false;
    }
    if (hasShape) {
        if (!q.keys.empty() || !q.exclude.empty()) {
            replyError(Status::BadRequest, "a shape replaces keys and exclude", base_);
            return false;
        }
        if (!req_.body.is<JsonObjectConst>()) {
            replyError(Status::BadRequest, "shape must be an object", base_);
            return false;
        }
        f.shape = req_.body.as<JsonObjectConst>();
        f.hasShape = true;
        if (!validateShape(obj, f.shape)) {
            replyError();
            return false;
        }
    } else {
        f.keys = q.keys;
        f.exclude = q.exclude;
        std::string_view list = f.keys.empty() ? f.exclude : f.keys;
        bool ok = forEachItem(list, [&](std::string_view key) {
            const Node* c = obj.child(key);
            if (c && readable(*c)) return true;
            push(key);
            if (c) {
                error(Status::Unauthorized, "not authorized", Reason::NotAuthorized);
            } else {
                error(Status::NotFound, "no such key");
            }
            pop();
            return false;
        });
        if (!ok) {
            replyError();
            return false;
        }
    }

    JsonWriter w(reply_.begin(Status::Ok));
    renderAny(w, target, depth_, f, view);
    detail::finishBody(reply_, w, base_);
    return true;
}

bool Handler::validateShape(const Object& obj, JsonObjectConst shape) {
    for (JsonPairConst kv : shape) {
        std::string_view key(kv.key().c_str(), kv.key().size());
        if (!push(key)) return error(Status::BadRequest, "shape too deep");
        const Node* c = obj.child(key);
        if (!c) return error(Status::NotFound, "no such key");
        if (!readable(*c)) return error(Status::Unauthorized, "not authorized", Reason::NotAuthorized);
        JsonVariantConst v = kv.value();
        if (v.is<JsonObjectConst>()) {
            if (c->type() != NodeType::Object) return error(Status::BadRequest, "not an object");
            if (!validateShape(static_cast<const Object&>(*c), v.as<JsonObjectConst>())) return false;
        } else if (!v.isNull() && !(v.is<bool>() && v.as<bool>())) {
            return error(Status::BadRequest, "shape values must be null, true or objects");
        }
        pop();
    }
    return true;
}

void Handler::renderAny(JsonWriter& w, const Node& n, int depth, const Filter& f, View view) {
    if (view == View::Schema) {
        renderSchema(w, n, depth, f);
        return;
    }
    switch (n.type()) {
        case NodeType::Object: renderChildren(w, static_cast<const Object&>(n), depth, f, view); break;
        default: detail::renderValue(w, n, depth, render_); break;
    }
}

void Handler::renderChildren(JsonWriter& w, const Object& o, int depth, const Filter& f, View view) {
    w.beginObject();
    if (depth > 0) {
        if (f.hasShape) {
            renderShaped(w, o, depth, f, view);
        } else {
            for (const Node* c = o.first(); c; c = c->next()) {
                if (view == View::Value && !detail::hasValue(*c)) {
                    continue;
                }
                if (view == View::Value && c->type() == NodeType::Remote && !req_.query.remotes) continue;
                if (!readable(*c)) continue;
                if (!f.keys.empty() && !listContains(f.keys, c->name())) continue;
                if (!f.exclude.empty() && listContains(f.exclude, c->name())) continue;
                w.key(c->name());
                renderAny(w, *c, depth - 1, Filter(), view);
                yieldPoint();
            }
        }
    }
    w.endObject();
}

// Between an object's children: a streamed reply's pending pieces go out
// without the API lock (docs/DESIGN.md section 9), so a slow line holds
// up nobody else. Gets only (a subscription's snapshot must stay one
// with its start), and never below a list element, whose nodes the
// application's data backs. What a reply renders after the lock came
// back may be newer than what it rendered before.
void Handler::yieldPoint() {
    if (req_.op != Op::Get || !temps_.empty() || inList_ || !reply_.wantsDrain()) return;
    api_.yieldBegin();
    {
        MutexRelease release(api_.mutex());
        if (release.released()) reply_.drain();
    }
    api_.yieldEnd();
}

// Out of line, like schemaAttributes, so renders without a shape don't
// carry its frame.
void Handler::renderShaped(JsonWriter& w, const Object& o, int depth, const Filter& f, View view) {
    for (JsonPairConst kv : f.shape) {
        const Node* c = o.child(std::string_view(kv.key().c_str(), kv.key().size()));
        if (!c || !readable(*c)) continue;  // validated already; patches can't get here either
        w.key(c->name());
        Filter sub;
        if (kv.value().is<JsonObjectConst>() && c->type() == NodeType::Object) {
            sub.shape = kv.value().as<JsonObjectConst>();
            sub.hasShape = true;
        }
        renderAny(w, *c, depth - 1, sub, view);
    }
}

void Handler::renderSchema(JsonWriter& w, const Node& n, int depth, const Filter& f) {
#if defined(TESSER_NO_SCHEMA)
    (void)n;
    (void)depth;
    (void)f;
    w.null();
    return;
#endif
    w.beginObject();
    schemaAttributes(w, n);
    if (&n == &api_ && !hashing_) {
        // The device, as remote stubs elsewhere show it (§14.1); not hashed.
        std::string id = api_.deviceIdHex();
        if (!id.empty()) {
            w.key("id");
            w.string(id);
        }
    }
    if (n.type() == NodeType::Object && depth > 0) {
        w.key("children");
        renderChildren(w, static_cast<const Object&>(n), depth, f, View::Schema);
    }
    if (n.type() == NodeType::List && depth > 0) {
        w.key("items");
        inList_++;
        renderSchema(w, *static_cast<const ListNode&>(n).prototype(), depth, Filter());
        inList_--;
    }
    w.endObject();
}

// Everything but the children, out of line: the recursion through
// renderSchema and renderChildren then keeps small frames (with -Og, the
// ESP-IDF default, this one alone takes over 500 bytes).
void Handler::schemaAttributes(JsonWriter& w, const Node& n) {
    w.key("type");
    switch (n.type()) {
        case NodeType::Object: w.string("object"); break;
        case NodeType::Value: {
            const auto& v = static_cast<const ValueNode&>(n);
            w.string(kindName(v.kind()));
            if (v.writable()) {
                w.key("writable");
                w.boolean(true);
            }
            if (const NodeMeta* m = v.meta(); m && m->hasRange()) {
                w.key("min");
                w.number(m->min());
                w.key("max");
                w.number(m->max());
            }
            if (v.maxLength()) {
                w.key("maxLength");
                w.uinteger(v.maxLength());
            }
            if (const EnumNames* opts = v.options()) {
                w.key("enum");
                w.beginArray();
                for (const char* o : *opts) w.string(o);
                w.endArray();
            }
            if (v.isSecret()) {
                w.key("secret");
                w.boolean(true);
            }
            break;
        }
        case NodeType::Action: {
            const auto& a = static_cast<const ActionNode&>(n);
            w.string("action");
            if (a.argKind()) {
                w.key("arg");
                w.string(a.argKind());
            }
            if (std::unique_ptr<Object> params = a.params()) {
                w.key("params");
                renderSchema(w, *params, api_.config().maxDepth, Filter());
            }
            if (const NodeMeta* m = a.meta(); m && m->hasRange()) {
                w.key("min");
                w.number(m->min());
                w.key("max");
                w.number(m->max());
            }
            if (a.returnKind()) {
                w.key("returns");
                w.string(a.returnKind());
            }
            if (a.deferring()) {
                w.key("deferred");
                w.boolean(true);
            }
            break;
        }
        case NodeType::Custom: {
            w.string("custom");
            if (static_cast<const CustomNode&>(n).writable()) {
                w.key("writable");
                w.boolean(true);
            }
            break;
        }
        case NodeType::Event: w.string("event"); break;
        case NodeType::File: {
            const auto& file = static_cast<const FileNode&>(n);
            w.string("file");
            w.key("readable");
            w.boolean(file.readable());
            w.key("writable");
            w.boolean(file.writable());
            if (file.maxSize()) {
                w.key("maxSize");
                w.uinteger(file.maxSize());
            }
            w.key("contentType");
            w.string(file.contentType());
            if (file.acceptTypes()) {
                w.key("accept");
                w.string(file.acceptTypes());
            }
            break;
        }
        case NodeType::Array: {
            const auto& a = static_cast<const ArrayNode&>(n);
            w.string("array");
            if (a.writable()) {
                w.key("writable");
                w.boolean(true);
            }
            w.key("maxSize");
            w.uinteger(a.maxSize());
            if (a.fixedSize()) {
                w.key("fixed");
                w.boolean(true);
            }
            w.key("items");
            w.beginObject();
            w.key("type");
            w.string(kindName(a.itemKind()));
            if (const NodeMeta* m = a.meta(); m && m->hasRange()) {
                w.key("min");
                w.number(m->min());
                w.key("max");
                w.number(m->max());
            }
            w.endObject();
            break;
        }
        case NodeType::List: {
            w.string("list");
            if (static_cast<const ListNode&>(n).writable()) {
                w.key("writable");
                w.boolean(true);
            }
            w.key("maxSize");
            w.uinteger(static_cast<const ListNode&>(n).maxSize());
            if (const char* k = static_cast<const ListNode&>(n).keyField()) {
                w.key("key");
                w.string(k);
            }
            break;
        }
        case NodeType::Remote: {
            const auto& r = static_cast<const RemoteNode&>(n);
            w.string("remote");
            if (r.mirrored()) {
                w.key("mirror");
                w.boolean(true);
            }
            if (!r.online() && !hashing_) {
                w.key("online");
                w.boolean(false);
            }
            // The device and the transport in use (docs/DESIGN.md section
            // 14.3): state, not part of the hash.
            RemoteNode::Route route;
            if (!hashing_ && r.routes().size() > 1 && r.route(route)) {
                w.key("route");
                w.string(route.name());
            }
            if (uint32_t hash = hashing_ ? 0 : r.advertisedSchema()) {
                char hex[9];
                snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(hash));
                w.key("schema");
                w.string(hex);
            }
            PeerAddress device = hashing_ ? PeerAddress() : r.device();
            if (device.length == 6) {
                char hex[13];
                snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x", device.bytes[0], device.bytes[1],
                         device.bytes[2], device.bytes[3], device.bytes[4], device.bytes[5]);
                w.key("id");
                w.string(hex);
            }
            if (uint8_t v = hashing_ ? 0 : r.protocol()) {  // docs/DESIGN.md section 10.1.2
                w.key("protocol");
                w.integer(v);
            }
            break;
        }
    }
    if (n.persisted()) {
        w.key("persist");
        w.boolean(true);
    }
    if (n.writeAccess() != Access::Public) {
        w.key("access");
        w.string(toString(n.writeAccess()));
    }
    if (const NodeMeta* m = n.meta()) renderMeta(w, *m);
}

void Handler::renderMeta(JsonWriter& w, const NodeMeta& m) {
    if (m.doc()) {
        w.key("description");
        w.string(m.doc());
    }
    if (m.label()) {
        w.key("label");
        w.string(m.label());
    }
    if (m.unit()) {
        w.key("unit");
        w.string(m.unit());
    }
    if (m.step() != 0) {
        w.key("step");
        w.number(static_cast<double>(m.step()), 7);
    }
    if (!m.hintCount()) return;
    w.key("ui");
    w.beginObject();
    for (size_t i = 0; i < m.hintCount(); i++) {
        UiHint h = m.hint(i);
        w.key(h.key);
        switch (h.kind) {
            case UiHint::Kind::Flag: w.boolean(true); break;
            case UiHint::Kind::String: w.string(h.str ? h.str : ""); break;
            case UiHint::Kind::Number: w.number(static_cast<double>(h.num), 7); break;
        }
    }
    w.endObject();
}

void Handler::subscribe(Node& target) {
    if (!req_.subscriber) {
        return replyError(Status::NotAllowed, Reason::CannotWait, "this transport can't deliver notifications", base_);
    }
    if (!req_.body.isNull()) {
        return replyError(Status::BadRequest, "subscriptions take keys, exclude and depth, not shapes", base_);
    }
    if (req_.query.view != View::Value) return replyError(Status::BadRequest, "subscriptions are on values", base_);
    if (target.type() == NodeType::Action || target.type() == NodeType::File) {
        return replyError(Status::NotAllowed, "actions and files can't be subscribed to", base_);
    }
    if (!temps_.empty()) {
        return replyError(Status::NotAllowed, "subscribe to the list or array itself, not to its elements", base_);
    }
    Status allowed = api_.subscriptionAllowed(req_.subscriber);
    if (allowed != Status::Ok) return replyError(allowed, Reason::SubscriptionLimit, "too many subscriptions", base_);
    // Without this, the first sample after subscribing would become the
    // baseline, and a change made before it would never be notified.
    api_.sampleWatched(target);
    if (target.type() == NodeType::Event || !req_.query.snapshot) {
        // Nothing to snapshot, or the client doesn't want one.
        JsonWriter w(reply_.begin(Status::Ok));
        w.null();
        detail::finishBody(reply_, w, base_);
    } else if (!get(target)) {
        return;
    }
    Subscription sub;
    sub.subscriber = req_.subscriber;
    sub.node = &target;
    sub.path.assign(base_.data(), base_.size());
    sub.keys.assign(req_.query.keys.data(), req_.query.keys.size());
    sub.exclude.assign(req_.query.exclude.data(), req_.query.exclude.size());
    sub.depth = req_.query.depth;
    sub.interval = req_.query.interval;
    sub.changes = req_.query.changes;
    sub.events = req_.query.events;
    sub.eventHint.assign(req_.query.eventHint.data(), req_.query.eventHint.size());
    sub.remotes = req_.query.remotes;
    sub.access = level_;
    sub.ceiling = req_.query.ceiling;
    api_.addSubscription(std::move(sub));
}

void Handler::unsubscribe() {
    size_t removed = req_.subscriber ? api_.removeSubscriptions(req_.subscriber, base_, req_.query.ceiling) : 0;
    JsonWriter w(reply_.begin(Status::Ok));
    w.uinteger(removed);
    detail::finishBody(reply_, w, base_);
}
}  // namespace detail
}  // namespace tesser

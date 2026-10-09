#include "tesser/api.h"

#include <stdio.h>

#include <memory>
#include <string>
#include <vector>

#include "tesser/detail/lazy_patch.h"
#include "tesser/remote.h"

namespace tesser {

bool parseOp(std::string_view s, Op& out) {
    if (s == "get") {
        out = Op::Get;
    } else if (s == "set") {
        out = Op::Set;
    } else if (s == "sub") {
        out = Op::Subscribe;
    } else if (s == "unsub") {
        out = Op::Unsubscribe;
    } else {
        return false;
    }
    return true;
}

bool parseView(std::string_view s, View& out) {
    if (s == "value") {
        out = View::Value;
    } else if (s == "schema") {
        out = View::Schema;
    } else {
        return false;
    }
    return true;
}

const char* toString(Op op) {
    switch (op) {
        case Op::Get: return "get";
        case Op::Set: return "set";
        case Op::Subscribe: return "sub";
        case Op::Unsubscribe: return "unsub";
    }
    return "get";
}

bool Api::pendingBegin() {
    int n = pending_.fetch_add(1);
    if (n >= config_.maxPending) {
        pending_.fetch_sub(1);
        return false;
    }
    return true;
}

namespace {

// Calls f(item) for each non-empty, space-trimmed item of a comma-separated list.
template <class F>
bool forEachItem(std::string_view list, F f) {
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string_view::npos) comma = list.size();
        std::string_view item = list.substr(pos, comma - pos);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (!item.empty() && !f(item)) return false;
        pos = comma + 1;
    }
    return true;
}

bool listContains(std::string_view list, std::string_view name) {
    return !forEachItem(list, [&](std::string_view item) { return item != name; });
}

bool parseIndex(std::string_view s, size_t& out) {
    if (s.empty() || s.size() > 9 || (s.size() > 1 && s[0] == '0')) return false;
    size_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<size_t>(c - '0');
    }
    out = v;
    return true;
}

struct Filter {
    std::string_view keys;
    std::string_view exclude;
    JsonObjectConst shape;
    bool hasShape = false;
};

}  // namespace

namespace detail {

void renderValue(JsonWriter& w, const Node& n, int depth, bool remotes) {
    switch (n.type()) {
        case NodeType::Object:
            w.beginObject();
            if (depth > 0) {
                for (const Node* c = static_cast<const Object&>(n).first(); c; c = c->next()) {
                    if (c->type() == NodeType::Action || c->type() == NodeType::Event) continue;
                    if (!remotes && c->type() == NodeType::Remote) continue;
                    w.key(c->name());
                    renderValue(w, *c, depth - 1, remotes);
                }
            }
            w.endObject();
            break;
        case NodeType::List: {
            const auto& l = static_cast<const ListNode&>(n);
            w.beginArray();
            if (depth > 0) {
                for (size_t i = 0; i < l.size(); i++) renderValue(w, *l.element(i), depth, remotes);
            }
            w.endArray();
            break;
        }
        case NodeType::Value: static_cast<const ValueNode&>(n).write(w); break;
        case NodeType::Custom: static_cast<const CustomNode&>(n).write(w); break;
        case NodeType::Remote: static_cast<const RemoteNode&>(n).writeCopy(w, depth); break;
        case NodeType::Action:
        case NodeType::Event: w.null(); break;
    }
}

}  // namespace detail

namespace {

// One request in flight. Holds the error state and the path trail used to
// name the failing node in error replies.
class Handler {
public:
    static constexpr int kMaxTrail = 32;

    Handler(Api& api, const Request& req, Reply& reply) : api_(api), req_(req), reply_(reply) {
        base_ = req.path;
        if (base_.size() > 1 && base_.back() == '/') base_.remove_suffix(1);
        if (base_ == "/") base_ = std::string_view();
        int maxDepth = api.config().maxDepth;
        depth_ = (req.query.depth < 0 || req.query.depth > maxDepth) ? maxDepth : req.query.depth;
    }

    void run() {
        Node* target = resolve();
        if (!target) return;
        if (req_.op != Op::Unsubscribe && !allowed(*target)) {
            return replyError(Status::Unauthorized, "not authorized", base_);
        }
        if (target->type() == NodeType::Remote) {
            auto& remote = static_cast<RemoteNode&>(*target);
            bool itself = remoteRest_.empty();
            if (req_.op == Op::Get || req_.op == Op::Set) return forward(remote);
            // Subscriptions: to a mirrored remote node itself, locally.
            if (!itself || !remote.mirrored()) {
                return replyError(Status::NotAllowed, "subscribe to a mirrored remote node itself", base_);
            }
        }
        switch (req_.op) {
            case Op::Get: get(*target); break;
            case Op::Set: set(*target); break;
            case Op::Subscribe: subscribe(*target); break;
            case Op::Unsubscribe: unsubscribe(); break;
        }
    }

private:
    // A write below a list element marks the list itself changed: element
    // nodes are temporary, the list is what subscribers and persistence see.
    void markLists() {
        for (ListNode* l : lists_) l->changed();
    }

    bool allowed(const Node& n) const {
        const Authorizer& a = api_.authorizer();
        return !a || a(req_.client, req_.op, n);
    }

    // ---- errors -------------------------------------------------------

    std::string trailPath() const {
        std::string p(base_.data(), base_.size());
        for (int i = 0; i < trailLen_; i++) {
            p += '/';
            if (indexMask_ & (uint32_t(1) << i)) {
                char buf[8];
                snprintf(buf, sizeof(buf), "%u", unsigned(indexes_[i]));
                p += buf;
            } else {
                p.append(trail_[i].data(), trail_[i].size());
            }
        }
        if (p.empty()) p = "/";
        return p;
    }

    bool push(std::string_view name) {
        if (trailLen_ >= kMaxTrail) return false;
        indexMask_ &= ~(uint32_t(1) << trailLen_);
        trail_[trailLen_++] = name;
        return true;
    }
    void pop() { trailLen_--; }

    // A list index in the trail: stored as a number, formatted on error only.
    bool pushIndex(size_t i) {
        if (trailLen_ >= kMaxTrail) return false;
        indexMask_ |= uint32_t(1) << trailLen_;
        indexes_[trailLen_++] = static_cast<uint16_t>(i);
        return true;
    }

    // Records the first error; the reply is written once the walk unwinds.
    bool error(Status s, const char* message) {
        if (!failed_) {
            failed_ = true;
            errStatus_ = s;
            errMessage_ = message;
            errPath_ = trailPath();
        }
        return false;
    }

    void replyError() { writeError(reply_, errStatus_, errPath_, errMessage_, partial_); }

    void replyError(Status s, const char* message, std::string_view path) {
        writeError(reply_, s, path.empty() ? std::string_view("/") : path, message);
    }

    // ---- resolution ---------------------------------------------------

    Node* resolve() {
        std::string_view path = req_.path;
        if (path.empty() || path == "/") return &api_;
        if (path.front() != '/') {
            replyError(Status::BadRequest, "path must start with '/'", path);
            return nullptr;
        }
        Node* node = &api_;
        size_t pos = 1;
        std::string_view p = base_;
        while (pos < p.size()) {
            if (node->type() == NodeType::Remote) {
                remoteRest_ = p.substr(pos - 1);  // forwarded as is
                break;
            }
            size_t slash = p.find('/', pos);
            if (slash == std::string_view::npos) slash = p.size();
            std::string_view seg = p.substr(pos, slash - pos);
            if (seg.empty()) {
                replyError(Status::BadRequest, "empty path segment", p);
                return nullptr;
            }
            Node* next = nullptr;
            if (node->type() == NodeType::Object) {
                next = static_cast<Object*>(node)->child(seg);
            } else if (node->type() == NodeType::List) {
                auto* list = static_cast<ListNode*>(node);
                size_t index;
                if (parseIndex(seg, index) && index < list->size()) {
                    temps_.push_back(list->element(index));
                    lists_.push_back(list);
                    next = temps_.back().get();
                }
            }
            if (!next) {
                replyError(Status::NotFound, "no such node", p.substr(0, slash));
                return nullptr;
            }
            node = next;
            pos = slash + 1;
        }
        return node;
    }

    // ---- get ----------------------------------------------------------

    // Replies with the target's representation; true if that was a success.
    bool get(Node& target) {
        const Query& q = req_.query;
        bool hasShape = !req_.body.isNull();
        View view = q.view;
#if defined(TESSER_NO_SCHEMA)
        if (view == View::Schema) {
            replyError(Status::NotAllowed, "schemas are compiled out (TESSER_NO_SCHEMA)", base_);
            return false;
        }
#endif

        if (target.type() != NodeType::Object) {
            if (!q.keys.empty() || !q.exclude.empty() || hasShape) {
                replyError(Status::BadRequest, "keys, exclude and shapes apply to objects", base_);
                return false;
            }
            if (view == View::Value && (target.type() == NodeType::Action || target.type() == NodeType::Event)) {
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
                if (obj.child(key)) return true;
                push(key);
                error(Status::NotFound, "no such key");
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

    bool validateShape(const Object& obj, JsonObjectConst shape) {
        for (JsonPairConst kv : shape) {
            std::string_view key(kv.key().c_str(), kv.key().size());
            if (!push(key)) return error(Status::BadRequest, "shape too deep");
            const Node* c = obj.child(key);
            if (!c) return error(Status::NotFound, "no such key");
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

    // ---- rendering ----------------------------------------------------

    void renderAny(JsonWriter& w, const Node& n, int depth, const Filter& f, View view) {
        if (view == View::Schema) {
            renderSchema(w, n, depth, f);
            return;
        }
        switch (n.type()) {
            case NodeType::Object: renderChildren(w, static_cast<const Object&>(n), depth, f, view); break;
            default: detail::renderValue(w, n, depth, req_.query.remotes); break;
        }
    }

    void renderChildren(JsonWriter& w, const Object& o, int depth, const Filter& f, View view) {
        w.beginObject();
        if (depth > 0) {
            if (f.hasShape) {
                for (JsonPairConst kv : f.shape) {
                    const Node* c = o.child(std::string_view(kv.key().c_str(), kv.key().size()));
                    if (!c) continue;  // validated already; patches can't get here either
                    w.key(c->name());
                    Filter sub;
                    if (kv.value().is<JsonObjectConst>() && c->type() == NodeType::Object) {
                        sub.shape = kv.value().as<JsonObjectConst>();
                        sub.hasShape = true;
                    }
                    renderAny(w, *c, depth - 1, sub, view);
                }
            } else {
                for (const Node* c = o.first(); c; c = c->next()) {
                    if (view == View::Value && (c->type() == NodeType::Action || c->type() == NodeType::Event)) {
                        continue;
                    }
                    if (view == View::Value && c->type() == NodeType::Remote && !req_.query.remotes) continue;
                    if (!f.keys.empty() && !listContains(f.keys, c->name())) continue;
                    if (!f.exclude.empty() && listContains(f.exclude, c->name())) continue;
                    w.key(c->name());
                    renderAny(w, *c, depth - 1, Filter(), view);
                }
            }
        }
        w.endObject();
    }

    void renderSchema(JsonWriter& w, const Node& n, int depth, const Filter& f) {
#if defined(TESSER_NO_SCHEMA)
        (void)n;
        (void)depth;
        (void)f;
        w.null();
        return;
#endif
        w.beginObject();
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
                if (const NodeMeta* m = v.meta(); m && m->hasRange) {
                    w.key("min");
                    w.number(m->min);
                    w.key("max");
                    w.number(m->max);
                }
                if (v.maxLength()) {
                    w.key("maxLength");
                    w.uinteger(v.maxLength());
                }
                if (const std::vector<const char*>* opts = v.options()) {
                    w.key("enum");
                    w.beginArray();
                    for (const char* o : *opts) w.string(o);
                    w.endArray();
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
            case NodeType::List: {
                w.string("list");
                w.key("maxSize");
                w.uinteger(static_cast<const ListNode&>(n).maxSize());
                break;
            }
            case NodeType::Remote: {
                w.string("remote");
                if (static_cast<const RemoteNode&>(n).mirrored()) {
                    w.key("mirror");
                    w.boolean(true);
                }
                break;
            }
        }
        if (n.persisted()) {
            w.key("persist");
            w.boolean(true);
        }
        if (const NodeMeta* m = n.meta(); m && m->doc) {
            w.key("description");
            w.string(m->doc);
        }
        if (n.type() == NodeType::Object && depth > 0) {
            w.key("children");
            renderChildren(w, static_cast<const Object&>(n), depth, f, View::Schema);
        }
        if (n.type() == NodeType::List && depth > 0) {
            w.key("items");
            renderSchema(w, *static_cast<const ListNode&>(n).prototype(), depth, Filter());
        }
        w.endObject();
    }

    // ---- remote nodes -------------------------------------------------

    // Virtual, so the datagram and forwarding code is only linked into
    // applications that create remote nodes.
    void forward(RemoteNode& remote) { remote.forward(api_, req_, remoteRest_, base_, reply_); }

    // ---- subscriptions ------------------------------------------------

    void subscribe(Node& target) {
        if (!req_.subscriber) {
            return replyError(Status::NotAllowed, "this transport can't deliver notifications", base_);
        }
        if (!req_.body.isNull()) {
            return replyError(Status::BadRequest, "subscriptions take keys, exclude and depth, not shapes", base_);
        }
        if (req_.query.view != View::Value) return replyError(Status::BadRequest, "subscriptions are on values", base_);
        if (target.type() == NodeType::Action) {
            return replyError(Status::NotAllowed, "actions can't be subscribed to", base_);
        }
        if (!temps_.empty()) {
            return replyError(Status::NotAllowed, "subscribe to the list itself, not to its elements", base_);
        }
        Status allowed = api_.subscriptionAllowed(req_.subscriber);
        if (allowed != Status::Ok) return replyError(allowed, "too many subscriptions", base_);
        if (target.type() == NodeType::Event) {
            // Nothing to snapshot.
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
        sub.events = req_.query.events;
        sub.remotes = req_.query.remotes;
        api_.addSubscription(std::move(sub));
    }

    void unsubscribe() {
        size_t removed = req_.subscriber ? api_.removeSubscriptions(req_.subscriber, base_) : 0;
        JsonWriter w(reply_.begin(Status::Ok));
        w.uinteger(removed);
        detail::finishBody(reply_, w, base_);
    }

    // ---- set ----------------------------------------------------------

    void set(Node& target) {
        if (req_.query.view != View::Value || !req_.query.keys.empty() || !req_.query.exclude.empty()) {
            replyError(Status::BadRequest, "set takes no query options", base_);
            return;
        }
        JsonVariantConst body = req_.body;
        switch (target.type()) {
            case NodeType::Object: setObject(static_cast<Object&>(target), body); return;
            case NodeType::Value: {
                auto& v = static_cast<ValueNode&>(target);
                if (!v.writable()) return replyError(Status::ReadOnly, "read-only", base_);
                Check c = v.check(body);
                if (c.isOk()) c = v.apply(body);
                if (!c.isOk()) return replyError(c.status, c.message, base_);
                v.changed();
                markLists();
                JsonWriter w(reply_.begin(Status::Ok));
                v.write(w);
                detail::finishBody(reply_, w, base_);
                return;
            }
            case NodeType::Custom: {
                auto& c = static_cast<CustomNode&>(target);
                if (!c.writable()) return replyError(Status::ReadOnly, "read-only", base_);
                Check chk = c.check(body);
                if (!chk.isOk()) return replyError(chk.status, chk.message, base_);
                Status s = c.apply(body);
                if (s != Status::Ok) return replyError(s, "rejected", base_);
                c.changed();
                markLists();
                JsonWriter w(reply_.begin(Status::Ok));
                c.write(w);
                detail::finishBody(reply_, w, base_);
                return;
            }
            case NodeType::Action: {
                auto& a = static_cast<ActionNode&>(target);
                Check chk = a.check(body);
                if (!chk.isOk()) return replyError(chk.status, chk.message, base_);
                Call call(api_, &reply_, body, req_.client, base_.empty() ? std::string_view("/") : base_);
                a.invoke(body, call);
                markLists();
                if (call.deferred() || call.replied()) return;
                if (call.status() != Status::Ok) return replyError(call.status(), call.message(), base_);
                JsonWriter w(reply_.begin(Status::Ok));
                w.null();
                detail::finishBody(reply_, w, base_);
                return;
            }
            case NodeType::Event: replyError(Status::NotAllowed, "events can't be set", base_); return;
            case NodeType::List: {
                auto& list = static_cast<ListNode&>(target);
                if (!validatePatch(list, body)) return replyError();
                if (!applyPatch(list, body, false) || !applyPatch(list, body, true)) {
                    partial_ = applied_ > 0;
                    return replyError();
                }
                markLists();
                JsonWriter w(reply_.begin(Status::Ok));
                detail::renderValue(w, list, api_.config().maxDepth, req_.query.remotes);
                detail::finishBody(reply_, w, base_);
                return;
            }
            case NodeType::Remote: return;  // forwarded before set() is reached
        }
    }

    void setObject(Object& obj, JsonVariantConst body) {
        if (!body.is<JsonObjectConst>()) {
            replyError(Status::InvalidValue, "expected object", base_);
            return;
        }
        // Pass 1: validate everything. Nothing is applied on failure.
        if (!validatePatch(obj, body)) return replyError();
        // Pass 2: values, in document order. Pass 3: actions.
        if (!applyPatch(obj, body, false) || !applyPatch(obj, body, true)) {
            partial_ = applied_ > 0;
            return replyError();
        }
        markLists();
        Filter f;
        f.shape = body.as<JsonObjectConst>();
        f.hasShape = true;
        JsonWriter w(reply_.begin(Status::Ok));
        renderAny(w, obj, api_.config().maxDepth, f, View::Value);
        detail::finishBody(reply_, w, base_);
    }

    bool validatePatch(Node& n, JsonVariantConst v) {
        if (!allowed(n)) return error(Status::Unauthorized, "not authorized");
        switch (n.type()) {
            case NodeType::Object: {
                if (!v.is<JsonObjectConst>()) return error(Status::InvalidValue, "expected object");
                auto& o = static_cast<Object&>(n);
                for (JsonPairConst kv : v.as<JsonObjectConst>()) {
                    std::string_view key(kv.key().c_str(), kv.key().size());
                    if (!push(key)) return error(Status::BadRequest, "patch too deep");
                    Node* c = o.child(key);
                    if (!c) return error(Status::NotFound, "no such key");
                    if (!validatePatch(*c, kv.value())) return false;
                    pop();
                }
                return true;
            }
            case NodeType::Value: {
                auto& val = static_cast<ValueNode&>(n);
                if (!val.writable()) return error(Status::ReadOnly, "read-only");
                Check c = val.check(v);
                return c.isOk() || error(c.status, c.message);
            }
            case NodeType::Action: {
                auto& a = static_cast<ActionNode&>(n);
                if (a.deferring()) return error(Status::BadRequest, "call deferred actions directly");
                Check c = a.check(v);
                return c.isOk() || error(c.status, c.message);
            }
            case NodeType::Custom: {
                auto& cu = static_cast<CustomNode&>(n);
                if (!cu.writable()) return error(Status::ReadOnly, "read-only");
                Check c = cu.check(v);
                return c.isOk() || error(c.status, c.message);
            }
            case NodeType::Event: return error(Status::NotAllowed, "events can't be set");
            case NodeType::Remote: return error(Status::NotAllowed, "write remote nodes directly");
            case NodeType::List: {
                auto& list = static_cast<ListNode&>(n);
                if (!v.is<JsonArrayConst>()) return error(Status::InvalidValue, "expected array");
                JsonArrayConst items = v.as<JsonArrayConst>();
                if (items.size() > list.maxSize()) return error(Status::InvalidValue, "too many elements");
                size_t i = 0;
                for (JsonVariantConst item : items) {
                    if (!pushIndex(i)) return error(Status::BadRequest, "patch too deep");
                    if (!item.is<JsonObjectConst>()) return error(Status::InvalidValue, "expected object");
                    std::unique_ptr<Object> element = i < list.size() ? list.element(i) : list.prototype();
                    if (!validatePatch(*element, item)) return false;
                    pop();
                    i++;
                }
                return true;
            }
        }
        return false;
    }

    // actions == false: apply values and custom nodes; true: run actions.
    bool applyPatch(Node& n, JsonVariantConst v, bool actions) {
        switch (n.type()) {
            case NodeType::Object: {
                auto& o = static_cast<Object&>(n);
                for (JsonPairConst kv : v.as<JsonObjectConst>()) {
                    Node* c = o.child(std::string_view(kv.key().c_str(), kv.key().size()));
                    push(c->name());
                    if (!applyPatch(*c, kv.value(), actions)) return false;
                    pop();
                }
                return true;
            }
            case NodeType::Value: {
                if (actions) return true;
                Check c = static_cast<ValueNode&>(n).apply(v);
                if (!c.isOk()) return error(c.status, c.message);
                n.changed();
                applied_++;
                return true;
            }
            case NodeType::Custom: {
                if (actions) return true;
                Status s = static_cast<CustomNode&>(n).apply(v);
                if (s != Status::Ok) return error(s, "rejected");
                n.changed();
                applied_++;
                return true;
            }
            case NodeType::Action: {
                if (!actions) return true;
                std::string path = trailPath();
                Call call(api_, nullptr, v, req_.client, path);
                static_cast<ActionNode&>(n).invoke(v, call);
                if (call.status() != Status::Ok) return error(call.status(), call.message());
                applied_++;
                return true;
            }
            case NodeType::Event:
            case NodeType::Remote: return true;
            case NodeType::List: {
                // The array replaces the list: its length is the new size, and
                // each element is patched (new ones start from defaults).
                auto& list = static_cast<ListNode&>(n);
                JsonArrayConst items = v.as<JsonArrayConst>();
                if (!actions) list.resize(items.size());
                size_t i = 0;
                for (JsonVariantConst item : items) {
                    pushIndex(i);
                    if (!applyPatch(*list.element(i), item, actions)) return false;
                    pop();
                    i++;
                }
                if (!actions) {
                    list.changed();
                    applied_++;
                }
                return true;
            }
        }
        return true;
    }

    Api& api_;
    const Request& req_;
    Reply& reply_;
    std::string_view base_;  // request path without trailing slash; empty for the root
    int depth_;

    std::string_view trail_[kMaxTrail];
    int trailLen_ = 0;
    bool failed_ = false;
    Status errStatus_ = Status::Internal;
    const char* errMessage_ = nullptr;
    std::string errPath_;
    int applied_ = 0;
    bool partial_ = false;
    std::vector<std::unique_ptr<Object>> temps_;  // list elements on the request path
    std::vector<ListNode*> lists_;                // lists the path went through
    uint16_t indexes_[kMaxTrail];                 // list indexes in the trail...
    uint32_t indexMask_ = 0;                      // ...at the levels whose bit is set
    std::string_view remoteRest_;                 // path below a remote node, forwarded as is
};

}  // namespace

void Api::authorize(Authorizer fn) {
    MutexGuard guard(mutex_);
    authorizer_ = std::move(fn);
}

void Api::handle(const Request& request, Reply& reply) {
    if (config_.queued && enqueue(request, reply)) return;
    handleNow(request, reply);
}

void Api::handleNow(const Request& request, Reply& reply) {
    MutexGuard guard(mutex_);
    Handler(*this, request, reply).run();
}

// ---- queued mode ------------------------------------------------------------

namespace {
// Lets a queued request's action defer: the detached reply changes hands.
class QueuedReply : public Reply {
public:
    explicit QueuedReply(Reply* inner) : inner_(inner) {}
    Sink& begin(Status s) override { return inner_->begin(s); }
    void end() override { inner_->end(); }
    bool rollback() override { return inner_->rollback(); }
    Reply* detach() override {
        if (transferred) return nullptr;
        transferred = true;
        return inner_;
    }
    bool transferred = false;

private:
    Reply* inner_;
};
}  // namespace

bool Api::enqueue(const Request& request, Reply& reply) {
    {
        MutexGuard guard(queueMutex_);
        if (queue_.size() >= config_.maxQueued) {
            writeError(reply, Status::Busy, request.path, "request queue full");
            return true;
        }
    }
    Reply* detached = reply.detach();
    if (!detached) return false;  // the transport can't wait: handle inline
    auto* q = new QueuedRequest();
    q->request = request;
    q->path.assign(request.path.data(), request.path.size());
    q->keys.assign(request.query.keys.data(), request.query.keys.size());
    q->exclude.assign(request.query.exclude.data(), request.query.exclude.size());
    if (!request.body.isNull()) serializeJson(request.body, q->body);
    q->request.path = q->path;
    q->request.query.keys = q->keys;
    q->request.query.exclude = q->exclude;
    q->request.body = JsonVariantConst();
    q->reply = detached;
    MutexGuard guard(queueMutex_);
    queue_.push_back(q);
    return true;
}

void Api::runQueued() {
    for (;;) {
        QueuedRequest* q;
        {
            MutexGuard guard(queueMutex_);
            if (queue_.empty()) return;
            q = queue_.front();
            queue_.erase(queue_.begin());
        }
        JsonDocument doc;
        if (!q->body.empty()) {
            deserializeJson(doc, q->body);
            q->request.body = doc.as<JsonVariantConst>();
        }
        QueuedReply reply(q->reply);
        handleNow(q->request, reply);
        if (!reply.transferred) q->reply->release();
        delete q;
    }
}

uint32_t Api::schemaHash() {
    class HashReply : public Reply {
    public:
        Sink& begin(Status) override { return sink; }
        void end() override {}
        HashSink sink;
    } reply;
    Request req;
    req.path = "/";
    req.query.view = View::Schema;
    req.client.authenticated = true;  // internal
    handleNow(req, reply);
    return reply.sink.hash;
}

size_t Api::queuedRequests() const {
    MutexGuard guard(queueMutex_);
    return queue_.size();
}

}  // namespace tesser

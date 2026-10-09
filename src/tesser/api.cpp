#include "tesser/api.h"

#include <string>

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

struct Filter {
    std::string_view keys;
    std::string_view exclude;
    JsonObjectConst shape;
    bool hasShape = false;
};

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
        switch (req_.op) {
            case Op::Get: get(*target); break;
            case Op::Set: set(*target); break;
            case Op::Subscribe: subscribe(*target); break;
            case Op::Unsubscribe: unsubscribe(); break;
        }
    }

private:
    // ---- errors -------------------------------------------------------

    std::string trailPath() const {
        std::string p(base_.data(), base_.size());
        for (int i = 0; i < trailLen_; i++) {
            p += '/';
            p.append(trail_[i].data(), trail_[i].size());
        }
        if (p.empty()) p = "/";
        return p;
    }

    bool push(std::string_view name) {
        if (trailLen_ >= kMaxTrail) return false;
        trail_[trailLen_++] = name;
        return true;
    }
    void pop() { trailLen_--; }

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
            size_t slash = p.find('/', pos);
            if (slash == std::string_view::npos) slash = p.size();
            std::string_view seg = p.substr(pos, slash - pos);
            if (seg.empty()) {
                replyError(Status::BadRequest, "empty path segment", p);
                return nullptr;
            }
            Node* next = nullptr;
            if (node->type() == NodeType::Object) next = static_cast<Object*>(node)->child(seg);
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
            case NodeType::Value: static_cast<const ValueNode&>(n).write(w); break;
            case NodeType::Custom: static_cast<const CustomNode&>(n).write(w); break;
            case NodeType::Action:
            case NodeType::Event: w.null(); break;
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
        w.endObject();
    }

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
                if (call.deferred() || call.replied()) return;
                if (call.status() != Status::Ok) return replyError(call.status(), call.message(), base_);
                JsonWriter w(reply_.begin(Status::Ok));
                w.null();
                detail::finishBody(reply_, w, base_);
                return;
            }
            case NodeType::Event: replyError(Status::NotAllowed, "events can't be set", base_); return;
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
        Filter f;
        f.shape = body.as<JsonObjectConst>();
        f.hasShape = true;
        JsonWriter w(reply_.begin(Status::Ok));
        renderAny(w, obj, api_.config().maxDepth, f, View::Value);
        detail::finishBody(reply_, w, base_);
    }

    bool validatePatch(Node& n, JsonVariantConst v) {
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
            case NodeType::Event: return true;
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
};

}  // namespace

void Api::handle(const Request& request, Reply& reply) {
    MutexGuard guard(mutex_);
    Handler(*this, request, reply).run();
}

}  // namespace tesser

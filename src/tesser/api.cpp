#include "tesser/api.h"

#include <stdio.h>
#include <stdlib.h>

#include <memory>
#include <string>
#include <vector>

#include "tesser/detail/lazy_patch.h"
#include "tesser/envelope.h"
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
    } else if (s == "hash") {
        out = View::Hash;
    } else {
        return false;
    }
    return true;
}

const char* toString(Access a) {
    switch (a) {
        case Access::Public: return "public";
        case Access::User: return "user";
        case Access::Admin: return "admin";
    }
    return "public";
}

bool parseAccess(std::string_view s, Access& out) {
    if (s == "public") {
        out = Access::Public;
    } else if (s == "user") {
        out = Access::User;
    } else if (s == "admin") {
        out = Access::Admin;
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

Access higher(Access a, Access b) { return a > b ? a : b; }

struct Filter {
    std::string_view keys;
    std::string_view exclude;
    JsonObjectConst shape;
    bool hasShape = false;
};

}  // namespace

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
            const auto& l = static_cast<const ListNode&>(n);
            w.beginArray();
            if (depth > 0) {
                for (size_t i = 0; i < l.size(); i++) renderValue(w, *l.element(i), depth, options);
            }
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
        case NodeType::Remote: static_cast<const RemoteNode&>(n).writeCopy(w, depth); break;
        case NodeType::Action:
        case NodeType::Event:
        case NodeType::File: w.null(); break;
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
        level_ = req.client.level();
        render_.remotes = req.query.remotes;
        render_.access = level_;
        base_ = req.path;
        if (base_.size() > 1 && base_.back() == '/') base_.remove_suffix(1);
        if (base_ == "/") base_ = std::string_view();
        int maxDepth = api.config().maxDepth;
        depth_ = (req.query.depth < 0 || req.query.depth > maxDepth) ? maxDepth : req.query.depth;
    }

    void run() {
        Node* target = resolve();
        if (!target) return;
        if (req_.op != Op::Unsubscribe) {
            Access need = req_.op == Op::Set ? writeNeed_ : readNeed_;
            if (level_ < need || !allowed(*target)) return replyError(Status::Unauthorized, "not authorized", base_);
        }
        if (target->type() == NodeType::Remote) {
            auto& remote = static_cast<RemoteNode&>(*target);
            bool itself = remoteRest_.empty();
            if (req_.op == Op::Get || req_.op == Op::Set) return forward(remote);
            // A mirrored remote node itself is subscribed to locally, like a
            // value; anything else is forwarded. Unsubscribing is local.
            if (req_.op == Op::Subscribe && (!itself || !remote.mirrored())) {
                return remote.subscribe(api_, req_, remoteRest_, base_, reply_);
            }
        }
        switch (req_.op) {
            case Op::Get: get(*target); break;
            case Op::Set: set(*target); break;
            case Op::Subscribe: subscribe(*target); break;
            case Op::Unsubscribe: unsubscribe(); break;
        }
    }

    // The file node a transfer targets (FileTransfer), or null with the error
    // written; `notFile` (no error) when the path resolves to something else.
    FileNode* file(bool& notFile) {
        Node* target = resolve();
        if (!target) return nullptr;
        if (target->type() != NodeType::File) {
            notFile = true;
            return nullptr;
        }
        if (!temps_.empty()) {
            replyError(Status::NotAllowed, "files in list elements can't be transferred", base_);
            return nullptr;
        }
        Access need = req_.op == Op::Set ? writeNeed_ : readNeed_;
        if (level_ < need || !allowed(*target)) {
            replyError(Status::Unauthorized, "not authorized", base_);
            return nullptr;
        }
        return static_cast<FileNode*>(target);
    }

private:
    // A write below a list element marks the list itself changed: element
    // nodes are temporary, the list is what subscribers and persistence see.
    void markLists() {
        for (Node* l : containers_) l->changed();
    }

    bool allowed(const Node& n) const {
        const Authorizer& a = api_.authorizer();
        return !a || a(req_.client, req_.op, n);
    }

    // Whether a child of an already readable node may be read.
    bool readable(const Node& n) const { return n.readAccess() <= level_; }

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

    // The levels needed to read and write `n`, given its parent's.
    void require(const Node& n) {
        readNeed_ = higher(readNeed_, n.readAccess());
        writeNeed_ = higher(writeNeed_, n.writeAccess());
    }

    Node* resolve() {
        std::string_view path = req_.path;
        require(api_);
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
                size_t index = list->size();
                if (list->keyField()) {
                    std::vector<std::string> keys;
                    list->keys(keys);
                    for (size_t i = 0; i < keys.size(); i++) {
                        if (keys[i] == seg) index = i;
                    }
                } else if (!parseIndex(seg, index)) {
                    index = list->size();
                }
                if (index < list->size()) {
                    std::unique_ptr<Object> element = list->element(index);
                    if (list->keyField()) keyRefs_.push_back({element->child(list->keyField()), list, index});
                    temps_.push_back(std::move(element));
                    containers_.push_back(list);
                    next = temps_.back().get();
                }
            } else if (node->type() == NodeType::Array) {
                auto* array = static_cast<ArrayNode*>(node);
                size_t index;
                if (parseIndex(seg, index) && index < array->size()) {
                    temps_.push_back(array->element(index));
                    containers_.push_back(array);
                    next = temps_.back().get();
                }
            }
            if (!next) {
                replyError(Status::NotFound, "no such node", p.substr(0, slash));
                return nullptr;
            }
            require(*next);
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
            // The schema this client would read, whole, without the state of
            // remote nodes (online, advertised hash), which changes with
            // peers and reaches subscribers as changes. Otherwise two gateways
            // mounting each other would re-advertise each other's hashes
            // forever.
            HashSink sink;
            JsonWriter hw(sink);
            hashing_ = true;
            renderSchema(hw, target, api_.config().maxDepth, Filter());
            hashing_ = false;
            char hex[9];
            snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(sink.hash));
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
                    error(Status::Unauthorized, "not authorized");
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

    bool validateShape(const Object& obj, JsonObjectConst shape) {
        for (JsonPairConst kv : shape) {
            std::string_view key(kv.key().c_str(), kv.key().size());
            if (!push(key)) return error(Status::BadRequest, "shape too deep");
            const Node* c = obj.child(key);
            if (!c) return error(Status::NotFound, "no such key");
            if (!readable(*c)) return error(Status::Unauthorized, "not authorized");
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
            default: detail::renderValue(w, n, depth, render_); break;
        }
    }

    void renderChildren(JsonWriter& w, const Object& o, int depth, const Filter& f, View view) {
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
                }
            }
        }
        w.endObject();
    }

    // Out of line, like schemaAttributes, so renders without a shape don't
    // carry its frame.
    __attribute__((noinline)) void renderShaped(JsonWriter& w, const Object& o, int depth, const Filter& f, View view) {
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

    void renderSchema(JsonWriter& w, const Node& n, int depth, const Filter& f) {
#if defined(TESSER_NO_SCHEMA)
        (void)n;
        (void)depth;
        (void)f;
        w.null();
        return;
#endif
        w.beginObject();
        schemaAttributes(w, n);
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

    // Everything but the children, out of line: the recursion through
    // renderSchema and renderChildren then keeps small frames (with -Og, the
    // ESP-IDF default, this one alone takes over 500 bytes).
    __attribute__((noinline)) void schemaAttributes(JsonWriter& w, const Node& n) {
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
                if (const NodeMeta* m = a.meta(); m && m->hasRange) {
                    w.key("min");
                    w.number(m->min);
                    w.key("max");
                    w.number(m->max);
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
                if (const NodeMeta* m = a.meta(); m && m->hasRange) {
                    w.key("min");
                    w.number(m->min);
                    w.key("max");
                    w.number(m->max);
                }
                w.endObject();
                break;
            }
            case NodeType::List: {
                w.string("list");
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
                if (uint32_t hash = hashing_ ? 0 : r.advertisedSchema()) {
                    char hex[9];
                    snprintf(hex, sizeof(hex), "%08lx", static_cast<unsigned long>(hash));
                    w.key("schema");
                    w.string(hex);
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
        if (const NodeMeta* m = n.meta(); m && m->doc) {
            w.key("description");
            w.string(m->doc);
        }
        if (const NodeMeta* m = n.meta(); m && m->ui) renderUi(w, *m->ui);
    }

    static void renderUi(JsonWriter& w, const UiMeta& m) {
        if (m.label) {
            w.key("label");
            w.string(m.label);
        }
        if (m.unit) {
            w.key("unit");
            w.string(m.unit);
        }
        if (m.step != 0) {
            w.key("step");
            w.number(static_cast<double>(m.step), 7);
        }
        if (!m.hints) return;
        w.key("ui");
        w.beginObject();
        for (const UiHint* h = m.hints; h; h = h->next) {
            w.key(h->key);
            switch (h->kind) {
                case UiHint::Kind::Flag: w.boolean(true); break;
                case UiHint::Kind::String: w.string(h->str ? h->str : ""); break;
                case UiHint::Kind::Number: w.number(static_cast<double>(h->num), 7); break;
            }
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
        if (target.type() == NodeType::Action || target.type() == NodeType::File) {
            return replyError(Status::NotAllowed, "actions and files can't be subscribed to", base_);
        }
        if (!temps_.empty()) {
            return replyError(Status::NotAllowed, "subscribe to the list or array itself, not to its elements", base_);
        }
        Status allowed = api_.subscriptionAllowed(req_.subscriber);
        if (allowed != Status::Ok) return replyError(allowed, "too many subscriptions", base_);
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
        sub.events = req_.query.events;
        sub.remotes = req_.query.remotes;
        sub.access = level_;
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
                if (!keyWriteOk(v, body)) return replyError();
                Check c = v.check(body);
                if (c.isOk()) c = v.apply(body);
                if (!c.isOk()) return replyError(c.status, c.message, base_);
                v.changed();
                markLists();
                JsonWriter w(reply_.begin(Status::Ok));
                detail::renderValue(w, v, 0, render_);
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
                std::unique_ptr<Object> params;
                if (a.takesObject() && !(params = fillParams(a, body))) return replyError();
                Call call(api_, &reply_, body, req_.client, base_.empty() ? std::string_view("/") : base_);
                a.invoke(body, params.get(), call);
                markLists();
                if (call.deferred() || call.replied()) return;
                if (call.status() != Status::Ok) return replyError(call.status(), call.message(), base_);
                JsonWriter w(reply_.begin(Status::Ok));
                w.null();
                detail::finishBody(reply_, w, base_);
                return;
            }
            case NodeType::Event: replyError(Status::NotAllowed, "events can't be set", base_); return;
            case NodeType::File: replyError(Status::NotAllowed, "upload files over HTTP", base_); return;
            case NodeType::Array: {
                auto& a = static_cast<ArrayNode&>(target);
                if (!validatePatch(a, body, writeNeed_)) return replyError();
                a.apply(body);
                a.changed();
                markLists();
                JsonWriter w(reply_.begin(Status::Ok));
                a.write(w);
                detail::finishBody(reply_, w, base_);
                return;
            }
            case NodeType::List: {
                auto& list = static_cast<ListNode&>(target);
                if (!validatePatch(list, body, writeNeed_)) return replyError();
                if (!applyPatch(list, body, false) || !applyPatch(list, body, true)) {
                    partial_ = applied_ > 0;
                    return replyError();
                }
                markLists();
                JsonWriter w(reply_.begin(Status::Ok));
                detail::renderValue(w, list, api_.config().maxDepth, render_);
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
        if (!validatePatch(obj, body, writeNeed_)) return replyError();
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

    // A keyed list replaced by an array: every item names its element by a
    // valid key, once; it patches that element, or a default one.
    bool validateKeyed(ListNode& list, JsonArrayConst items, Access need) {
        const char* field = list.keyField();
        std::vector<std::string> keys;
        list.keys(keys);
        std::vector<std::string_view> seen;
        size_t i = 0;
        for (JsonVariantConst item : items) {
            if (!item.is<JsonObjectConst>()) {
                pushIndex(i);
                return error(Status::InvalidValue, "expected object");
            }
            Check c = ListNode::checkKey(item[field]);
            if (!c.isOk()) {
                pushIndex(i);
                push(field);
                return error(c.status, c.message);
            }
            JsonString k = item[field].as<JsonString>();
            std::string_view key(k.c_str(), k.size());
            if (!push(key)) return error(Status::BadRequest, "patch too deep");
            for (std::string_view s : seen) {
                if (s == key) return error(Status::InvalidValue, "duplicate key");
            }
            seen.push_back(key);
            size_t at = keys.size();
            for (size_t j = 0; j < keys.size(); j++) {
                if (keys[j] == key) at = j;
            }
            std::unique_ptr<Object> element = at < keys.size() ? list.element(at) : list.prototype();
            if (!validatePatch(*element, item, higher(need, element->writeAccess()))) return false;
            pop();
            i++;
        }
        return true;
    }

    // A write to the key of a keyed list's element (renaming it, reached
    // through /list/<key>) must give a valid key no other element has.
    bool keyWriteOk(const Node& n, JsonVariantConst v) {
        for (const KeyRef& ref : keyRefs_) {
            if (ref.node != &n) continue;
            Check c = ListNode::checkKey(v);
            if (!c.isOk()) return error(c.status, c.message);
            std::vector<std::string> keys;
            ref.list->keys(keys);
            for (size_t j = 0; j < keys.size(); j++) {
                if (j != ref.index && keys[j] == v.as<const char*>()) return error(Status::InvalidValue, "key already used");
            }
        }
        return true;
    }

    // An action's object argument: a default one, validated and filled from
    // `arg` like a patch, so errors name the field (/add/az). Null (with the
    // error recorded) if `arg` doesn't fit.
    std::unique_ptr<Object> fillParams(const ActionNode& a, JsonVariantConst arg) {
        std::unique_ptr<Object> params = a.params();
        if (arg.isNull()) return params;  // every field keeps its default
        bool was = filling_;
        filling_ = true;
        bool ok = validatePatch(*params, arg, Access::Public) && applyPatch(*params, arg, false);
        filling_ = was;
        if (!ok) params.reset();
        return params;
    }

    // `need`: the level writing `n` takes, its ancestors' included.
    bool validatePatch(Node& n, JsonVariantConst v, Access need) {
        // An action's argument isn't the tree: the action itself was checked.
        if (!filling_ && (level_ < need || !allowed(n))) return error(Status::Unauthorized, "not authorized");
        switch (n.type()) {
            case NodeType::Object: {
                if (!v.is<JsonObjectConst>()) return error(Status::InvalidValue, "expected object");
                auto& o = static_cast<Object&>(n);
                for (JsonPairConst kv : v.as<JsonObjectConst>()) {
                    std::string_view key(kv.key().c_str(), kv.key().size());
                    if (!push(key)) return error(Status::BadRequest, "patch too deep");
                    Node* c = o.child(key);
                    if (!c) return error(Status::NotFound, "no such key");
                    if (!validatePatch(*c, kv.value(), higher(need, c->writeAccess()))) return false;
                    pop();
                }
                return true;
            }
            case NodeType::List: return validateList(static_cast<ListNode&>(n), v, need);
            default: return validateLeaf(n, v);
        }
    }

    // The cases that don't recurse, and lists, out of line: walking a deep
    // object then keeps small frames (with -Og, the ESP-IDF default).
    __attribute__((noinline)) bool validateLeaf(Node& n, JsonVariantConst v) {
        switch (n.type()) {
            case NodeType::Value: {
                auto& val = static_cast<ValueNode&>(n);
                if (!val.writable()) return error(Status::ReadOnly, "read-only");
                if (!keyWriteOk(val, v)) return false;
                Check c = val.check(v);
                return c.isOk() || error(c.status, c.message);
            }
            case NodeType::Action: {
                auto& a = static_cast<ActionNode&>(n);
                if (a.deferring()) return error(Status::BadRequest, "call deferred actions directly");
                Check c = a.check(v);
                if (!c.isOk()) return error(c.status, c.message);
                return !a.takesObject() || fillParams(a, v) != nullptr;
            }
            case NodeType::Custom: {
                auto& cu = static_cast<CustomNode&>(n);
                if (!cu.writable()) return error(Status::ReadOnly, "read-only");
                Check c = cu.check(v);
                return c.isOk() || error(c.status, c.message);
            }
            case NodeType::Event: return error(Status::NotAllowed, "events can't be set");
            case NodeType::File: return error(Status::NotAllowed, "upload files over HTTP");
            case NodeType::Remote: return error(Status::NotAllowed, "write remote nodes directly");
            case NodeType::Array: {
                auto& a = static_cast<ArrayNode&>(n);
                if (!a.writable()) return error(Status::ReadOnly, "read-only");
                Check c = a.checkLength(v);
                if (!c.isOk()) return error(c.status, c.message);
                size_t i = 0;
                for (JsonVariantConst item : v.as<JsonArrayConst>()) {
                    c = a.checkElement(item);
                    if (!c.isOk()) {
                        pushIndex(i);  // names the element; the walk stops here
                        return error(c.status, c.message);
                    }
                    i++;
                }
                return true;
            }
            case NodeType::Object:
            case NodeType::List: break;
        }
        return false;
    }

    __attribute__((noinline)) bool validateList(ListNode& list, JsonVariantConst v, Access need) {
        if (!v.is<JsonArrayConst>()) return error(Status::InvalidValue, "expected array");
        JsonArrayConst items = v.as<JsonArrayConst>();
        if (items.size() > list.maxSize()) return error(Status::InvalidValue, "too many elements");
        if (list.keyField()) return validateKeyed(list, items, need);
        size_t i = 0;
        for (JsonVariantConst item : items) {
            if (!pushIndex(i)) return error(Status::BadRequest, "patch too deep");
            if (!item.is<JsonObjectConst>()) return error(Status::InvalidValue, "expected object");
            std::unique_ptr<Object> element = i < list.size() ? list.element(i) : list.prototype();
            if (!validatePatch(*element, item, higher(need, element->writeAccess()))) return false;
            pop();
            i++;
        }
        return true;
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
            case NodeType::List: return applyList(static_cast<ListNode&>(n), v, actions);
            default: return applyLeaf(n, v, actions);
        }
    }

    __attribute__((noinline)) bool applyLeaf(Node& n, JsonVariantConst v, bool actions) {
        switch (n.type()) {
            case NodeType::Value: {
                if (actions) return true;
                Check c = static_cast<ValueNode&>(n).apply(v);
                if (!c.isOk()) return error(c.status, c.message);
                if (filling_) return true;  // an action's argument: not state
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
                auto& a = static_cast<ActionNode&>(n);
                std::unique_ptr<Object> params;
                if (a.takesObject() && !(params = fillParams(a, v))) return false;
                std::string path = trailPath();
                Call call(api_, nullptr, v, req_.client, path);
                a.invoke(v, params.get(), call);
                if (call.status() != Status::Ok) return error(call.status(), call.message());
                applied_++;
                return true;
            }
            case NodeType::Event:
            case NodeType::File:
            case NodeType::Remote: return true;
            case NodeType::Array: {
                if (actions) return true;
                static_cast<ArrayNode&>(n).apply(v);
                if (filling_) return true;
                n.changed();
                applied_++;
                return true;
            }
            case NodeType::Object:
            case NodeType::List: break;
        }
        return true;
    }

    __attribute__((noinline)) bool applyList(ListNode& list, JsonVariantConst v, bool actions) {
        // The array replaces the list: its length is the new size, and
        // each element is patched (new ones start from defaults).
        JsonArrayConst items = v.as<JsonArrayConst>();
        const char* keyField = list.keyField();
        if (!actions && keyField) {
            // Elements follow their keys: kept ones move, missing
            // ones go, new ones start from defaults.
            std::vector<std::string> keys;
            list.keys(keys);
            std::vector<int> from;
            for (JsonVariantConst item : items) {
                int at = -1;
                for (size_t k = 0; k < keys.size(); k++) {
                    if (keys[k] == item[keyField].as<const char*>()) at = static_cast<int>(k);
                }
                from.push_back(at);
            }
            list.reorder(from);
        } else if (!actions) {
            list.resize(items.size());
        }
        size_t i = 0;
        for (JsonVariantConst item : items) {
            if (keyField) {
                JsonString k = item[keyField].as<JsonString>();
                push(std::string_view(k.c_str(), k.size()));
            } else {
                pushIndex(i);
            }
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

    Api& api_;
    const Request& req_;
    Reply& reply_;
    std::string_view base_;  // request path without trailing slash; empty for the root
    int depth_;
    Access level_;                       // the client's
    Access readNeed_ = Access::Public;   // what the target needs, its ancestors' included
    Access writeNeed_ = Access::Public;
    detail::RenderOptions render_;

    std::string_view trail_[kMaxTrail];
    int trailLen_ = 0;
    bool failed_ = false;
    Status errStatus_ = Status::Internal;
    const char* errMessage_ = nullptr;
    std::string errPath_;
    int applied_ = 0;
    bool partial_ = false;
    bool filling_ = false;  // validating or filling an action's object argument
    bool hashing_ = false;  // rendering a schema for view=hash
    std::vector<std::unique_ptr<Node>> temps_;  // list and array elements on the request path
    std::vector<Node*> containers_;             // lists and arrays the path went through
    struct KeyRef {
        const Node* node;  // the key value of an element on the path
        ListNode* list;
        size_t index;
    };
    std::vector<KeyRef> keyRefs_;
    uint16_t indexes_[kMaxTrail];                 // list indexes in the trail...
    uint32_t indexMask_ = 0;                      // ...at the levels whose bit is set
    std::string_view remoteRest_;                 // path below a remote node, forwarded as is
};

}  // namespace

void Api::authorize(Authorizer fn) {
    MutexGuard guard(mutex_);
    authorizer_ = std::move(fn);
}

void Api::authenticate(TokenCheck fn) {
    MutexGuard guard(mutex_);
    tokenCheck_ = std::move(fn);
}

// Under the API lock: checks typically read keys or users from the tree.
Access Api::tokenAccess(std::string_view token) const {
    MutexGuard guard(mutex_);
    return tokenCheck_ ? tokenCheck_(token) : Access::Public;
}

void Api::handle(const Request& request, Reply& reply) {
    if (config_.queued && enqueue(request, reply)) return;
    handleNow(request, reply);
}

FileNode* Api::fileNode(const Request& request, Reply& reply, bool& notFile) {
    MutexGuard guard(mutex_);
    notFile = false;
    return Handler(*this, request, reply).file(notFile);
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
        // The body parsed once already, so only memory can run out here.
        JsonDocument doc;
        bool parsed = q->body.empty() || !deserializeJson(doc, q->body, DeserializationOption::NestingLimit(kWrittenNesting));
        q->request.body = doc.as<JsonVariantConst>();
        QueuedReply reply(q->reply);
        if (parsed) {
            handleNow(q->request, reply);
        } else {
            writeError(reply, Status::Internal, q->request.path, "out of memory");
        }
        if (!reply.transferred) q->reply->release();
        delete q;
    }
}

uint32_t Api::schemaHash() {
    StringReply reply;
    Request req;
    req.path = "/";
    req.query.view = View::Hash;
    req.client.authenticated = true;  // internal
    handleNow(req, reply);
    // "1a2b3c4d", quoted
    return reply.body.size() == 10 ? static_cast<uint32_t>(strtoul(reply.body.c_str() + 1, nullptr, 16)) : 0;
}

size_t Api::queuedRequests() const {
    MutexGuard guard(queueMutex_);
    return queue_.size();
}

}  // namespace tesser

#include "tesser/detail/api_handler.h"

#include <stdio.h>
#include <stdlib.h>

#include "tesser/envelope.h"
#include "tesser/file.h"
#include "tesser/remote.h"

// Writing (detail::Handler): a patch is validated whole, then applied, and
// '*' fans it out.

namespace tesser {
namespace detail {

// A set whose path has a '*' segment, at star_: one set per child of
// `parent`, each run like a request of its own (docs/DESIGN.md section
// 6.5). Children the client can't read aren't matched.
void Handler::fanOut(Node& parent) {
    if (req_.query.view != View::Value || !req_.query.keys.empty() || !req_.query.exclude.empty()) {
        return replyError(Status::BadRequest, "set takes no query options", base_);
    }
    if (level_ < readNeed_) return replyError(Status::Unauthorized, Reason::NotAuthorized, "not authorized", base_);
    std::vector<detail::Match> matches;
    auto add = [&](std::string name, bool offline) {
        matches.emplace_back();
        matches.back().name = std::move(name);
        matches.back().offline = offline;
    };
    auto indexes = [&](size_t n) {
        for (size_t i = 0; i < n; i++) add(std::to_string(i), false);
    };
    switch (parent.type()) {
        case NodeType::Object:
            for (const Node* c = static_cast<const Object&>(parent).first(); c; c = c->next()) {
                if (!readable(*c)) continue;
                bool offline = c->type() == NodeType::Remote && !static_cast<const RemoteNode*>(c)->online();
                add(c->name(), offline);
            }
            break;
        case NodeType::List: {
            auto& list = static_cast<ListNode&>(parent);
            if (!list.keyField()) {
                indexes(list.size());
                break;
            }
            std::vector<std::string> keys;
            list.keys(keys);
            for (std::string& k : keys) add(std::move(k), false);
            break;
        }
        case NodeType::Array: indexes(static_cast<ArrayNode&>(parent).size()); break;
        default: return replyError(Status::NotFound, "no such node", base_.substr(0, star_ + 1));
    }
    detail::fanOut(api_, req_, reply_, base_.substr(0, star_ - 1), base_.substr(star_ + 1), base_, matches);
}

void Handler::set(Node& target) {
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
            if (call.status() != Status::Ok) return replyError(call.status(), call.reason(), call.message(), base_);
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

void Handler::setObject(Object& obj, JsonVariantConst body) {
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
bool Handler::validateKeyed(ListNode& list, JsonArrayConst items, Access need) {
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
bool Handler::keyWriteOk(const Node& n, JsonVariantConst v) {
    for (const KeyRef& ref : keyRefs_) {
        if (ref.node != &n) continue;
        Check c = ListNode::checkKey(v);
        if (!c.isOk()) return error(c.status, c.message);
        JsonString key = v.as<JsonString>();
        if (ref.list->find(std::string_view(key.c_str(), key.size()), ref.index) < ref.list->size()) {
            return error(Status::InvalidValue, "key already used");
        }
    }
    return true;
}

// An action's object argument: a default one, validated and filled from
// `arg` like a patch, so errors name the field (/add/az). Null (with the
// error recorded) if `arg` doesn't fit.
std::unique_ptr<Object> Handler::fillParams(const ActionNode& a, JsonVariantConst arg) {
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
bool Handler::validatePatch(Node& n, JsonVariantConst v, Access need) {
    // An action's argument isn't the tree: the action itself was checked.
    if (!filling_ && (level_ < need || !allowed(n))) return error(Status::Unauthorized, "not authorized", Reason::NotAuthorized);
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
bool Handler::validateLeaf(Node& n, JsonVariantConst v) {
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

bool Handler::validateList(ListNode& list, JsonVariantConst v, Access need) {
    if (!list.writable()) return error(Status::ReadOnly, "read-only");
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
bool Handler::applyPatch(Node& n, JsonVariantConst v, bool actions) {
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

bool Handler::applyLeaf(Node& n, JsonVariantConst v, bool actions) {
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
            if (call.status() != Status::Ok) return error(call.status(), call.message(), call.reason());
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

bool Handler::applyList(ListNode& list, JsonVariantConst v, bool actions) {
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
}  // namespace detail
}  // namespace tesser

// Persistence (docs/DESIGN.md section 12).
#include "tesser/api.h"
#include "tesser/detail/lazy_patch.h"
#include "tesser/envelope.h"
#include "tesser/storage.h"

namespace tesser {

namespace {

bool isLeaf(const Node& n) {
    return n.type() == NodeType::Value || n.type() == NodeType::Custom || n.type() == NodeType::List ||
           n.type() == NodeType::Array;
}

// Whether `n` holds persisted state: a persisted leaf, or an object with one
// below it.
bool holdsPersisted(const Node& n, bool inherited) {
    bool p = inherited || n.persisted();
    if (n.type() != NodeType::Object) return p && isLeaf(n);
    for (const Node* c = static_cast<const Object&>(n).first(); c; c = c->next()) {
        if (holdsPersisted(*c, p)) return true;
    }
    return false;
}

// Newest change among persisted values at or below `n` (0: none ever changed).
void newestPersisted(const Node& n, bool inherited, uint16_t& newest) {
    bool p = inherited || n.persisted();
    if (n.type() == NodeType::Object) {
        for (const Node* c = static_cast<const Object&>(n).first(); c; c = c->next()) newestPersisted(*c, p, newest);
    } else if (p && isLeaf(n)) {
        uint16_t g = n.generation();
        if (g && (newest == 0 || newerGeneration(g, newest))) newest = g;
    }
}

void writeLeaf(const Node& n, JsonWriter& w) {
    detail::RenderOptions options;
    options.secrets = true;
    detail::writeLeaf(w, n, options);
}

void writePersisted(const Object& o, bool inherited, detail::LazyPatch& lp, JsonWriter& w) {
    for (const Node* c = o.first(); c; c = c->next()) {
        bool p = inherited || c->persisted();
        if (c->type() == NodeType::Object) {
            if (!lp.enter(c->name())) continue;
            writePersisted(static_cast<const Object&>(*c), p, lp, w);
            lp.leave();
        } else if (p && isLeaf(*c)) {
            lp.key(c->name());
            writeLeaf(*c, w);
        }
    }
}

// A top-level node's record: its value, or for an object the sparse object
// of its persisted values.
std::string record(const Node& top, bool inherited) {
    std::string out;
    StringSink sink(out);
    JsonWriter w(sink);
    if (top.type() == NodeType::Object) {
        detail::LazyPatch lp(w);
        writePersisted(static_cast<const Object&>(top), inherited || top.persisted(), lp, w);
        if (!lp.finish()) out = "{}";
    } else {
        writeLeaf(top, w);
    }
    return out;
}

// Applies stored state leniently: whatever doesn't fit the current tree is
// skipped, so renamed or removed fields never break a boot. `notify` marks
// what it applies changed (a restore at run time; a boot load needn't).
struct Applier {
    bool notify = false;
    std::vector<std::string>* skipped = nullptr;
    std::string path;

    void skip(const char* problem) {
        logWarning("stored state: %s %s, skipped", path.c_str(), problem);
        if (skipped) skipped->push_back(path);
    }

    // `c` is the node at `path` (null if there is none).
    void apply(Node* c, JsonVariantConst value, bool inherited) {
        bool p = c && (inherited || c->persisted());
        if (!c) return skip("no longer exists");
        if (c->type() == NodeType::Object) {
            if (!value.is<JsonObjectConst>()) return skip("is now an object");
            for (JsonPairConst kv : value.as<JsonObjectConst>()) {
                std::string_view key(kv.key().c_str(), kv.key().size());
                size_t len = path.size();
                path += '/';
                path.append(key.data(), key.size());
                apply(static_cast<Object*>(c)->child(key), kv.value(), p);
                path.resize(len);
            }
            return;
        }
        if (!p) return skip("is not persisted");
        if (c->type() == NodeType::Value) {
            auto& v = static_cast<ValueNode&>(*c);
            Check chk = v.check(value);
            if (chk.isOk()) chk = v.apply(value);  // readOnly() values are restored too
            if (!chk.isOk()) return skip(chk.message ? chk.message : "rejected");
        } else if (c->type() == NodeType::List) {
            auto& list = static_cast<ListNode&>(*c);
            if (!value.is<JsonArrayConst>()) return skip("is now a list");
            JsonArrayConst items = value.as<JsonArrayConst>();
            const char* field = list.keyField();
            std::vector<JsonVariantConst> kept;  // items that make it into the list
            if (field) {
                // Matched by key, like a write: elements keep what the stored
                // state doesn't say.
                std::vector<std::string> keys;
                list.keys(keys);
                std::vector<int> from;
                std::vector<std::string> seen;
                size_t n = 0;
                for (JsonVariantConst item : items) {
                    size_t at = path.size();
                    path += '/';
                    path += std::to_string(n++);
                    const char* key = item[field].as<const char*>();
                    bool dup = false;
                    for (const std::string& k : seen) dup = dup || (key && k == key);
                    if (!ListNode::checkKey(item[field]).isOk() || dup) {
                        skip("has no valid key");
                    } else if (kept.size() < list.maxSize()) {
                        int index = -1;
                        for (size_t k = 0; k < keys.size(); k++) {
                            if (keys[k] == key) index = static_cast<int>(k);
                        }
                        from.push_back(index);
                        seen.push_back(key);
                        kept.push_back(item);
                    }
                    path.resize(at);
                }
                list.reorder(from);
            } else {
                for (JsonVariantConst item : items) {
                    if (kept.size() < list.maxSize()) kept.push_back(item);
                }
                list.resize(kept.size());
            }
            size_t i = 0;
            for (JsonVariantConst item : kept) {
                size_t at = path.size();
                path += '/';
                path += field ? std::string(item[field].as<const char*>()) : std::to_string(i);
                if (item.is<JsonObjectConst>()) {
                    bool was = notify;
                    notify = false;  // the list is marked as a whole
                    apply(list.element(i).get(), item, true);
                    notify = was;
                }
                path.resize(at);
                i++;
            }
        } else if (c->type() == NodeType::Array) {
            auto& arr = static_cast<ArrayNode&>(*c);
            Check chk = arr.check(value);  // readOnly() arrays are restored too
            if (!chk.isOk()) return skip(chk.message ? chk.message : "rejected");
            arr.apply(value);
        } else if (c->type() == NodeType::Custom) {
            auto& cu = static_cast<CustomNode&>(*c);
            Check chk = cu.check(value);
            if (!chk.isOk() || cu.apply(value) != Status::Ok) return skip("rejected");
        } else {
            return skip("can't be restored");
        }
        if (notify) c->changed();
    }
};

}  // namespace

void Api::persistence(Storage& storage, uint32_t debounceMs) {
    MutexGuard guard(mutex_);
    storage_ = &storage;
    debounceMs_ = debounceMs;
    persistHook_ = [](Api& api, uint32_t nowMs) { api.checkPersistence(nowMs); };
    savedGen_ = seenGen_ = currentGeneration();
}

std::string Api::persistedState() {
    MutexGuard guard(mutex_);
    std::string out;
    StringSink sink(out);
    JsonWriter w(sink);
    detail::LazyPatch lp(w);
    writePersisted(*this, persisted(), lp, w);
    if (!lp.finish()) out = "{}";
    return out;
}

bool Api::saveRecords(bool all) {
    if (!storage_) return false;
    uint16_t gen = currentGeneration();
    bool ok = true;
    for (const Node* c = first(); c; c = c->next()) {
        if (!holdsPersisted(*c, persisted())) continue;
        if (!all) {
            uint16_t newest = 0;
            newestPersisted(*c, persisted(), newest);
            if (!newerGeneration(newest, savedGen_)) continue;  // its record is current
        }
        if (!storage_->save(c->name(), record(*c, persisted()))) {
            logError("saving state %s failed", c->name());
            ok = false;
        }
    }
    // After a failure the dirty records stay dirty and are written again.
    if (ok) savedGen_ = seenGen_ = gen;
    return ok;
}

bool Api::save() {
    MutexGuard guard(mutex_);
    return saveRecords(true);
}

bool Api::load() {
    MutexGuard guard(mutex_);
    if (!storage_) return false;
    bool any = false;
    Applier a;
    for (Node* c = first(); c; c = c->next()) {
        if (!holdsPersisted(*c, persisted())) continue;
        std::string data;
        if (!storage_->load(c->name(), data)) continue;
        JsonDocument doc;
        a.path = "/";
        a.path += c->name();
        if (deserializeJson(doc, data, DeserializationOption::NestingLimit(kWrittenNesting))) {
            logWarning("stored state %s doesn't parse; keeping defaults", a.path.c_str());
            continue;
        }
        a.apply(c, doc.as<JsonVariantConst>(), persisted());
        any = true;
    }
    savedGen_ = seenGen_ = currentGeneration();
    return any;
}

Status Api::restore(std::string_view json, std::vector<std::string>* skipped) {
    MutexGuard guard(mutex_);
    JsonDocument doc;
    if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(kWrittenNesting)) ||
        !doc.is<JsonObjectConst>()) {
        return Status::BadRequest;
    }
    Applier a;
    a.notify = true;
    a.skipped = skipped;
    a.apply(this, doc.as<JsonVariantConst>(), persisted());
    if (storage_ && !saveRecords(true)) return Status::Internal;
    return Status::Ok;
}

void Api::checkPersistence(uint32_t nowMs) {
    // A walk over the tree: at most every 50 ms.
    if (nowMs - lastPersistCheckMs_ < 50) return;
    lastPersistCheckMs_ = nowMs;
    uint16_t newest = 0;
    newestPersisted(*this, persisted(), newest);
    if (!newerGeneration(newest, savedGen_)) return;
    if (newest != seenGen_) {  // a new change: restart the debounce
        seenGen_ = newest;
        changedAtMs_ = nowMs;
        return;
    }
    if (nowMs - changedAtMs_ >= debounceMs_) saveRecords(false);
}

}  // namespace tesser

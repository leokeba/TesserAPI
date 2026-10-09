// Persistence (docs/DESIGN.md section 12).
#include "tesser/api.h"
#include "tesser/detail/lazy_patch.h"
#include "tesser/storage.h"

namespace tesser {

namespace {

bool isLeaf(const Node& n) {
    return n.type() == NodeType::Value || n.type() == NodeType::Custom || n.type() == NodeType::List;
}

// Newest change among persisted values (0: none ever changed).
void newestPersisted(const Object& o, bool inherited, uint16_t& newest) {
    for (const Node* c = o.first(); c; c = c->next()) {
        bool p = inherited || c->persisted();
        if (c->type() == NodeType::Object) {
            newestPersisted(static_cast<const Object&>(*c), p, newest);
        } else if (p && isLeaf(*c)) {
            uint16_t g = c->generation();
            if (g && (newest == 0 || newerGeneration(g, newest))) newest = g;
        }
    }
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
            detail::writeLeaf(w, *c);
        }
    }
}

// Applies a stored state leniently: whatever doesn't fit the current tree is
// skipped, so renamed or removed fields never break a boot.
void applyStored(Object& o, JsonObjectConst stored, bool inherited, std::string& path) {
    for (JsonPairConst kv : stored) {
        std::string_view key(kv.key().c_str(), kv.key().size());
        size_t len = path.size();
        path += '/';
        path.append(key.data(), key.size());
        Node* c = o.child(key);
        bool p = c && (inherited || c->persisted());
        const char* problem = nullptr;
        if (!c) {
            problem = "no longer exists";
        } else if (c->type() == NodeType::Object) {
            if (kv.value().is<JsonObjectConst>()) {
                applyStored(static_cast<Object&>(*c), kv.value().as<JsonObjectConst>(), p, path);
            } else {
                problem = "is now an object";
            }
        } else if (!p) {
            problem = "is not persisted";
        } else if (c->type() == NodeType::Value) {
            auto& v = static_cast<ValueNode&>(*c);
            Check chk = v.check(kv.value());
            if (chk.isOk()) chk = v.apply(kv.value());  // readOnly() values are restored too
            if (!chk.isOk()) problem = chk.message ? chk.message : "rejected";
        } else if (c->type() == NodeType::List) {
            auto& list = static_cast<ListNode&>(*c);
            if (!kv.value().is<JsonArrayConst>()) {
                problem = "is now a list";
            } else {
                JsonArrayConst items = kv.value().as<JsonArrayConst>();
                list.resize(items.size() < list.maxSize() ? items.size() : list.maxSize());
                size_t i = 0;
                for (JsonVariantConst item : items) {
                    if (i >= list.size()) break;
                    size_t at = path.size();
                    path += '/';
                    path += std::to_string(i);
                    if (item.is<JsonObjectConst>()) {
                        applyStored(*list.element(i), item.as<JsonObjectConst>(), true, path);
                    }
                    path.resize(at);
                    i++;
                }
            }
        } else if (c->type() == NodeType::Custom) {
            auto& cu = static_cast<CustomNode&>(*c);
            Check chk = cu.check(kv.value());
            if (!chk.isOk() || cu.apply(kv.value()) != Status::Ok) problem = "rejected";
        } else {
            problem = "can't be restored";
        }
        if (problem) logWarning("stored state: %s %s, skipped", path.c_str(), problem);
        path.resize(len);
    }
}

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

bool Api::save() {
    MutexGuard guard(mutex_);
    if (!storage_) return false;
    uint16_t gen = currentGeneration();
    if (!storage_->save(persistedState())) {
        logError("saving state failed");
        return false;
    }
    savedGen_ = seenGen_ = gen;
    return true;
}

bool Api::load() {
    MutexGuard guard(mutex_);
    std::string data;
    if (!storage_ || !storage_->load(data)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, data) || !doc.is<JsonObjectConst>()) {
        logWarning("stored state doesn't parse; keeping defaults");
        return false;
    }
    std::string path;
    applyStored(*this, doc.as<JsonObjectConst>(), persisted(), path);
    savedGen_ = seenGen_ = currentGeneration();
    return true;
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
    if (nowMs - changedAtMs_ >= debounceMs_) save();
}

}  // namespace tesser

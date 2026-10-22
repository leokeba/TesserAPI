// Persistence (docs/DESIGN.md section 12).
#include <algorithm>

#include "tesser/api.h"
#include "tesser/budget.h"
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

// Newest change after `since` (as of `now`) among persisted values at or
// below `n`; `newest` stays 0 when there is none.
void newestPersisted(const Node& n, bool inherited, uint32_t since, uint32_t now, uint32_t& newest) {
    bool p = inherited || n.persisted();
    if (n.type() == NodeType::Object) {
        for (const Node* c = static_cast<const Object&>(n).first(); c; c = c->next()) {
            newestPersisted(*c, p, since, now, newest);
        }
    } else if (p && isLeaf(n)) {
        uint32_t g = n.generation();
        if (changedSince(g, since, now) && (newest == 0 || g - since > newest - since)) newest = g;
    }
}

void writeLeaf(const Node& n, JsonWriter& w) {
    detail::RenderOptions options;
    options.secrets = true;
    detail::writeLeaf(w, n, options);
}

uint32_t hashOf(const Node& n) {
    HashSink sink;
    JsonWriter w(sink);
    writeLeaf(n, w);
    return sink.hash;
}

// `skip(n)`: a value left out (at its default).
template <class Skip>
void writePersisted(const Object& o, bool inherited, detail::LazyPatch& lp, JsonWriter& w, const Skip& skip) {
    for (const Node* c = o.first(); c; c = c->next()) {
        bool p = inherited || c->persisted();
        if (c->type() == NodeType::Object) {
            if (!lp.enter(c->name())) continue;
            writePersisted(static_cast<const Object&>(*c), p, lp, w, skip);
            lp.leave();
        } else if (p && isLeaf(*c) && !skip(*c)) {
            lp.key(c->name());
            writeLeaf(*c, w);
        }
    }
}

// A top-level node's record: its value, or for an object the sparse object
// of its persisted values; empty when all are at their defaults.
template <class Skip>
void writeRecord(const Node& top, bool inherited, JsonWriter& w, const Skip& skip, bool& any) {
    if (top.type() == NodeType::Object) {
        detail::LazyPatch lp(w);
        writePersisted(static_cast<const Object&>(top), inherited || top.persisted(), lp, w, skip);
        any = lp.finish();
    } else {
        any = !skip(top);
        if (any) writeLeaf(top, w);
    }
}

enum class Record : uint8_t { Written, Empty, NoRoom };

// Measured first, then written in one allocation of its exact size, from
// the byte budget (docs/DESIGN.md section 15): a record never grows by
// doubling on a short heap.
template <class Skip>
Record record(const Node& top, bool inherited, const Skip& skip, std::string& out, Reservation& held) {
    bool any = false;
    CountingSink counter;
    {
        JsonWriter w(counter);
        writeRecord(top, inherited, w, skip, any);
    }
    if (!any) return Record::Empty;
    held = memoryBudget().reserve(counter.count + 1, 0, true, true, mallocPool(counter.count + 1));
    if (!held) return Record::NoRoom;
    out.clear();
    out.reserve(counter.count + 1);
    held.settle();
    StringSink sink(out);
    JsonWriter w(sink);
    writeRecord(top, inherited, w, skip, any);
    return Record::Written;
}

// Persisted leaves at or below `n`.
void persistedLeaves(const Node& n, bool inherited, std::vector<const Node*>& out) {
    bool p = inherited || n.persisted();
    if (n.type() == NodeType::Object) {
        for (const Node* c = static_cast<const Object&>(n).first(); c; c = c->next()) persistedLeaves(*c, p, out);
    } else if (p && isLeaf(n)) {
        out.push_back(&n);
    }
}

constexpr const char* kVersionKey = "@version";  // no node can be named so

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
        applyOther(c, value);
    }

    // Everything but objects, out of line: the recursion through objects
    // then keeps small frames (with -Og, the ESP-IDF default).
    __attribute__((noinline)) void applyOther(Node* c, JsonVariantConst value) {
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
    captureDefaults();
}

void Api::persistVersion(uint32_t version, Migration migrate) {
    MutexGuard guard(mutex_);
    persistVersion_ = version;
    migrate_ = std::move(migrate);
}

void Api::captureDefaults() {
    std::vector<const Node*> leaves;
    persistedLeaves(*this, persisted(), leaves);
    bool added = false;
    for (const Node* n : leaves) {
        auto at = std::lower_bound(defaults_.begin(), defaults_.end(), n,
                                   [](const std::pair<const Node*, uint32_t>& d, const Node* k) { return d.first < k; });
        if (at != defaults_.end() && at->first == n) continue;  // the first one seen stays
        defaults_.emplace_back(n, hashOf(*n));
        added = true;
    }
    if (added) std::sort(defaults_.begin(), defaults_.end());
}

bool Api::atDefault(const Node& n) const {
    auto at = std::lower_bound(defaults_.begin(), defaults_.end(), &n,
                               [](const std::pair<const Node*, uint32_t>& d, const Node* k) { return d.first < k; });
    return at != defaults_.end() && at->first == &n && at->second == hashOf(n);
}

std::string Api::persistedState() {
    MutexGuard guard(mutex_);
    std::string out;
    StringSink sink(out);
    JsonWriter w(sink);
    detail::LazyPatch lp(w);
    if (persistVersion_) {
        lp.key(kVersionKey);
        w.uinteger(persistVersion_);
    }
    // Every value, defaults included, unlike the records: a restore must
    // bring back the backed-up state whatever changed since, defaults too.
    writePersisted(*this, persisted(), lp, w, [](const Node&) { return false; });
    if (!lp.finish()) out = "{}";
    return out;
}

bool Api::saveRecords(bool all) {
    if (!storage_) return false;
    uint32_t gen = currentGeneration();
    bool ok = true;
    auto skip = [this](const Node& n) { return atDefault(n); };
    for (const Node* c = first(); c; c = c->next()) {
        if (!holdsPersisted(*c, persisted())) continue;
        if (!all) {
            uint32_t newest = 0;
            newestPersisted(*c, persisted(), savedGen_, gen, newest);
            if (!newest) continue;  // its record is current
        }
        std::string text;
        Reservation held;
        switch (record(*c, persisted(), skip, text, held)) {
            case Record::Empty:
                // Every value at its default: no record, so a new default
                // reaches this device.
                if (!storage_->remove(c->name())) {
                    logError("removing state %s failed", c->name());
                    ok = false;
                }
                break;
            case Record::NoRoom:
                logWarning("no memory to save state %s now; trying again", c->name());
                ok = false;
                break;
            case Record::Written:
                if (!storage_->save(c->name(), text)) {
                    logError("saving state %s failed", c->name());
                    ok = false;
                }
                break;
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
    captureDefaults();  // nodes declared since persistence()
    uint32_t stored = 0;
    std::string data;
    if (storage_->load(kVersionKey, data)) stored = static_cast<uint32_t>(strtoul(data.c_str(), nullptr, 10));
    bool older = stored != persistVersion_;
    bool any = false;
    Applier a;
    for (Node* c = first(); c; c = c->next()) {
        if (!holdsPersisted(*c, persisted())) continue;
        data.clear();
        if (!storage_->load(c->name(), data) || data == "null") continue;
        JsonDocument doc(jsonAllocator(Pool::Large));  // a whole subtree: PSRAM when the chip has it
        a.path = "/";
        a.path += c->name();
        if (deserializeJson(doc, data, DeserializationOption::NestingLimit(kWrittenNesting))) {
            logWarning("stored state %s doesn't parse; keeping defaults", a.path.c_str());
            continue;
        }
        any = true;
        // Stored under another version: brought up to this one first.
        if (older && migrate_ && !migrate_(c->name(), stored, doc.as<JsonVariant>())) continue;
        a.apply(c, doc.as<JsonVariantConst>(), persisted());
    }
    // Rewritten under this version: migrated, and sparse.
    if (older && saveRecords(true)) storage_->save(kVersionKey, std::to_string(persistVersion_));
    savedGen_ = seenGen_ = currentGeneration();
    return any;
}

Status Api::restore(std::string_view json, std::vector<std::string>* skipped) {
    MutexGuard guard(mutex_);
    JsonDocument doc(jsonAllocator(Pool::Large));  // a whole backup: PSRAM when the chip has it
    if (deserializeJson(doc, json.data(), json.size(), DeserializationOption::NestingLimit(kWrittenNesting)) ||
        !doc.is<JsonObjectConst>()) {
        return Status::BadRequest;
    }
    // A backup from an older version goes through the migration first.
    JsonObject root = doc.as<JsonObject>();
    uint32_t version = root[kVersionKey] | 0u;
    root.remove(kVersionKey);
    if (version != persistVersion_ && migrate_) {
        std::vector<std::string> dropped;
        for (JsonPair kv : root) {
            if (!migrate_(kv.key().c_str(), version, kv.value())) dropped.emplace_back(kv.key().c_str());
        }
        for (const std::string& k : dropped) root.remove(k);
    }
    Applier a;
    a.notify = true;
    a.skipped = skipped;
    a.apply(this, doc.as<JsonVariantConst>(), persisted());
    if (storage_) {
        if (!saveRecords(true)) return Status::Internal;
        if (persistVersion_) storage_->save(kVersionKey, std::to_string(persistVersion_));
    }
    return Status::Ok;
}

void Api::checkPersistence(uint32_t nowMs) {
    // A walk over the tree: at most every 50 ms.
    if (nowMs - lastPersistCheckMs_ < 50) return;
    lastPersistCheckMs_ = nowMs;
    uint32_t gen = currentGeneration();
    uint32_t newest = 0;
    newestPersisted(*this, persisted(), savedGen_, gen, newest);
    if (!newest) {
        // Nothing to save while other values keep changing: savedGen_
        // follows the counter, so the window stays narrow and a stamp a
        // whole lap old can't fall into it. A margin behind, in case a
        // change has taken its generation but not stamped it yet.
        if (gen - savedGen_ > kGenerationHorizon) savedGen_ = seenGen_ = gen - (uint32_t(1) << 16);
        return;
    }
    if (newest != seenGen_) {  // a new change: restart the debounce
        seenGen_ = newest;
        changedAtMs_ = nowMs;
        return;
    }
    if (nowMs - changedAtMs_ >= debounceMs_) saveRecords(false);
}

}  // namespace tesser

#include "tesser/node.h"

#include <atomic>
#include <new>

#include "tesser/platform.h"

namespace tesser {

namespace {

std::atomic<int> g_declarationErrors{0};

// Nodes rejected at declaration time. Their reference was already handed out,
// so they can't be deleted; keeping them reachable makes that explicit.
Node* g_orphans = nullptr;

Mutex& orphanMutex() {
    static Mutex m;  // constructed on first use: declarations may run in global constructors
    return m;
}

bool validName(const char* name) {
    if (!name || !*name) return false;
    for (const char* p = name; *p; p++) {
        char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                  c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

const char* kindName(ValueKind k) {
    switch (k) {
        case ValueKind::Boolean: return "boolean";
        case ValueKind::Integer: return "integer";
        case ValueKind::Number: return "number";
        case ValueKind::String: return "string";
    }
    return "string";
}

int declarationErrors() { return g_declarationErrors.load(); }

namespace {
std::atomic<uint32_t> g_generation{0};

uint32_t nextGeneration() {
    uint32_t g = g_generation.fetch_add(1) + 1;
    if (g == 0) g = g_generation.fetch_add(1) + 1;  // 0 means "never changed"
    return g;
}
}  // namespace

uint32_t currentGeneration() { return g_generation.load(); }

void detail::setGeneration(uint32_t g) { g_generation.store(g); }

namespace {
std::atomic<uint32_t> g_schemaRevision{0};
}  // namespace

uint32_t schemaRevision() { return g_schemaRevision.load(); }

void Node::touch() {
    if (!(flags_ & kTemporary)) g_schemaRevision.fetch_add(1);
}

void Node::changed() {
    uint32_t g = nextGeneration();
    if (type_ != NodeType::Object) {
        generation_.store(g, std::memory_order_relaxed);
        return;
    }
    // Every node below an object; recursion depth is bounded by the tree.
    for (Node* n = static_cast<Object*>(this)->first(); n; n = n->next_) {
        if (n->type_ == NodeType::Object) {
            n->changed();
        } else {
            n->generation_.store(g, std::memory_order_relaxed);
        }
    }
}

Node::~Node() { NodeMeta::destroy(meta_); }

// ---- metadata ---------------------------------------------------------

size_t NodeMeta::slotsOf(Field f, size_t hints) {
    switch (f) {
        case kRange: return (2 * sizeof(double) + sizeof(Slot) - 1) / sizeof(Slot);
        case kHints: return 1 + 2 * hints;
        default: return 1;
    }
}

size_t NodeMeta::offset(Field f) const {
    size_t at = 0;
    for (uint8_t bit = 1; bit < f; bit = static_cast<uint8_t>(bit << 1)) {
        if (present_ & bit) at += slotsOf(static_cast<Field>(bit), count_);
    }
    return at;
}

size_t NodeMeta::slots() const { return offset(static_cast<Field>(128)); }

float NodeMeta::step() const {
    if (!(present_ & kStep)) return 0;
    float v;
    memcpy(&v, slot(kStep), sizeof(v));
    return v;
}

double NodeMeta::bound(size_t i) const {
    if (!(present_ & kRange)) return 0;
    double v;
    memcpy(&v, reinterpret_cast<const char*>(slot(kRange)) + i * sizeof(double), sizeof(v));
    return v;
}

UiHint NodeMeta::hint(size_t i) const {
    UiHint h;
    if (i >= count_) return h;
    Slot* at = slot(kHints);
    uint32_t kinds;
    memcpy(&kinds, at, sizeof(kinds));
    h.kind = static_cast<UiHint::Kind>((kinds >> (2 * i)) & 3);
    h.key = static_cast<const char*>(at[1 + 2 * i]);
    if (h.kind == UiHint::Kind::Number) {
        memcpy(&h.num, &at[2 + 2 * i], sizeof(h.num));
    } else {
        h.str = static_cast<const char*>(at[2 + 2 * i]);
    }
    return h;
}

UiHint NodeMeta::hint(const char* key) const {
    for (size_t i = 0; key && i < count_; i++) {
        UiHint h = hint(i);
        if (strcmp(h.key, key) == 0) return h;
    }
    return UiHint();
}

NodeMeta* NodeMeta::grow(NodeMeta* old, Field f) {
    NodeMeta header;
    if (old) header = *old;
    size_t before = old ? old->slots() : 0;
    size_t at = old ? old->offset(f) : 0;  // where the new slots go
    size_t added = 0;
    if (!f) {
        // the header alone
    } else if (f == kHints && (header.present_ & kHints)) {
        at += slotsOf(kHints, header.count_);  // after the last hint
        added = 2;
        header.count_++;
    } else {
        header.present_ |= f;
        added = slotsOf(f, 0);
    }
    auto* m = new (::operator new(sizeof(NodeMeta) + (before + added) * sizeof(Slot))) NodeMeta(header);
    Slot* to = reinterpret_cast<Slot*>(m + 1);
    if (old) {
        const Slot* from = reinterpret_cast<const Slot*>(old + 1);
        memcpy(to, from, at * sizeof(Slot));
        memcpy(to + at + added, from + at, (before - at) * sizeof(Slot));
        ::operator delete(old);  // the Extra, if any, moved
    }
    memset(to + at, 0, added * sizeof(Slot));
    return m;
}

void NodeMeta::destroy(NodeMeta* m) {
    if (!m) return;
    delete m->extra();
    ::operator delete(m);
}

NodeMeta::Slot* Node::metaSlot(NodeMeta::Field f) {
    touch();  // a modifier is changing the node's schema
    if (!meta_ || !(meta_->present_ & f)) meta_ = NodeMeta::grow(meta_, f);
    return meta_->slot(f);
}

NodeMeta::Extra& Node::editExtra() {
    NodeMeta::Slot* s = metaSlot(NodeMeta::kExtra);
    if (!*s) *s = new NodeMeta::Extra();
    return *static_cast<NodeMeta::Extra*>(*s);
}

void Node::setDoc(const char* text) {
#if !defined(TESSER_NO_DESCRIPTIONS)
    *metaSlot(NodeMeta::kDoc) = const_cast<char*>(text);
#else
    (void)text;
#endif
}

void Node::setLabel(const char* text) {
#if !defined(TESSER_NO_UI)
    *metaSlot(NodeMeta::kLabel) = const_cast<char*>(text);
#else
    (void)text;
#endif
}

void Node::setUnit(const char* text) {
#if !defined(TESSER_NO_UI)
    *metaSlot(NodeMeta::kUnit) = const_cast<char*>(text);
#else
    (void)text;
#endif
}

void Node::setStep(double s) {
#if !defined(TESSER_NO_UI)
    float v = static_cast<float>(s);
    memcpy(metaSlot(NodeMeta::kStep), &v, sizeof(v));
#else
    (void)s;
#endif
}

void Node::addHint(const char* key, UiHint::Kind kind, const char* str, float num) {
#if !defined(TESSER_NO_UI)
    if (!key) return;
    // Setting a key again replaces it; otherwise hints keep declaration order.
    size_t i = 0;
    size_t count = meta_ ? meta_->count_ : 0;
    while (i < count && strcmp(meta_->hint(i).key, key) != 0) i++;
    if (i == count) {
        if (count == NodeMeta::kMaxHints) {
            g_declarationErrors++;
            logError("\"%s\" has more than %u ui hints", name_ ? name_ : "(element)",
                     static_cast<unsigned>(NodeMeta::kMaxHints));
            return;
        }
        metaSlot(NodeMeta::kHints);
        if (meta_->count_ == count) meta_ = NodeMeta::grow(meta_, NodeMeta::kHints);
    } else {
        touch();
    }
    NodeMeta::Slot* at = meta_->slot(NodeMeta::kHints);
    uint32_t kinds;
    memcpy(&kinds, at, sizeof(kinds));
    kinds = (kinds & ~(uint32_t(3) << (2 * i))) | (uint32_t(kind) << (2 * i));
    memcpy(at, &kinds, sizeof(kinds));
    at[1 + 2 * i] = const_cast<char*>(key);
    at[2 + 2 * i] = nullptr;
    if (kind == UiHint::Kind::Number) {
        memcpy(&at[2 + 2 * i], &num, sizeof(num));
    } else {
        at[2 + 2 * i] = const_cast<char*>(str);
    }
#else
    (void)key;
    (void)kind;
    (void)str;
    (void)num;
#endif
}

void Node::setAccess(Access read, Access write) {
    touch();
    if (!meta_) meta_ = NodeMeta::grow(nullptr, NodeMeta::Field(0));
    meta_->read_ = read;
    meta_->write_ = write;
}

void Node::setRange(double min, double max) {
    double bounds[2] = {min, max};
    memcpy(metaSlot(NodeMeta::kRange), bounds, sizeof(bounds));
}

ValueNode& ValueNode::range(double min, double max) {
    setRange(min, max);
    return *this;
}

ValueNode& ValueNode::readOnly() {
    setFlag(kReadOnly);
    return *this;
}

ValueNode& ValueNode::persist() {
    setFlag(kPersist);
    return *this;
}

ValueNode& ValueNode::secret() {
    setFlag(kSecret);
    return *this;
}

void ValueNode::setValidator(std::function<Check(JsonVariantConst)> v, const ValueKind* argKind) {
    if (argKind && *argKind != kind()) {
        g_declarationErrors++;
        logError("validator of \"%s\" reads %s, the value is %s", name_ ? name_ : "(element)", kindName(*argKind),
                 kindName(kind()));
        return;
    }
    editExtra().validator = std::move(v);
}

void EnumNode::write(JsonWriter& w) const {
    int64_t v = current();
    if (v >= 0 && static_cast<uint64_t>(v) < names_.size()) {
        w.string(names_[static_cast<size_t>(v)]);
    } else {
        w.integer(v);  // an enumerator without a name
    }
}

Check EnumNode::check(JsonVariantConst v) const {
    if (!v.is<const char*>()) return Check::fail(Status::InvalidValue, "expected string");
    return index(v.as<const char*>()) < names_.size() ? validated(v)
                                                      : Check::fail(Status::InvalidValue, "not one of the options");
}

Check EnumNode::apply(JsonVariantConst v) {
    size_t i = index(v.as<const char*>());
    if (i >= names_.size()) return Check::fail(Status::InvalidValue, "not one of the options");
    return store(i);
}

size_t EnumNode::index(const char* s) const {
    for (size_t i = 0; i < names_.size(); i++) {
        if (s && strcmp(s, names_[i]) == 0) return i;
    }
    return names_.size();
}

ValueNode& ValueNode::watch() {
    setFlag(kWatch);
    editExtra();
    return *this;
}

Check Node::checkRange(double v) const {
    if (meta_ && meta_->hasRange() && (v < meta_->min() || v > meta_->max())) {
        return Check::fail(Status::InvalidValue, "out of range");
    }
    return Check::ok();
}

void ListNode::markReadOnly(Object& element) {
    for (Node* n = element.first(); n; n = n->next()) {
        switch (n->type()) {
            case NodeType::Value: static_cast<ValueNode*>(n)->readOnly(); break;
            case NodeType::Array: static_cast<ArrayNode*>(n)->readOnly(); break;
            case NodeType::Custom: static_cast<CustomNode*>(n)->readOnly(); break;
            case NodeType::List: static_cast<ListNode*>(n)->readOnly(); break;
            case NodeType::Object: markReadOnly(*static_cast<Object*>(n)); break;
            default: break;
        }
    }
}

ListNode& ListNode::key(const char* field) {
    key_ = field;
    touch();
    // The key must be a string value of the element.
    std::unique_ptr<Object> proto = prototype();
    Node* k = field ? proto->child(field) : nullptr;
    if (!k || k->type() != NodeType::Value || static_cast<ValueNode*>(k)->kind() != ValueKind::String) {
        g_declarationErrors++;
        logError("key \"%s\" of list \"%s\" isn't a string value of its elements", field ? field : "(null)",
                 name_ ? name_ : "");
        key_ = nullptr;
    }
    return *this;
}

namespace {
// Element `i`'s key as the JSON its value writes (a quoted string), into
// `json`, cleared first.
void keyJson(const ListNode& list, size_t i, std::string& json) {
    json.clear();
    StringSink sink(json);
    JsonWriter w(sink);
    std::unique_ptr<Object> e = list.element(i);
    Node* k = e->child(list.keyField());
    if (k) static_cast<ValueNode*>(k)->write(w);
}
}  // namespace

void ListNode::keys(std::vector<std::string>& out) const {
    out.clear();
    if (!key_) return;
    std::string json;
    for (size_t i = 0; i < size(); i++) {
        keyJson(*this, i, json);
        if (json.size() < 2 || json.front() != '"') {
            out.emplace_back();
        } else if (json.find('\\') == std::string::npos) {
            out.emplace_back(json, 1, json.size() - 2);  // nothing escaped: the text between the quotes
        } else {
            JsonDocument doc;
            deserializeJson(doc, json);
            out.push_back(doc.is<const char*>() ? doc.as<const char*>() : "");
        }
    }
}

size_t ListNode::find(std::string_view key, size_t skip) const {
    if (!key_) return size();
    // Compared as JSON: the key written the way the elements' are.
    std::string want, json;
    {
        StringSink sink(want);
        JsonWriter w(sink);
        w.string(key);
    }
    for (size_t i = 0; i < size(); i++) {
        if (i == skip) continue;
        keyJson(*this, i, json);
        if (json == want) return i;
    }
    return size();
}

Check ListNode::checkKey(JsonVariantConst v) {
    if (!v.is<const char*>()) return Check::fail(Status::InvalidValue, "key must be a string");
    JsonString s = v.as<JsonString>();
    if (s.size() == 0) return Check::fail(Status::InvalidValue, "key must not be empty");
    if (memchr(s.c_str(), '/', s.size())) return Check::fail(Status::InvalidValue, "key must not contain '/'");
    return Check::ok();
}

void ArrayNode::write(JsonWriter& w) const {
    w.beginArray();
    for (size_t i = 0; i < size(); i++) writeItem(w, i);
    w.endArray();
}

Check ArrayNode::checkLength(JsonVariantConst v) const {
    if (!v.is<JsonArrayConst>()) return Check::fail(Status::InvalidValue, "expected array");
    size_t n = v.as<JsonArrayConst>().size();
    if (fixedSize() && n != size()) return Check::fail(Status::InvalidValue, "wrong number of elements");
    if (n > maxSize()) return Check::fail(Status::InvalidValue, "too many elements");
    return Check::ok();
}

Check ArrayNode::check(JsonVariantConst v) const {
    Check length = checkLength(v);
    if (!length.isOk()) return length;
    for (JsonVariantConst item : v.as<JsonArrayConst>()) {
        Check c = checkElement(item);
        if (!c.isOk()) return c;
    }
    return Check::ok();
}

void ArrayNode::apply(JsonVariantConst v) {
    JsonArrayConst items = v.as<JsonArrayConst>();
    resize(items.size());
    size_t i = 0;
    for (JsonVariantConst item : items) applyItem(i++, item);
}

Object::~Object() {
    Node* n = first_;
    while (n) {
        Node* next = n->next_;
        delete n;
        n = next;
    }
}

Node* Object::child(std::string_view name) const {
    for (Node* n = first_; n; n = n->next_) {
        if (name == n->name_) return n;
    }
    return nullptr;
}

size_t Object::childCount() const {
    size_t count = 0;
    for (Node* n = first_; n; n = n->next_) count++;
    return count;
}

namespace detail {
Mutex& treeWalkMutex() {
    static Mutex m;  // constructed on first use
    return m;
}
}  // namespace detail

void Object::remove(Node* child) {
    if (!unlink(child)) return;
    delete child;
}

bool Object::unlink(Node* child) {
    {
        // A walk without the API lock holds this for its whole length: once
        // the node is unlinked under it, no walk reaches it any more.
        MutexGuard guard(detail::treeWalkMutex());
        Node* prev = nullptr;
        Node* n = first_.load(std::memory_order_acquire);
        while (n && n != child) {
            prev = n;
            n = n->next_.load(std::memory_order_acquire);
        }
        if (!n) return false;
        Node* next = n->next_.load(std::memory_order_acquire);
        if (prev) {
            prev->next_.store(next, std::memory_order_release);
        } else {
            first_.store(next, std::memory_order_release);
        }
        if (last_ == n) last_ = prev;
    }
    touch();  // the schema changed
    return true;
}

void Object::link(Node* node) {
    const char* problem = nullptr;
    if (!validName(node->name_)) {
        problem = "invalid name";
    } else if (child(node->name_)) {
        problem = "duplicate name";
    }
    if (problem) {
        g_declarationErrors++;
        logError("%s \"%s\" in object \"%s\": node ignored", problem, node->name_ ? node->name_ : "(null)",
                 name_ ? name_ : "");
        MutexGuard guard(orphanMutex());
        node->next_ = g_orphans;
        g_orphans = node;
        return;
    }
    // Children of a temporary object are temporary too.
    node->flags_ |= kLinked | (flags_ & kTemporary);
    node->touch();
    // Published complete: readers that walk the tree without the lock see
    // the node whole or not at all.
    if (last_) {
        last_->next_.store(node, std::memory_order_release);
    } else {
        first_.store(node, std::memory_order_release);
    }
    last_ = node;
}

Object& Object::object(const char* name) {
    if (name) {
        Node* existing = child(name);
        if (existing && existing->type() == NodeType::Object) return *static_cast<Object*>(existing);
    }
    return add(new Object(name));
}

}  // namespace tesser

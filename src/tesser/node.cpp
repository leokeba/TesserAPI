#include "tesser/node.h"

#include <atomic>

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
std::atomic<uint16_t> g_generation{0};

uint16_t nextGeneration() {
    uint16_t g = static_cast<uint16_t>(g_generation.fetch_add(1) + 1);
    if (g == 0) g = static_cast<uint16_t>(g_generation.fetch_add(1) + 1);  // 0 means "never changed"
    return g;
}
}  // namespace

uint16_t currentGeneration() { return g_generation.load(); }

void Node::changed() {
    uint16_t g = nextGeneration();
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

Node::~Node() { delete meta_; }

NodeMeta& Node::editMeta() {
    if (!meta_) meta_ = new NodeMeta();
    return *meta_;
}

void Node::setDoc(const char* text) {
#if !defined(TESSER_NO_DESCRIPTIONS)
    editMeta().doc = text;
#else
    (void)text;
#endif
}

ValueNode& ValueNode::range(double min, double max) {
    NodeMeta& m = editMeta();
    m.min = min;
    m.max = max;
    m.hasRange = true;
    return *this;
}

ValueNode& ValueNode::readOnly() {
    flags_ |= kReadOnly;
    return *this;
}

ValueNode& ValueNode::persist() {
    flags_ |= kPersist;
    return *this;
}

ValueNode& ValueNode::watch() {
    flags_ |= kWatch;
    editMeta();
    return *this;
}

ValueNode& ValueNode::doc(const char* text) {
    setDoc(text);
    return *this;
}

Check ValueNode::checkRange(double v) const {
    if (meta_ && meta_->hasRange && (v < meta_->min || v > meta_->max)) {
        return Check::fail(Status::InvalidValue, "out of range");
    }
    return Check::ok();
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
    node->flags_ |= kLinked;
    if (last_) {
        last_->next_ = node;
    } else {
        first_ = node;
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

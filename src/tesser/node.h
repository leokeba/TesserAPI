#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <array>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <ArduinoJson.h>

#include "tesser/call.h"
#include "tesser/json_writer.h"
#include "tesser/request.h"
#include "tesser/status.h"
#include "tesser/value_types.h"

namespace tesser {

class Api;
class DatagramEndpoint;
class Object;
class RemoteNode;
struct PeerAddress;

enum class NodeType : uint8_t { Object, Value, Action, Event, Custom, List, Remote, Array };

// A presentation hint (docs/DESIGN.md section 7.1): a key and a string,
// number or `true`, emitted in the schema under "ui" and never interpreted.
struct UiHint {
    enum class Kind : uint8_t { Flag, String, Number };
    const char* key;
    union {
        const char* str;
        float num;
    };
    Kind kind;
    UiHint* next = nullptr;
};

// Presentation metadata, allocated only when a node has some.
struct UiMeta {
    const char* label = nullptr;
    const char* unit = nullptr;
    float step = 0;  // 0: none
    UiHint* hints = nullptr;
    ~UiMeta();
};

// Optional per-node metadata, allocated only when a modifier needs it.
struct NodeMeta {
    double min = 0;
    double max = 0;
    bool hasRange = false;
    bool hashed = false;  // watch(): `hash` holds the last sample
    Access readAccess = Access::Public;
    Access writeAccess = Access::Public;
    const char* doc = nullptr;
    uint32_t hash = 0;
    UiMeta* ui = nullptr;
    ~NodeMeta() { delete ui; }
};

// Change tracking: a global generation counter, stamped on nodes as they
// change. 16 bits, compared with wrap-around; 0 means "never changed".
uint16_t currentGeneration();

// True if generation `a` is newer than `b`.
inline bool newerGeneration(uint16_t a, uint16_t b) { return a != 0 && static_cast<int16_t>(a - b) > 0; }

// Bumped whenever the shape of a tree changes: a node is added, a modifier
// changes a node's schema, a remote node's state changes. Temporary objects
// (list elements) don't count. Lets transports re-advertise a changed schema
// (docs/DESIGN.md section 10.3) without rendering it to find out.
uint32_t schemaRevision();

// Number of declaration mistakes (duplicate or invalid names) since boot.
// Each one is also logged.
int declarationErrors();

class Node {
public:
    // A node without a name is temporary: part of a list element or
    // prototype, or an array element, built for one request.
    Node(const char* name, NodeType type) : name_(name), type_(type) {
        if (!name) flags_ |= kTemporary;
    }
    virtual ~Node();
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    const char* name() const { return name_; }
    NodeType type() const { return type_; }
    Node* next() const { return next_; }
    const NodeMeta* meta() const { return meta_; }
    bool persisted() const { return flags_ & kPersist; }
    bool watched() const { return flags_ & kWatch; }
    // Levels this node itself requires (docs/DESIGN.md section 13); a node
    // also requires its ancestors'.
    Access readAccess() const { return meta_ ? meta_->readAccess : Access::Public; }
    Access writeAccess() const { return meta_ ? meta_->writeAccess : Access::Public; }

    // Marks this node (for an object: every value below it) as changed, so
    // subscribers get it on the next Api::poll(). Writes through the API do
    // this automatically; call it when the application changes bound state.
    // Thread-safe and cheap.
    void changed();
    uint16_t generation() const { return generation_.load(std::memory_order_relaxed); }

protected:
    friend class Object;
    friend class Api;

    enum Flags : uint8_t {
        kReadOnly = 1,
        kPersist = 2,
        kDeferring = 4,
        kLinked = 8,
        kWatch = 16,
        kSecret = 32,
        kTemporary = 64,  // part of a temporary object (list element, prototype)
    };

    // Records a schema change (see schemaRevision()).
    void touch();
    void setFlag(uint8_t flag) {
        flags_ |= flag;
        touch();
    }

    NodeMeta& editMeta();
    UiMeta* editUi();  // null when compiled out (TESSER_NO_UI)
    void setDoc(const char* text);
    void setRange(double min, double max);
    void addHint(const char* key, UiHint::Kind kind, const char* str, float num);
    void setAccess(Access read, Access write);
    // Range check for values and numeric action arguments.
    Check checkRange(double v) const;

    const char* name_;
    Node* next_ = nullptr;
    NodeMeta* meta_ = nullptr;
    NodeType type_;
    uint8_t flags_ = 0;
    std::atomic<uint16_t> generation_{0};
};

// The modifiers every node type has, returning the node's own type so they
// chain with its specific ones. Description, label, unit, step and hints are
// schema metadata only (docs/DESIGN.md section 7.1).
template <class Self>
class Annotated : public Node {
public:
    using Node::Node;
    using Node::readAccess;
    using Node::writeAccess;

    // Schema description. Compiled out with TESSER_NO_DESCRIPTIONS.
    Self& doc(const char* text) {
        setDoc(text);
        return self();
    }
    // Human-readable name, for user interfaces. The rest of this group is
    // compiled out with TESSER_NO_UI.
    Self& label(const char* text) {
        if (UiMeta* m = editUi()) m->label = text;
        return self();
    }
    // Unit of a value or of an action's argument: "%", "°C", "ms".
    Self& unit(const char* text) {
        if (UiMeta* m = editUi()) m->unit = text;
        return self();
    }
    // Input granularity. A hint: writes are not rounded or checked against it.
    Self& step(double s) {
        if (UiMeta* m = editUi()) m->step = static_cast<float>(s);
        return self();
    }
    // Presentation hints for renderers: ui("advanced") is a flag,
    // ui("widget", "knob") a string, ui("precision", 1) a number.
    Self& ui(const char* key) {
        addHint(key, UiHint::Kind::Flag, nullptr, 0);
        return self();
    }
    Self& ui(const char* key, const char* value) {
        addHint(key, UiHint::Kind::String, value, 0);
        return self();
    }
    template <class T, std::enable_if_t<std::is_arithmetic<T>::value && !std::is_same<T, bool>::value, int> = 0>
    Self& ui(const char* key, T value) {
        addHint(key, UiHint::Kind::Number, nullptr, static_cast<float>(value));
        return self();
    }
    // The level a client needs to read this node and its subtree. Writing
    // needs at least as much.
    Self& readAccess(Access a) {
        setAccess(a, writeAccess() > a ? writeAccess() : a);
        return self();
    }
    // The level a client needs to write this node and its subtree, or to
    // call actions in it.
    Self& writeAccess(Access a) {
        setAccess(readAccess(), a > readAccess() ? a : readAccess());
        return self();
    }

private:
    Self& self() { return static_cast<Self&>(*this); }
};

// A typed scalar. Concrete classes bind it to a variable or to functions.
class ValueNode : public Annotated<ValueNode> {
public:
    explicit ValueNode(const char* name) : Annotated(name, NodeType::Value) {}

    // Numeric bounds, checked on write and reported in the schema.
    ValueNode& range(double min, double max);
    ValueNode& readOnly();
    ValueNode& persist();
    // Samples the value periodically (Config::watchIntervalMs, while anyone
    // is subscribed) and marks it changed when it differs. For bound state
    // the application doesn't report with changed().
    ValueNode& watch();
    // A password or key: writable and persisted like any value, but read as
    // null by clients (docs/DESIGN.md section 4.1).
    ValueNode& secret();

    bool writable() const { return canWrite() && !(flags_ & kReadOnly); }
    bool isSecret() const { return flags_ & kSecret; }

    virtual ValueKind kind() const = 0;
    virtual size_t maxLength() const { return 0; }
    // Allowed string values (enums), or null.
    virtual const std::vector<const char*>* options() const { return nullptr; }
    virtual void write(JsonWriter& w) const = 0;
    // Type and range check. Doesn't consider writability.
    virtual Check check(JsonVariantConst v) const = 0;
    // Applies a value that passed check().
    virtual Check apply(JsonVariantConst v) = 0;

protected:
    virtual bool canWrite() const = 0;
};

namespace detail {

template <class T>
using Bare = std::remove_cv_t<std::remove_reference_t<T>>;

template <class T>
constexpr void assertSupported() {
    static_assert(ValueTraits<T>::supported,
                  "unsupported value type: use bool, integers, float, double, std::string, char[N], "
                  "const char* or String");
}

}  // namespace detail

// Bound to a variable. T may be const, which makes the value read-only.
template <class T>
class RefValue : public ValueNode {
    using U = std::remove_const_t<T>;
    using Traits = ValueTraits<U>;

public:
    RefValue(const char* name, T& ref) : ValueNode(name), ref_(ref) { detail::assertSupported<U>(); }
    ValueKind kind() const override { return Traits::kind; }
    size_t maxLength() const override { return Traits::maxLength; }
    void write(JsonWriter& w) const override { Traits::write(w, ref_); }
    Check check(JsonVariantConst v) const override {
        Check c = Traits::check(v);
        if (!c.isOk()) return c;
        if constexpr (isNumericKind<U>()) {
            U tmp{};
            Traits::read(v, tmp);
            return checkRange(Traits::toDouble(tmp));
        }
        return c;
    }
    Check apply(JsonVariantConst v) override {
        if constexpr (std::is_const<T>::value) {
            (void)v;
            return Check::fail(Status::ReadOnly, "read-only");
        } else {
            Traits::read(v, ref_);
            return Check::ok();
        }
    }

protected:
    bool canWrite() const override { return !std::is_const<T>::value; }

private:
    T& ref_;
};

// Constant string (string literal).
class ConstStringValue : public ValueNode {
public:
    ConstStringValue(const char* name, const char* value) : ValueNode(name), value_(value) {}
    ValueKind kind() const override { return ValueKind::String; }
    void write(JsonWriter& w) const override { ValueTraits<const char*>::write(w, value_); }
    Check check(JsonVariantConst v) const override { return ValueTraits<const char*>::check(v); }
    Check apply(JsonVariantConst) override { return Check::fail(Status::ReadOnly, "read-only"); }

protected:
    bool canWrite() const override { return false; }

private:
    const char* value_;
};

// A C++ enum exposed as strings: names[i] is the name of the enumerator
// whose underlying value is i.
template <class E>
class EnumValue : public ValueNode {
public:
    EnumValue(const char* name, E& ref, std::vector<const char*> names)
        : ValueNode(name), ref_(ref), names_(std::move(names)) {}
    ValueKind kind() const override { return ValueKind::String; }
    const std::vector<const char*>* options() const override { return &names_; }
    void write(JsonWriter& w) const override {
        size_t i = static_cast<size_t>(ref_);
        if (i < names_.size()) {
            w.string(names_[i]);
        } else {
            w.integer(static_cast<int64_t>(ref_));  // an enumerator without a name
        }
    }
    Check check(JsonVariantConst v) const override {
        if (!v.is<const char*>()) return Check::fail(Status::InvalidValue, "expected string");
        return index(v.as<const char*>()) < names_.size() ? Check::ok()
                                                          : Check::fail(Status::InvalidValue, "not one of the options");
    }
    Check apply(JsonVariantConst v) override {
        size_t i = index(v.as<const char*>());
        if (i >= names_.size()) return Check::fail(Status::InvalidValue, "not one of the options");
        ref_ = static_cast<E>(i);
        return Check::ok();
    }

protected:
    bool canWrite() const override { return true; }

private:
    size_t index(const char* s) const {
        for (size_t i = 0; i < names_.size(); i++) {
            if (s && strcmp(s, names_[i]) == 0) return i;
        }
        return names_.size();
    }
    E& ref_;
    std::vector<const char*> names_;
};

// Getter, and optionally a setter. R is the getter's type, P the setter's
// parameter type (void for read-only).
template <class R, class P>
class FnValue : public ValueNode {
public:
    using Getter = std::function<R()>;
    using Setter = std::function<Check(const P&)>;

    FnValue(const char* name, Getter get, Setter set) : ValueNode(name), get_(std::move(get)), set_(std::move(set)) {
        detail::assertSupported<R>();
        detail::assertSupported<P>();
    }
    ValueKind kind() const override { return ValueTraits<P>::kind; }
    size_t maxLength() const override { return ValueTraits<P>::maxLength; }
    void write(JsonWriter& w) const override { ValueTraits<R>::write(w, get_()); }
    Check check(JsonVariantConst v) const override {
        Check c = ValueTraits<P>::check(v);
        if (!c.isOk()) return c;
        if constexpr (isNumericKind<P>()) {
            P tmp{};
            ValueTraits<P>::read(v, tmp);
            return checkRange(ValueTraits<P>::toDouble(tmp));
        }
        return c;
    }
    Check apply(JsonVariantConst v) override {
        P tmp{};
        ValueTraits<P>::read(v, tmp);
        return set_(tmp);
    }

protected:
    bool canWrite() const override { return true; }

private:
    Getter get_;
    Setter set_;
};

template <class R>
class FnValue<R, void> : public ValueNode {
public:
    using Getter = std::function<R()>;

    FnValue(const char* name, Getter get) : ValueNode(name), get_(std::move(get)) { detail::assertSupported<R>(); }
    ValueKind kind() const override { return ValueTraits<R>::kind; }
    size_t maxLength() const override { return ValueTraits<R>::maxLength; }
    void write(JsonWriter& w) const override { ValueTraits<R>::write(w, get_()); }
    Check check(JsonVariantConst v) const override { return ValueTraits<R>::check(v); }
    Check apply(JsonVariantConst) override { return Check::fail(Status::ReadOnly, "read-only"); }

protected:
    bool canWrite() const override { return false; }

private:
    Getter get_;
};

// Something that happens: emit() pushes it to subscribers right away, never
// coalesced. Events have no value; they appear only in the schema.
class EventNode : public Annotated<EventNode> {
public:
    explicit EventNode(const char* name) : Annotated(name, NodeType::Event) {}

    void emit();  // null payload
    void emit(JsonVariantConst payload);
    template <class T>
    void emit(const T& payload) {
        emitWith([&payload](JsonWriter& w) { ValueTraits<std::remove_cv_t<T>>::write(w, payload); });
    }
    // Arbitrary JSON payload. Thread-safe; takes the API lock.
    void emitWith(const std::function<void(JsonWriter&)>& write);

private:
    Api* api_ = nullptr;  // found on the first emit()
    std::string path_;
};

class ActionNode : public Annotated<ActionNode> {
public:
    // Converts the argument and runs the action; the result goes through
    // call.reply() / call.fail() / call.defer(). `params` is the object
    // argument, filled from `arg`, for actions that take one (else null).
    using Invoker = std::function<void(JsonVariantConst arg, Object* params, Call& call)>;
    using ArgCheck = Check (*)(JsonVariantConst);
    // A temporary object describing a default object argument.
    using ParamsFactory = std::unique_ptr<Object> (*)();

    ActionNode(const char* name, Invoker invoker, ArgCheck argCheck, const char* argKindName,
               const char* returnKindName, bool defers, ParamsFactory makeParams = nullptr)
        : Annotated(name, NodeType::Action),
          invoke_(std::move(invoker)),
          check_(argCheck),
          params_(makeParams),
          argKind_(argKindName),
          returnKind_(returnKindName) {
        if (defers) flags_ |= kDeferring;
    }

    // Bounds for a numeric argument, checked before the action runs and
    // reported in the schema.
    ActionNode& range(double min, double max) {
        setRange(min, max);
        return *this;
    }

    Check check(JsonVariantConst arg) const {
        Check c = check_ ? check_(arg) : Check::ok();
        if (c.isOk() && arg.is<double>()) c = checkRange(arg.as<double>());
        return c;
    }
    void invoke(JsonVariantConst arg, Object* params, Call& call) { invoke_(arg, params, call); }
    bool deferring() const { return flags_ & kDeferring; }
    const char* argKind() const { return argKind_; }        // null: no argument; "object": see params()
    const char* returnKind() const { return returnKind_; }  // null: no result
    // For an object argument: a temporary object describing its default,
    // which the request handler validates and fills like a patch. Else null.
    std::unique_ptr<Object> params() const;
    bool takesObject() const { return params_ != nullptr; }

private:
    Invoker invoke_;
    ArgCheck check_;
    ParamsFactory params_;
    const char* argKind_;
    const char* returnKind_;
};

// Escape hatch: user code writes the JSON and applies incoming values.
class CustomNode : public Annotated<CustomNode> {
public:
    using Writer = std::function<void(JsonWriter&)>;
    using Applier = std::function<Status(JsonVariantConst)>;
    using Validator = std::function<Check(JsonVariantConst)>;

    CustomNode(const char* name, Writer writer, Applier applier)
        : Annotated(name, NodeType::Custom), writer_(std::move(writer)), applier_(std::move(applier)) {}

    CustomNode& validate(Validator v) {
        validator_ = std::move(v);
        return *this;
    }
    CustomNode& persist() {
        setFlag(kPersist);
        return *this;
    }

    bool writable() const { return static_cast<bool>(applier_); }
    void write(JsonWriter& w) const {
        if (writer_) {
            writer_(w);
        } else {
            w.null();
        }
    }
    Check check(JsonVariantConst v) const { return validator_ ? validator_(v) : Check::ok(); }
    Status apply(JsonVariantConst v) { return applier_ ? applier_(v) : Status::ReadOnly; }

private:
    Writer writer_;
    Applier applier_;
    Validator validator_;
};

// A std::vector of described objects (docs/DESIGN.md section 4.4). Elements
// are described on demand into temporary objects, so a list costs one node
// however long it is.
class ListNode : public Annotated<ListNode> {
public:
    explicit ListNode(const char* name) : Annotated(name, NodeType::List) {}

    virtual size_t size() const = 0;
    // A temporary object describing element `i` (< size()).
    virtual std::unique_ptr<Object> element(size_t i) const = 0;
    // A temporary object describing a default element (schema, validation).
    virtual std::unique_ptr<Object> prototype() const = 0;
    virtual void resize(size_t n) = 0;
    // Rebuilds the list: element i becomes the old element from[i], or a
    // default one where from[i] < 0.
    virtual void reorder(const std::vector<int>& from) = 0;

    // Makes this a keyed list: elements are named by their `field`, a string
    // value of the element, and addressed as /list/<key> instead of by
    // index. Replacing the list matches elements by key (docs/DESIGN.md
    // section 4.4).
    ListNode& key(const char* field);
    const char* keyField() const { return key_; }
    // Keys of the elements, in order (keyed lists).
    void keys(std::vector<std::string>& out) const;
    // A valid key: a non-empty string without '/'.
    static Check checkKey(JsonVariantConst v);

    // Longest list a write may create (default 32).
    ListNode& maxSize(size_t n) {
        maxSize_ = n;
        touch();
        return *this;
    }
    size_t maxSize() const { return maxSize_; }
    ListNode& persist() {
        setFlag(kPersist);
        return *this;
    }

private:
    size_t maxSize_ = 32;
    const char* key_ = nullptr;
};

// An array of scalars bound to a std::vector (resizable, up to maxSize()),
// a C array or a std::array (fixed size) (docs/DESIGN.md section 4.5). It is
// one value: written whole, or element by element as /name/<index>.
class ArrayNode : public Annotated<ArrayNode> {
public:
    explicit ArrayNode(const char* name) : Annotated(name, NodeType::Array) {}

    // Bounds for every element, checked on write and reported in the schema.
    ArrayNode& range(double min, double max) {
        setRange(min, max);
        return *this;
    }
    ArrayNode& readOnly() {
        setFlag(kReadOnly);
        return *this;
    }
    ArrayNode& persist() {
        setFlag(kPersist);
        return *this;
    }
    // Longest array a write may create (resizable arrays; default 64).
    ArrayNode& maxSize(size_t n) {
        maxSize_ = n;
        touch();
        return *this;
    }

    virtual size_t size() const = 0;
    virtual bool fixedSize() const = 0;
    virtual ValueKind itemKind() const = 0;
    size_t maxSize() const { return fixedSize() ? size() : maxSize_; }
    bool writable() const { return canWrite() && !(flags_ & kReadOnly); }
    // Element `i` (< size()) as a temporary value node, with the array's
    // range and writability.
    virtual std::unique_ptr<ValueNode> element(size_t i) = 0;

    void write(JsonWriter& w) const;
    // An array of valid elements, of the right length. Doesn't consider
    // writability.
    Check check(JsonVariantConst v) const;
    // An array of the right length (elements unchecked).
    Check checkLength(JsonVariantConst v) const;
    // One element: its type and the range.
    Check checkElement(JsonVariantConst v) const {
        Check c = checkItem(v);
        return c.isOk() && v.is<double>() ? checkRange(v.as<double>()) : c;
    }
    // Replaces the contents with an array that passed check().
    void apply(JsonVariantConst v);

protected:
    virtual void writeItem(JsonWriter& w, size_t i) const = 0;
    virtual Check checkItem(JsonVariantConst v) const = 0;
    virtual void applyItem(size_t i, JsonVariantConst v) = 0;
    virtual void resize(size_t n) = 0;
    virtual bool canWrite() const = 0;

private:
    size_t maxSize_ = 64;
};

namespace detail {

// How an array binding reaches its elements.
template <class C>
struct ArrayAccess;
template <class T>
struct ArrayAccess<std::vector<T>> {
    using Item = T;
    static constexpr bool fixed = false;
    static size_t size(const std::vector<T>& c) { return c.size(); }
    static void resize(std::vector<T>& c, size_t n) { c.resize(n); }
};
template <class T, size_t N>
struct ArrayAccess<T[N]> {
    using Item = T;
    static constexpr bool fixed = true;
    static size_t size(const T (&)[N]) { return N; }
    static void resize(T (&)[N], size_t) {}
};
template <class T, size_t N>
struct ArrayAccess<std::array<T, N>> {
    using Item = T;
    static constexpr bool fixed = true;
    static size_t size(const std::array<T, N>&) { return N; }
    static void resize(std::array<T, N>&, size_t) {}
};

template <class C>
class BoundArray final : public ArrayNode {
    using Bare = std::remove_const_t<C>;
    using Access = ArrayAccess<Bare>;
    using T = typename Access::Item;

public:
    BoundArray(const char* name, C& items) : ArrayNode(name), items_(items) {
        static_assert(ValueTraits<T>::supported, "array elements must be a value type");
        static_assert(!std::is_same<Bare, std::vector<bool>>::value,
                      "std::vector<bool> has no element references: use std::vector<uint8_t>");
    }
    size_t size() const override { return Access::size(items_); }
    bool fixedSize() const override { return Access::fixed; }
    ValueKind itemKind() const override { return ValueTraits<T>::kind; }
    std::unique_ptr<ValueNode> element(size_t i) override {
        std::unique_ptr<ValueNode> e(new RefValue<std::conditional_t<std::is_const<C>::value, const T, T>>(nullptr, items_[i]));
        if (const NodeMeta* m = meta(); m && m->hasRange) e->range(m->min, m->max);
        if (!writable()) e->readOnly();
        return e;
    }

protected:
    void writeItem(JsonWriter& w, size_t i) const override { ValueTraits<T>::write(w, items_[i]); }
    Check checkItem(JsonVariantConst v) const override { return ValueTraits<T>::check(v); }
    void applyItem(size_t i, JsonVariantConst v) override {
        if constexpr (!std::is_const<C>::value) ValueTraits<T>::read(v, items_[i]);
    }
    void resize(size_t n) override {
        if constexpr (!std::is_const<C>::value) Access::resize(items_, n);
    }
    bool canWrite() const override { return !std::is_const<C>::value; }

private:
    C& items_;
};

}  // namespace detail

namespace detail {

// Signature of a callable: return type and (at most one) argument type.
template <class F>
struct Signature : Signature<decltype(&F::operator())> {};
template <class R, class... A>
struct Signature<R (*)(A...)> {
    using Return = R;
    static constexpr size_t arity = sizeof...(A);
    using Arg = std::tuple_element_t<0, std::tuple<A..., void>>;
    using Arg2 = std::tuple_element_t<1, std::tuple<A..., void, void>>;
};
template <class R, class... A>
struct Signature<R(A...)> : Signature<R (*)(A...)> {};
template <class C, class R, class... A>
struct Signature<R (C::*)(A...)> : Signature<R (*)(A...)> {};
template <class C, class R, class... A>
struct Signature<R (C::*)(A...) const> : Signature<R (*)(A...)> {};

template <class F, class = void>
struct IsCallable : std::false_type {};
template <class F>
struct IsCallable<F, std::void_t<decltype(&F::operator())>> : std::true_type {};
template <class R, class... A>
struct IsCallable<R (*)(A...), void> : std::true_type {};
template <class R, class... A>
struct IsCallable<R(A...), void> : std::true_type {};

template <class S>
Check toCheck(S&& setterResult) {
    using T = Bare<S>;
    if constexpr (std::is_same<T, bool>::value) {
        return setterResult ? Check::ok() : Check::fail(Status::InvalidValue, "rejected");
    } else if constexpr (std::is_same<T, Status>::value) {
        return setterResult == Status::Ok ? Check::ok() : Check::fail(setterResult, "rejected");
    } else if constexpr (std::is_same<T, Check>::value) {
        return setterResult;
    } else {
        static_assert(sizeof(T) == 0, "setter must return void, bool, tesser::Status or tesser::Check");
    }
}

template <class T>
Check checkArg(JsonVariantConst v) {
    return ValueTraits<T>::check(v);
}

inline Check checkNoArg(JsonVariantConst) { return Check::ok(); }

// An object argument: its fields are checked by the request handler.
inline Check checkObjectArg(JsonVariantConst v) {
    return v.isNull() || v.is<JsonObjectConst>() ? Check::ok() : Check::fail(Status::InvalidValue, "expected object");
}
using ArgCheck_t = Check (*)(JsonVariantConst);

// Converts an action's return value into the call's reply.
template <class F, class... A>
void runAndReply(F& f, Call& call, A&&... args) {
    using R = decltype(f(std::forward<A>(args)...));
    if constexpr (std::is_void<R>::value) {
        f(std::forward<A>(args)...);
    } else if constexpr (std::is_same<Bare<R>, Status>::value) {
        Status s = f(std::forward<A>(args)...);
        if (s != Status::Ok) call.fail(s);
    } else {
        assertSupported<Bare<R>>();
        call.reply(f(std::forward<A>(args)...));
    }
}

template <class R>
const char* returnKindName() {
    if constexpr (std::is_void<R>::value || std::is_same<Bare<R>, Status>::value) {
        return nullptr;
    } else {
        return kindName(ValueTraits<Bare<R>>::kind);
    }
}

template <class T, class = void>
struct HasDescribeMember : std::false_type {};
template <class T>
struct HasDescribeMember<T, std::void_t<decltype(std::declval<T&>().describe(std::declval<Object&>()))>>
    : std::true_type {};
// describe(Object&, T&), found by argument-dependent lookup.
template <class T, class = void>
struct HasDescribeFree : std::false_type {};
template <class T>
struct HasDescribeFree<T, std::void_t<decltype(describe(std::declval<Object&>(), std::declval<T&>()))>>
    : std::true_type {};
// A type that describes itself: usable as an object argument.
template <class T>
constexpr bool describable() {
    return HasDescribeMember<T>::value || HasDescribeFree<T>::value;
}

template <class T>
void describeInto(Object& o, T& thing);
// A temporary object owning a default T it describes (defined below Object).
template <class T>
std::unique_ptr<Object> makeParams();
template <class T>
T& paramsValue(Object& params);

// The argument of a typed action, converted: a scalar from the JSON, an
// object argument from its filled params.
template <class B>
struct ArgReader {
    static B read(JsonVariantConst arg, Object* params) {
        if constexpr (describable<B>() && !ValueTraits<B>::supported) {
            (void)arg;
            return params ? paramsValue<B>(*params) : B{};
        } else {
            (void)params;
            B value{};
            ValueTraits<B>::read(arg, value);
            return value;
        }
    }
    static constexpr ArgCheck_t check() {
        if constexpr (describable<B>() && !ValueTraits<B>::supported) {
            return &checkObjectArg;
        } else {
            return &checkArg<B>;
        }
    }
    static const char* kind() {
        if constexpr (describable<B>() && !ValueTraits<B>::supported) {
            return "object";
        } else {
            return kindName(ValueTraits<B>::kind);
        }
    }
    static ActionNode::ParamsFactory factory() {
        if constexpr (describable<B>() && !ValueTraits<B>::supported) {
            return &makeParams<B>;
        } else {
            return nullptr;
        }
    }
};

}  // namespace detail

class Object : public Annotated<Object> {
public:
    explicit Object(const char* name) : Annotated(name, NodeType::Object) {}
    ~Object() override;

    // Child object; returns the existing one when the name is already an object.
    Object& object(const char* name);

    // A variable. Writable unless const or marked readOnly().
    template <class T, std::enable_if_t<!detail::IsCallable<std::decay_t<T>>::value, int> = 0>
    ValueNode& value(const char* name, T& var) {
        using V = std::remove_reference_t<T>;
        return add(new RefValue<V>(name, var));
    }

    // A constant string.
    ValueNode& value(const char* name, const char* constant) { return add(new ConstStringValue(name, constant)); }

    // An enum, as strings: `names` lists the enumerators in order of their
    // underlying values (0, 1, 2, ...). The names must outlive the API.
    template <class E, std::enable_if_t<std::is_enum<E>::value, int> = 0>
    ValueNode& value(const char* name, E& var, std::initializer_list<const char*> names) {
        return add(new EnumValue<E>(name, var, std::vector<const char*>(names)));
    }

    // A getter: read-only.
    template <class G, std::enable_if_t<detail::IsCallable<std::decay_t<G>>::value, int> = 0>
    ValueNode& value(const char* name, G getter) {
        using R = detail::Bare<decltype(getter())>;
        return add(new FnValue<R, void>(name, std::move(getter)));
    }

    // A getter and a setter. The setter may return void, bool, Status or Check.
    template <class G, class S>
    ValueNode& value(const char* name, G getter, S setter) {
        using R = detail::Bare<decltype(getter())>;
        using P = detail::Bare<typename detail::Signature<std::decay_t<S>>::Arg>;
        static_assert(detail::Signature<std::decay_t<S>>::arity == 1, "setter must take exactly one argument");
        auto wrapped = [setter = std::move(setter)](const P& v) mutable -> Check {
            using SR = decltype(setter(v));
            if constexpr (std::is_void<SR>::value) {
                setter(v);
                return Check::ok();
            } else {
                return detail::toCheck(setter(v));
            }
        };
        return add(new FnValue<R, P>(name, std::move(getter), std::move(wrapped)));
    }

    // An action taking nothing, one typed value, an object argument (a type
    // with describe(), as for mount()), a JsonVariantConst, or a
    // tesser::Call& (for deferred replies). It may return void, a value or a
    // Status. A typed or object argument followed by a tesser::Call& makes a
    // typed deferred action.
    template <class F>
    ActionNode& action(const char* name, F fn) {
        using Sig = detail::Signature<std::decay_t<F>>;
        using R = typename Sig::Return;
        static_assert(Sig::arity <= 2, "actions take at most one argument, then optionally a tesser::Call&");
        if constexpr (Sig::arity == 0) {
            return add(new ActionNode(
                name,
                [fn = std::move(fn)](JsonVariantConst, Object*, Call& call) mutable { detail::runAndReply(fn, call); },
                &detail::checkNoArg, nullptr, detail::returnKindName<R>(), false));
        } else if constexpr (Sig::arity == 2) {
            using B = detail::Bare<typename Sig::Arg>;
            static_assert(std::is_same<detail::Bare<typename Sig::Arg2>, Call>::value,
                          "a two-argument action is (argument, tesser::Call&)");
            static_assert(ValueTraits<B>::supported || detail::describable<B>(),
                          "the argument of a deferred action must be a value type or have describe()");
            using Reader = detail::ArgReader<B>;
            return add(new ActionNode(
                name,
                [fn = std::move(fn)](JsonVariantConst arg, Object* params, Call& call) mutable {
                    B value = Reader::read(arg, params);
                    fn(value, call);
                },
                Reader::check(), Reader::kind(), nullptr, true, Reader::factory()));
        } else {
            using A = typename Sig::Arg;
            using B = detail::Bare<A>;
            if constexpr (std::is_same<B, Call>::value) {
                return add(new ActionNode(
                    name,
                    [fn = std::move(fn)](JsonVariantConst, Object*, Call& call) mutable {
                        detail::runAndReply(fn, call, call);
                    },
                    &detail::checkNoArg, "any", nullptr, true));
            } else if constexpr (std::is_same<B, JsonVariantConst>::value || std::is_same<B, JsonVariant>::value) {
                return add(new ActionNode(
                    name,
                    [fn = std::move(fn)](JsonVariantConst arg, Object*, Call& call) mutable {
                        detail::runAndReply(fn, call, arg);
                    },
                    &detail::checkNoArg, "any", detail::returnKindName<R>(), false));
            } else {
                static_assert(ValueTraits<B>::supported || detail::describable<B>(),
                              "unsupported argument type: use a value type, a type with describe(), "
                              "JsonVariantConst or tesser::Call&");
                using Reader = detail::ArgReader<B>;
                return add(new ActionNode(
                    name,
                    [fn = std::move(fn)](JsonVariantConst arg, Object* params, Call& call) mutable {
                        B value = Reader::read(arg, params);
                        detail::runAndReply(fn, call, value);
                    },
                    Reader::check(), Reader::kind(), detail::returnKindName<R>(), false, Reader::factory()));
            }
        }
    }

    EventNode& event(const char* name) { return add(new EventNode(name)); }

    // A std::vector<T>, each element described by `describe(Object&, T&)`.
    template <class T, class F>
    ListNode& list(const char* name, std::vector<T>& items, F describe);
    // Same, with the element type's own describe (member or free function, as
    // for mount()).
    template <class T>
    ListNode& list(const char* name, std::vector<T>& items);

    // An array of scalars: a std::vector (resizable, up to maxSize()), a C
    // array or a std::array (fixed size). Const makes it read-only.
    template <class T>
    ArrayNode& array(const char* name, std::vector<T>& items) {
        return add(new detail::BoundArray<std::vector<T>>(name, items));
    }
    template <class T>
    ArrayNode& array(const char* name, const std::vector<T>& items) {
        return add(new detail::BoundArray<const std::vector<T>>(name, items));
    }
    template <class T, size_t N>
    ArrayNode& array(const char* name, T (&items)[N]) {
        return add(new detail::BoundArray<T[N]>(name, items));
    }
    template <class T, size_t N>
    ArrayNode& array(const char* name, const T (&items)[N]) {
        return add(new detail::BoundArray<const T[N]>(name, items));
    }
    template <class T, size_t N>
    ArrayNode& array(const char* name, std::array<T, N>& items) {
        return add(new detail::BoundArray<std::array<T, N>>(name, items));
    }

    // Another node's tree, reached through a datagram endpoint (NowTP, ...),
    // grafted here: requests below it are forwarded. See tesser/remote.h.
    RemoteNode& remote(const char* name, DatagramEndpoint& endpoint, const PeerAddress& peer,
                       const char* remotePath = "/");

    // Writer renders the node; the optional applier accepts writes.
    CustomNode& custom(const char* name, CustomNode::Writer writer, CustomNode::Applier applier = nullptr) {
        return add(new CustomNode(name, std::move(writer), std::move(applier)));
    }

    // Creates (or reuses) a child object and lets `thing` describe itself in
    // it, through either a member `describe(tesser::Object&)` or a free
    // function `describe(tesser::Object&, T&)` found by argument-dependent
    // lookup.
    template <class T>
    Object& mount(const char* name, T& thing);

    // Persists every value below this object (see Api::persistence()).
    Object& persist() {
        setFlag(kPersist);
        return *this;
    }

    Node* first() const { return first_; }
    Node* child(std::string_view name) const;
    size_t childCount() const;

protected:
    friend class DatagramEndpoint;  // mounts discovered peers

    template <class N>
    N& add(N* node) {
        link(node);
        return *node;
    }
    void link(Node* node);

private:
    Node* first_ = nullptr;
    Node* last_ = nullptr;
};

namespace detail {
template <class T>
void describeInto(Object& o, T& thing) {
    if constexpr (HasDescribeMember<T>::value) {
        thing.describe(o);
    } else {
        describe(o, thing);  // argument-dependent lookup
    }
}
}  // namespace detail

namespace detail {
template <class T>
struct ParamsObject : Object {
    ParamsObject() : Object(nullptr) {}
    T value{};
};

template <class T>
std::unique_ptr<Object> makeParams() {
    std::unique_ptr<ParamsObject<T>> p(new ParamsObject<T>());
    describeInto(*p, p->value);
    return std::unique_ptr<Object>(p.release());
}

template <class T>
T& paramsValue(Object& params) {
    return static_cast<ParamsObject<T>&>(params).value;
}
}  // namespace detail

inline std::unique_ptr<Object> ActionNode::params() const { return params_ ? params_() : nullptr; }

template <class T>
Object& Object::mount(const char* name, T& thing) {
    Object& o = object(name);
    detail::describeInto(o, thing);
    return o;
}

template <class T>
class VectorList final : public ListNode {
public:
    using Describe = std::function<void(Object&, T&)>;
    VectorList(const char* name, std::vector<T>& items, Describe describe)
        : ListNode(name), items_(items), describe_(std::move(describe)) {}

    size_t size() const override { return items_.size(); }
    std::unique_ptr<Object> element(size_t i) const override {
        std::unique_ptr<Object> o(new Object(nullptr));
        describe_(*o, items_[i]);
        return o;
    }
    std::unique_ptr<Object> prototype() const override {
        std::unique_ptr<Prototype> p(new Prototype());
        describe_(*p, p->value);
        return std::unique_ptr<Object>(p.release());
    }
    void resize(size_t n) override { items_.resize(n); }
    void reorder(const std::vector<int>& from) override {
        std::vector<T> next;
        next.reserve(from.size());
        for (int i : from) {
            if (i >= 0 && static_cast<size_t>(i) < items_.size()) {
                next.push_back(std::move(items_[static_cast<size_t>(i)]));
            } else {
                next.emplace_back();
            }
        }
        items_ = std::move(next);
    }

private:
    // An object that owns the default element it describes.
    struct Prototype : Object {
        Prototype() : Object(nullptr) {}
        T value{};
    };
    std::vector<T>& items_;
    Describe describe_;
};

template <class T, class F>
ListNode& Object::list(const char* name, std::vector<T>& items, F describe) {
    return add(new VectorList<T>(name, items, std::move(describe)));
}

template <class T>
ListNode& Object::list(const char* name, std::vector<T>& items) {
    return add(new VectorList<T>(name, items, [](Object& o, T& item) { detail::describeInto(o, item); }));
}

}  // namespace tesser

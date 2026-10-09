#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <ArduinoJson.h>

#include "tesser/call.h"
#include "tesser/json_writer.h"
#include "tesser/status.h"
#include "tesser/value_types.h"

namespace tesser {

class Api;
class Object;

enum class NodeType : uint8_t { Object, Value, Action, Event, Custom };

// Optional per-node metadata, allocated only when a modifier needs it.
struct NodeMeta {
    double min = 0;
    double max = 0;
    bool hasRange = false;
    bool hashed = false;  // watch(): `hash` holds the last sample
    const char* doc = nullptr;
    uint32_t hash = 0;
};

// Change tracking: a global generation counter, stamped on nodes as they
// change. 16 bits, compared with wrap-around; 0 means "never changed".
uint16_t currentGeneration();

// True if generation `a` is newer than `b`.
inline bool newerGeneration(uint16_t a, uint16_t b) { return a != 0 && static_cast<int16_t>(a - b) > 0; }

// Number of declaration mistakes (duplicate or invalid names) since boot.
// Each one is also logged.
int declarationErrors();

class Node {
public:
    Node(const char* name, NodeType type) : name_(name), type_(type) {}
    virtual ~Node();
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    const char* name() const { return name_; }
    NodeType type() const { return type_; }
    Node* next() const { return next_; }
    const NodeMeta* meta() const { return meta_; }
    bool persisted() const { return flags_ & kPersist; }
    bool watched() const { return flags_ & kWatch; }

    // Marks this node (for an object: every value below it) as changed, so
    // subscribers get it on the next Api::poll(). Writes through the API do
    // this automatically; call it when the application changes bound state.
    // Thread-safe and cheap.
    void changed();
    uint16_t generation() const { return generation_.load(std::memory_order_relaxed); }

protected:
    friend class Object;
    friend class Api;

    enum Flags : uint8_t { kReadOnly = 1, kPersist = 2, kDeferring = 4, kLinked = 8, kWatch = 16 };

    NodeMeta& editMeta();
    void setDoc(const char* text);

    const char* name_;
    Node* next_ = nullptr;
    NodeMeta* meta_ = nullptr;
    NodeType type_;
    uint8_t flags_ = 0;
    std::atomic<uint16_t> generation_{0};
};

// A typed scalar. Concrete classes bind it to a variable or to functions.
class ValueNode : public Node {
public:
    explicit ValueNode(const char* name) : Node(name, NodeType::Value) {}

    // Numeric bounds, checked on write and reported in the schema.
    ValueNode& range(double min, double max);
    ValueNode& readOnly();
    ValueNode& persist();
    ValueNode& doc(const char* text);
    // Samples the value periodically (Config::watchIntervalMs, while anyone
    // is subscribed) and marks it changed when it differs. For bound state
    // the application doesn't report with changed().
    ValueNode& watch();

    bool writable() const { return canWrite() && !(flags_ & kReadOnly); }

    virtual ValueKind kind() const = 0;
    virtual size_t maxLength() const { return 0; }
    virtual void write(JsonWriter& w) const = 0;
    // Type and range check. Doesn't consider writability.
    virtual Check check(JsonVariantConst v) const = 0;
    // Applies a value that passed check().
    virtual Check apply(JsonVariantConst v) = 0;

protected:
    virtual bool canWrite() const = 0;
    Check checkRange(double v) const;
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
class EventNode : public Node {
public:
    explicit EventNode(const char* name) : Node(name, NodeType::Event) {}

    void emit();  // null payload
    void emit(JsonVariantConst payload);
    template <class T>
    void emit(const T& payload) {
        emitWith([&payload](JsonWriter& w) { ValueTraits<std::remove_cv_t<T>>::write(w, payload); });
    }
    // Arbitrary JSON payload. Thread-safe; takes the API lock.
    void emitWith(const std::function<void(JsonWriter&)>& write);

    EventNode& doc(const char* text) {
        setDoc(text);
        return *this;
    }

private:
    Api* api_ = nullptr;  // found on the first emit()
    std::string path_;
};

class ActionNode : public Node {
public:
    // Converts the argument and runs the action; the result goes through
    // call.reply() / call.fail() / call.defer().
    using Invoker = std::function<void(JsonVariantConst arg, Call& call)>;
    using ArgCheck = Check (*)(JsonVariantConst);

    ActionNode(const char* name, Invoker invoker, ArgCheck argCheck, const char* argKindName,
               const char* returnKindName, bool defers)
        : Node(name, NodeType::Action),
          invoke_(std::move(invoker)),
          check_(argCheck),
          argKind_(argKindName),
          returnKind_(returnKindName) {
        if (defers) flags_ |= kDeferring;
    }

    ActionNode& doc(const char* text) {
        setDoc(text);
        return *this;
    }

    Check check(JsonVariantConst arg) const { return check_ ? check_(arg) : Check::ok(); }
    void invoke(JsonVariantConst arg, Call& call) { invoke_(arg, call); }
    bool deferring() const { return flags_ & kDeferring; }
    const char* argKind() const { return argKind_; }        // null: no argument
    const char* returnKind() const { return returnKind_; }  // null: no result

private:
    Invoker invoke_;
    ArgCheck check_;
    const char* argKind_;
    const char* returnKind_;
};

// Escape hatch: user code writes the JSON and applies incoming values.
class CustomNode : public Node {
public:
    using Writer = std::function<void(JsonWriter&)>;
    using Applier = std::function<Status(JsonVariantConst)>;
    using Validator = std::function<Check(JsonVariantConst)>;

    CustomNode(const char* name, Writer writer, Applier applier)
        : Node(name, NodeType::Custom), writer_(std::move(writer)), applier_(std::move(applier)) {}

    CustomNode& validate(Validator v) {
        validator_ = std::move(v);
        return *this;
    }
    CustomNode& persist() {
        flags_ |= kPersist;
        return *this;
    }
    CustomNode& doc(const char* text) {
        setDoc(text);
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

namespace detail {

// Signature of a callable: return type and (at most one) argument type.
template <class F>
struct Signature : Signature<decltype(&F::operator())> {};
template <class R, class... A>
struct Signature<R (*)(A...)> {
    using Return = R;
    static constexpr size_t arity = sizeof...(A);
    using Arg = std::tuple_element_t<0, std::tuple<A..., void>>;
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

}  // namespace detail

class Object : public Node {
public:
    explicit Object(const char* name) : Node(name, NodeType::Object) {}
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

    // An action taking nothing, one typed value, a JsonVariantConst, or a
    // tesser::Call& (for deferred replies). It may return void, a value or a
    // Status.
    template <class F>
    ActionNode& action(const char* name, F fn) {
        using Sig = detail::Signature<std::decay_t<F>>;
        using R = typename Sig::Return;
        static_assert(Sig::arity <= 1, "actions take at most one argument");
        if constexpr (Sig::arity == 0) {
            return add(new ActionNode(
                name, [fn = std::move(fn)](JsonVariantConst, Call& call) mutable { detail::runAndReply(fn, call); },
                &detail::checkNoArg, nullptr, detail::returnKindName<R>(), false));
        } else {
            using A = typename Sig::Arg;
            using B = detail::Bare<A>;
            if constexpr (std::is_same<B, Call>::value) {
                return add(new ActionNode(
                    name, [fn = std::move(fn)](JsonVariantConst, Call& call) mutable { detail::runAndReply(fn, call, call); },
                    &detail::checkNoArg, "any", nullptr, true));
            } else if constexpr (std::is_same<B, JsonVariantConst>::value || std::is_same<B, JsonVariant>::value) {
                return add(new ActionNode(
                    name,
                    [fn = std::move(fn)](JsonVariantConst arg, Call& call) mutable { detail::runAndReply(fn, call, arg); },
                    &detail::checkNoArg, "any", detail::returnKindName<R>(), false));
            } else {
                detail::assertSupported<B>();
                return add(new ActionNode(
                    name,
                    [fn = std::move(fn)](JsonVariantConst arg, Call& call) mutable {
                        B value{};
                        ValueTraits<B>::read(arg, value);
                        detail::runAndReply(fn, call, value);
                    },
                    &detail::checkArg<B>, kindName(ValueTraits<B>::kind), detail::returnKindName<R>(), false));
            }
        }
    }

    EventNode& event(const char* name) { return add(new EventNode(name)); }

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

    Object& doc(const char* text) {
        setDoc(text);
        return *this;
    }
    // Persists every value below this object (see Api::persistence()).
    Object& persist() {
        flags_ |= kPersist;
        return *this;
    }

    Node* first() const { return first_; }
    Node* child(std::string_view name) const;
    size_t childCount() const;

protected:
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

template <class T>
Object& Object::mount(const char* name, T& thing) {
    Object& o = object(name);
    detail::describeInto(o, thing);
    return o;
}

}  // namespace tesser

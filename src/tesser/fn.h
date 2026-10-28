#pragma once

#include <stddef.h>
#include <string.h>

#include <functional>
#include <new>  // placement new
#include <type_traits>
#include <utility>

namespace tesser {

template <class Sig>
class Fn;

namespace detail {
template <class F>
struct IsStdFunction : std::false_type {};
template <class S>
struct IsStdFunction<std::function<S>> : std::true_type {};
template <class S>
struct IsStdFunction<Fn<S>> : std::true_type {};  // another signature's, empty or not

// An empty std::function or Fn, or a null pointer: nothing to call.
template <class F>
bool isEmptyCallable(const F& f) {
    if constexpr (std::is_pointer<F>::value || std::is_member_pointer<F>::value) {
        return f == nullptr;
    } else if constexpr (IsStdFunction<F>::value) {
        return !f;
    } else {
        (void)f;
        return false;
    }
}
}  // namespace detail

// A move-only callable for the callbacks nodes hold (getters, setters,
// actions, validators): the same 16 bytes as a std::function on 32-bit
// targets, without the manager std::function instantiates for every callable
// type to copy, move and destroy it (docs/DESIGN.md section 15). Nodes are
// never copied, so it needn't be. A callable that is trivially copyable and
// fits 8 bytes (a lambda capturing a reference or two) is kept inline and
// moved by copying its bytes; any other is kept on the heap. Built from an
// empty std::function or a null function pointer, it is empty too.
template <class Sig>
class Fn;

template <class R, class... A>
class Fn<R(A...)> {
public:
    Fn() = default;
    Fn(std::nullptr_t) {}
    template <class F, class D = std::decay_t<F>,
              std::enable_if_t<!std::is_same<D, Fn>::value && std::is_invocable_r<R, D&, A...>::value, int> = 0>
    Fn(F&& f) {
        if (detail::isEmptyCallable(f)) return;
        if constexpr (kInline<D>) {
            new (buf_) D(std::forward<F>(f));
            invoke_ = [](void* b, A... a) -> R { return (*static_cast<D*>(b))(std::forward<A>(a)...); };
        } else {
            *reinterpret_cast<D**>(buf_) = new D(std::forward<F>(f));
            invoke_ = [](void* b, A... a) -> R { return (**static_cast<D**>(b))(std::forward<A>(a)...); };
            destroy_ = [](void* b) { delete *static_cast<D**>(b); };
        }
    }
    Fn(Fn&& o) noexcept { take(o); }
    Fn& operator=(Fn&& o) noexcept {
        if (this != &o) {
            reset();
            take(o);
        }
        return *this;
    }
    Fn& operator=(std::nullptr_t) {
        reset();
        return *this;
    }
    Fn(const Fn&) = delete;
    Fn& operator=(const Fn&) = delete;
    ~Fn() { reset(); }

    explicit operator bool() const { return invoke_ != nullptr; }
    // Calling an empty one is a bug, as with std::function: check first.
    R operator()(A... a) const { return invoke_(const_cast<unsigned char*>(buf_), std::forward<A>(a)...); }

private:
    template <class D>
    static constexpr bool kInline = sizeof(D) <= 8 && alignof(D) <= alignof(void*) &&
                                    std::is_trivially_copyable<D>::value;

    void take(Fn& o) {
        memcpy(buf_, o.buf_, sizeof(buf_));
        invoke_ = o.invoke_;
        destroy_ = o.destroy_;
        o.invoke_ = nullptr;
        o.destroy_ = nullptr;
    }
    void reset() {
        if (destroy_) destroy_(buf_);
        invoke_ = nullptr;
        destroy_ = nullptr;
    }

    alignas(void*) unsigned char buf_[8] = {};
    R (*invoke_)(void*, A...) = nullptr;
    void (*destroy_)(void*) = nullptr;  // null for an inline callable: nothing to destroy
};

}  // namespace tesser

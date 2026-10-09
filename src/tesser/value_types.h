#pragma once

#include <float.h>
#include <stddef.h>
#include <string.h>

#include <string>
#include <string_view>
#include <type_traits>

#include <ArduinoJson.h>

#include "tesser/json_writer.h"
#include "tesser/status.h"

#if defined(ARDUINO)
#include <WString.h>
#endif

namespace tesser {

enum class ValueKind : uint8_t { Boolean, Integer, Number, String };

const char* kindName(ValueKind k);

// Result of validating a JSON value against a node.
struct Check {
    Status status = Status::Ok;
    const char* message = nullptr;

    static Check ok() { return {}; }
    static Check fail(Status s, const char* m) { return {s, m}; }
    bool isOk() const { return status == Status::Ok; }
};

// How a C++ type maps to JSON. Specializations provide:
//   kind                         ValueKind
//   write(JsonWriter&, const T&) render a value
//   check(JsonVariantConst)      type check (no user range)
//   read(JsonVariantConst, T&)   convert; only called after check() passed
//   toDouble(const T&)           numeric kinds, for range checks
//   maxLength                    String kinds with a fixed capacity, else 0
template <class T, class Enable = void>
struct ValueTraits {
    static constexpr bool supported = false;
};

template <>
struct ValueTraits<bool> {
    static constexpr bool supported = true;
    static constexpr ValueKind kind = ValueKind::Boolean;
    static constexpr size_t maxLength = 0;
    static void write(JsonWriter& w, bool v) { w.boolean(v); }
    static Check check(JsonVariantConst j) {
        return j.is<bool>() ? Check::ok() : Check::fail(Status::InvalidValue, "expected boolean");
    }
    static void read(JsonVariantConst j, bool& out) { out = j.as<bool>(); }
    static double toDouble(bool v) { return v ? 1 : 0; }
};

template <class T>
struct ValueTraits<T, std::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value>> {
    static constexpr bool supported = true;
    static constexpr ValueKind kind = ValueKind::Integer;
    static constexpr size_t maxLength = 0;
    static void write(JsonWriter& w, T v) {
        if (std::is_signed<T>::value) {
            w.integer(static_cast<int64_t>(v));
        } else {
            w.uinteger(static_cast<uint64_t>(v));
        }
    }
    static Check check(JsonVariantConst j) {
        if (j.is<T>()) return Check::ok();
        if (j.is<long long>() || j.is<unsigned long long>()) {
            return Check::fail(Status::InvalidValue, "integer out of range for type");
        }
        return Check::fail(Status::InvalidValue, "expected integer");
    }
    static void read(JsonVariantConst j, T& out) { out = j.as<T>(); }
    static double toDouble(T v) { return static_cast<double>(v); }
};

template <class T>
struct ValueTraits<T, std::enable_if_t<std::is_floating_point<T>::value>> {
    static constexpr bool supported = true;
    static constexpr ValueKind kind = ValueKind::Number;
    static constexpr size_t maxLength = 0;
    static void write(JsonWriter& w, T v) {
        w.number(static_cast<double>(v), std::is_same<T, float>::value ? 7 : 15);
    }
    static Check check(JsonVariantConst j) {
        if (!j.is<double>()) return Check::fail(Status::InvalidValue, "expected number");
        if (std::is_same<T, float>::value) {
            double d = j.as<double>();
            if (d > FLT_MAX || d < -FLT_MAX) {
                return Check::fail(Status::InvalidValue, "number out of range for type");
            }
        }
        return Check::ok();
    }
    static void read(JsonVariantConst j, T& out) { out = static_cast<T>(j.as<double>()); }
    static double toDouble(T v) { return static_cast<double>(v); }
};

inline Check checkString(JsonVariantConst j) {
    return j.is<const char*>() ? Check::ok() : Check::fail(Status::InvalidValue, "expected string");
}

template <>
struct ValueTraits<std::string> {
    static constexpr bool supported = true;
    static constexpr ValueKind kind = ValueKind::String;
    static constexpr size_t maxLength = 0;
    static void write(JsonWriter& w, const std::string& v) { w.string(v); }
    static Check check(JsonVariantConst j) { return checkString(j); }
    static void read(JsonVariantConst j, std::string& out) { out = j.as<const char*>(); }
};

// Read-only: there is no storage to write into.
template <>
struct ValueTraits<const char*> {
    static constexpr bool supported = true;
    static constexpr ValueKind kind = ValueKind::String;
    static constexpr size_t maxLength = 0;
    static void write(JsonWriter& w, const char* v) {
        if (v) {
            w.string(v);
        } else {
            w.null();
        }
    }
    static Check check(JsonVariantConst j) { return checkString(j); }
    // The pointer is valid only for the duration of the request.
    static void read(JsonVariantConst j, const char*& out) { out = j.as<const char*>(); }
};

template <>
struct ValueTraits<std::string_view> {
    static constexpr bool supported = true;
    static constexpr ValueKind kind = ValueKind::String;
    static constexpr size_t maxLength = 0;
    static void write(JsonWriter& w, std::string_view v) { w.string(v); }
    static Check check(JsonVariantConst j) { return checkString(j); }
    static void read(JsonVariantConst j, std::string_view& out) {
        JsonString s = j.as<JsonString>();
        out = std::string_view(s.c_str(), s.size());
    }
};

// Fixed buffer, always NUL-terminated.
template <size_t N>
struct ValueTraits<char[N]> {
    static_assert(N > 0, "zero-length buffer");
    static constexpr bool supported = true;
    static constexpr ValueKind kind = ValueKind::String;
    static constexpr size_t maxLength = N - 1;
    static void write(JsonWriter& w, const char (&v)[N]) { w.string(std::string_view(v, strnlen(v, N))); }
    static Check check(JsonVariantConst j) {
        Check c = checkString(j);
        if (!c.isOk()) return c;
        if (j.as<JsonString>().size() > N - 1) return Check::fail(Status::InvalidValue, "string too long");
        return c;
    }
    static void read(JsonVariantConst j, char (&out)[N]) {
        JsonString s = j.as<JsonString>();
        size_t n = s.size() < N - 1 ? s.size() : N - 1;
        memcpy(out, s.c_str(), n);
        out[n] = '\0';
    }
};

#if defined(ARDUINO)
template <>
struct ValueTraits<String> {
    static constexpr bool supported = true;
    static constexpr ValueKind kind = ValueKind::String;
    static constexpr size_t maxLength = 0;
    static void write(JsonWriter& w, const String& v) { w.string(std::string_view(v.c_str(), v.length())); }
    static Check check(JsonVariantConst j) { return checkString(j); }
    static void read(JsonVariantConst j, String& out) { out = j.as<const char*>(); }
};
#endif

template <class T>
constexpr bool isNumericKind() {
    return ValueTraits<T>::kind == ValueKind::Integer || ValueTraits<T>::kind == ValueKind::Number;
}

}  // namespace tesser

// Minimal test harness shared by the host tests and the on-target firmware.
// Prints "PASS <name>" / "FAIL <name>: ..." lines, then
// "DONE pass=<n> fail=<n>".
#pragma once

#include <stdio.h>
#include <string.h>

#include <string>
#include <type_traits>
#include <vector>

namespace check {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int g_caseFailures = 0;
inline std::string g_firstFailure;

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline void fail(const char* file, int line, const std::string& what) {
    if (g_caseFailures++ == 0) {
        const char* base = strrchr(file, '/');
        g_firstFailure = std::string(base ? base + 1 : file) + ":" + std::to_string(line) + ": " + what;
    }
}

inline bool expect(bool ok, const char* expr, const char* file, int line) {
    if (!ok) fail(file, line, expr);
    return ok;
}

inline std::string show(const std::string& s) { return "\"" + s + "\""; }
inline std::string show(const char* s) { return s ? show(std::string(s)) : "null"; }
inline std::string show(bool v) { return v ? "true" : "false"; }
template <class T>
std::string show(const T& v) {
    if constexpr (std::is_enum<T>::value) {
        return std::to_string(static_cast<long long>(v));
    } else {
        return std::to_string(v);
    }
}

template <class A, class B>
bool expectEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    bool ok;
    if constexpr (std::is_convertible<A, std::string>::value && std::is_convertible<B, std::string>::value) {
        ok = std::string(a) == std::string(b);
    } else {
        ok = a == b;
    }
    if (!ok) fail(file, line, std::string(ea) + " == " + eb + " (got " + show(a) + " vs " + show(b) + ")");
    return ok;
}

// Runs every case whose name contains `filter` (all when null or empty).
inline int run(const char* filter) {
    int pass = 0, failed = 0;
    for (const Case& c : registry()) {
        if (filter && *filter && !strstr(c.name, filter)) continue;
        g_caseFailures = 0;
        g_firstFailure.clear();
        c.fn();
        if (g_caseFailures == 0) {
            pass++;
            printf("PASS %s\n", c.name);
        } else {
            failed++;
            printf("FAIL %s: %s\n", c.name, g_firstFailure.c_str());
        }
    }
    printf("DONE pass=%d fail=%d\n", pass, failed);
    return failed;
}

}  // namespace check

#define TEST(name)                                                    \
    static void test_##name();                                        \
    static check::Registrar registrar_##name(#name, &test_##name);    \
    static void test_##name()

#define CHECK(cond) check::expect((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) check::expectEq((a), (b), #a, #b, __FILE__, __LINE__)

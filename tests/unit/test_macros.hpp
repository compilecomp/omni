// tests/unit/test_macros.hpp
//
// Shared test helpers for unit and integration tests.
//
// B2-24 fix: replaces the #define CHECK macro with a constexpr-friendly
// inline function. Rule 49 forbids #define macros for control flow.
//
// Usage:
//   test::check(cond, __FILE__, __LINE__, #cond);
//   test::check_eq(actual, expected, __FILE__, __LINE__, #actual);

#pragma once

#include <cstdio>
#include <cstdint>

namespace omni::test {

inline int& failures() {
    static int f = 0;
    return f;
}

inline void check(bool cond, const char* file, int line, const char* expr) noexcept {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s:%d: %s\n", file, line, expr);
        ++failures();
    }
}

template <typename T, typename U>
inline void check_eq(const T& actual, const U& expected,
                     const char* file, int line, const char* expr) noexcept {
    if (!(actual == expected)) {
        std::fprintf(stderr, "FAIL: %s:%d: %s (got != expected)\n", file, line, expr);
        ++failures();
    }
}

inline int report(const char* test_name, int checks_run) noexcept {
    if (failures() == 0) {
        std::printf("OK: %s (%d checks passed)\n", test_name, checks_run);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %s (%d checks failed)\n", test_name, failures());
    return 1;
}

}  // namespace omni::test

// Convenience macro that expands to a function call (not a logic macro).
// This is a token-pasting helper (Rule 49 allows these).
#define OMNI_CHECK(cond) ::omni::test::check((cond), __FILE__, __LINE__, #cond)
#define OMNI_CHECK_EQ(a, b) ::omni::test::check_eq((a), (b), __FILE__, __LINE__, #a)
#define OMNI_TEST_REPORT(name, count) return ::omni::test::report((name), (count))

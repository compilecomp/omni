// tests/unit/test_speculative_arithmetic.cpp
//
// Unit tests for core/interpreter/speculative_arithmetic.cpp.
//
// Covers:
//   - int add success path.
//   - int add overflow fallback (Rule 72: must fall back to BigInt).
//   - int add type mismatch fallback.
//   - float add success path.
//   - float add NaN preservation (Rule 72).
//   - int div by zero fallback.
//   - int div INT64_MIN / -1 overflow fallback.

#include "core/interpreter/speculative_arithmetic.hpp"
#include "core/object_model/tagged_value.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

using namespace omni::interpreter;
using namespace omni::object_model;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void test_int_add_success() {
    auto r = spec_int_add(TaggedValue::make_int(2), TaggedValue::make_int(3));
    CHECK(r.has_value());
    CHECK(r->is_int());
    CHECK(r->as_int() == 5);
}

static void test_int_add_overflow() {
    auto r = spec_int_add(TaggedValue::make_int(INT64_MAX),
                           TaggedValue::make_int(1));
    // Must fall back (Rule 72: overflow → BigInt, not wrap).
    CHECK(!r.has_value());
}

static void test_int_add_type_mismatch() {
    auto r = spec_int_add(TaggedValue::make_int(1), TaggedValue::make_float(2.0));
    CHECK(!r.has_value());
    auto r2 = spec_int_add(TaggedValue::make_null(), TaggedValue::make_int(1));
    CHECK(!r2.has_value());
}

static void test_float_add_success() {
    auto r = spec_float_add(TaggedValue::make_float(2.5),
                              TaggedValue::make_float(3.5));
    CHECK(r.has_value());
    CHECK(r->is_float());
    CHECK(r->as_float() == 6.0);
}

static void test_float_add_nan_preserved() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    auto r = spec_float_add(TaggedValue::make_float(nan),
                              TaggedValue::make_float(1.0));
    CHECK(r.has_value());
    CHECK(std::isnan(r->as_float()));
}

static void test_int_div_by_zero() {
    auto r = spec_int_div(TaggedValue::make_int(1), TaggedValue::make_int(0));
    CHECK(!r.has_value());
}

static void test_int_div_overflow() {
    auto r = spec_int_div(TaggedValue::make_int(INT64_MIN),
                           TaggedValue::make_int(-1));
    CHECK(!r.has_value());
}

static void test_int_mul_overflow() {
    auto r = spec_int_mul(TaggedValue::make_int(INT64_MAX),
                           TaggedValue::make_int(2));
    CHECK(!r.has_value());
    auto r2 = spec_int_mul(TaggedValue::make_int(INT64_MIN), TaggedValue::make_int(-1));
    CHECK(!r2.has_value());
}

int main() {
    test_int_add_success();
    test_int_add_overflow();
    test_int_add_type_mismatch();
    test_float_add_success();
    test_float_add_nan_preserved();
    test_int_div_by_zero();
    test_int_div_overflow();
    test_int_mul_overflow();
    if (g_failures == 0) {
        std::printf("OK: speculative_arithmetic (%d checks passed)\n", 8);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d checks failed\n", g_failures);
    return 1;
}

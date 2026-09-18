// tests/unit/test_tagged_value.cpp
//
// Unit tests for core/object_model/tagged_value.hpp.
//
// Covers:
//   - Construction of each tag kind.
//   - Tag predicates (is_null, is_int, etc.).
//   - Accessors (as_int, as_float, etc.).
//   - Bitwise equality (including NaN != NaN per Rule 72).
//   - 16-byte size invariant.

#include "core/object_model/tagged_value.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

using namespace omni::object_model;
using namespace omni::common;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void test_size_invariant() {
    CHECK(sizeof(TaggedValue) == 16);
}

static void test_construction_and_predicates() {
    CHECK(TaggedValue::make_null().is_null());
    CHECK(TaggedValue::make_bool(true).is_bool());
    CHECK(TaggedValue::make_bool(true).as_bool());
    CHECK(!TaggedValue::make_bool(false).as_bool());
    CHECK(TaggedValue::make_int(42).is_int());
    CHECK(TaggedValue::make_int(42).as_int() == 42);
    CHECK(TaggedValue::make_int(-1).as_int() == -1);
    CHECK(TaggedValue::make_int(INT64_MIN).as_int() == INT64_MIN);
    CHECK(TaggedValue::make_int(INT64_MAX).as_int() == INT64_MAX);
    CHECK(TaggedValue::make_float(3.14).is_float());
    CHECK(TaggedValue::make_float(3.14).as_float() == 3.14);
    CHECK(TaggedValue::make_str(SymbolId{7}).is_str());
    CHECK(TaggedValue::make_str(SymbolId{7}).as_str() == SymbolId{7});
}

static void test_nan_preserved() {
    // Rule 72: NaN bit pattern must be preserved exactly.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    TaggedValue v = TaggedValue::make_float(nan);
    CHECK(std::isnan(v.as_float()));
    // NaN != NaN per IEEE 754.
    CHECK(v.as_float() != v.as_float());
    // Bitwise equality of two NaN TaggedValues (same bit pattern).
    TaggedValue v2 = TaggedValue::make_float(nan);
    CHECK(v.bitwise_eq(v2));
}

static void test_negative_zero_preserved() {
    // Rule 72: -0.0 must be preserved exactly (not folded to +0.0).
    const double neg_zero = -0.0;
    TaggedValue v = TaggedValue::make_float(neg_zero);
    double f = v.as_float();
    uint64_t bits;
    std::memcpy(&bits, &f, sizeof(uint64_t));
    CHECK(bits != 0);  // -0.0 has sign bit set
    CHECK(std::signbit(f));
    // +0.0 is a different bit pattern.
    TaggedValue pos_zero = TaggedValue::make_float(0.0);
    CHECK(!v.bitwise_eq(pos_zero));
}

static void test_distinct_tags() {
    // Tag identity: int 1 and bool true are different values.
    TaggedValue one = TaggedValue::make_int(1);
    TaggedValue tru = TaggedValue::make_bool(true);
    CHECK(!one.bitwise_eq(tru));
    CHECK(one.tag() == Tag::Int);
    CHECK(tru.tag() == Tag::Bool);
    // Null is distinct from int 0 and bool false.
    CHECK(!TaggedValue::make_null().bitwise_eq(TaggedValue::make_int(0)));
    CHECK(!TaggedValue::make_null().bitwise_eq(TaggedValue::make_bool(false)));
}

int main() {
    test_size_invariant();
    test_construction_and_predicates();
    test_nan_preserved();
    test_negative_zero_preserved();
    test_distinct_tags();
    if (g_failures == 0) {
        std::printf("OK: tagged_value (%d checks passed)\n", 5);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d checks failed\n", g_failures);
    return 1;
}

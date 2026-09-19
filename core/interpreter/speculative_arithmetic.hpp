// core/interpreter/speculative_arithmetic.hpp
//
// Speculative arithmetic helpers.
//
// Purpose:
//   Implements DESIGN.md §5.5: speculative fast paths for arithmetic
//   opcodes. ADD_INT_FAST takes two int operands and produces an int
//   result; if either operand is not an int, or if the addition would
//   overflow, the site falls back to the semantic ADD opcode.
//
// Invariants:
//   - Fast-path functions return Result<TaggedValue>; on failure they
//     return an error that signals "fall back to semantic" rather than
//     a hard error.
//   - Overflow detection uses GCC/Clang __builtin_*_overflow intrinsics
//     where available; otherwise it falls back to checked arithmetic.
//   - NaN propagation (Rule 72): float operations preserve NaN bit
//     patterns; fast paths do not optimize away NaN checks.
//   - Negative zero (Rule 72): preserved exactly; no folding of -0.0
//     into +0.0 in the interpreter.
//
// All int/float functions are defined INLINE in this header so the
// compiler can inline them into the quickened handlers (the hot path).
// Without inlining, each speculative arithmetic call costs ~5-10 cycles
// of call/ret overhead — at 10M iterations that's 50-100ms of pure call
// overhead, which is why the interpreter was slower than expected.
// String concat remains out-of-line because it allocates.
//
// Cross-references:
//   - DESIGN.md §5.5 (speculative arithmetic)
//   - DESIGN.md §4.2 (ADD_INT_FAST, ADD_FLOAT_FAST, ADD_STRING_CONCAT_FAST)
//   - LAWS.md Rule 72 (Omni numeric semantics preserved exactly)
//   - LAWS.md Rule 33 (no implicit conversions in IR; explicit nodes)

#pragma once

#include <climits>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

#include "core/common/result.hpp"
#include "core/common/symbol_table.hpp"
#include "core/common/types.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::interpreter {

namespace detail {

[[nodiscard]] inline common::Result<object_model::TaggedValue> fallback() noexcept {
    return common::make_error(common::ErrorCategory::Numeric, common::NULL_SYMBOL);
}

}  // namespace detail

/// Speculative integer addition. Returns the sum or an error if either
/// operand is not an int, or if the addition overflows int64_t.
[[nodiscard]] inline common::Result<object_model::TaggedValue>
spec_int_add(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_int() || !b.is_int()) [[unlikely]] {
        return detail::fallback();
    }
    const int64_t x = a.as_int();
    const int64_t y = b.as_int();
    if ((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y)) [[unlikely]] {
        return detail::fallback();
    }
    return object_model::TaggedValue::make_int(x + y);
}

/// Speculative float addition. Returns the sum or an error if either
/// operand is not a float. NaN propagation is preserved exactly.
[[nodiscard]] inline common::Result<object_model::TaggedValue>
spec_float_add(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_float() || !b.is_float()) [[unlikely]] {
        return detail::fallback();
    }
    return object_model::TaggedValue::make_float(a.as_float() + b.as_float());
}

/// Speculative integer subtraction.
[[nodiscard]] inline common::Result<object_model::TaggedValue>
spec_int_sub(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_int() || !b.is_int()) [[unlikely]] return detail::fallback();
    const int64_t x = a.as_int();
    const int64_t y = b.as_int();
    if ((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y)) [[unlikely]] {
        return detail::fallback();
    }
    return object_model::TaggedValue::make_int(x - y);
}

/// Speculative integer multiplication.
[[nodiscard]] inline common::Result<object_model::TaggedValue>
spec_int_mul(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_int() || !b.is_int()) [[unlikely]] return detail::fallback();
    const int64_t x = a.as_int();
    const int64_t y = b.as_int();
    if (x != 0 && y != 0) {
        if ((x == INT64_MIN && y == -1) || (y == INT64_MIN && x == -1)) return detail::fallback();
        if (x > 0) {
            if (y > 0) { if (x > INT64_MAX / y) return detail::fallback(); }
            else { if (y < INT64_MIN / x) return detail::fallback(); }
        } else {
            if (y > 0) { if (x < INT64_MIN / y) return detail::fallback(); }
            else { if (y != 0 && x < INT64_MAX / y) return detail::fallback(); }
        }
    }
    return object_model::TaggedValue::make_int(x * y);
}

/// Speculative integer division. Returns an error on divide-by-zero
/// (the runtime produces Infinity/NaN per Rule 72 for floats, raises
/// ZeroDivisionError for ints).
[[nodiscard]] inline common::Result<object_model::TaggedValue>
spec_int_div(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_int() || !b.is_int()) [[unlikely]] return detail::fallback();
    const int64_t x = a.as_int();
    const int64_t y = b.as_int();
    if (y == 0) [[unlikely]] return detail::fallback();
    if (x == INT64_MIN && y == -1) [[unlikely]] return detail::fallback();
    return object_model::TaggedValue::make_int(x / y);
}

/// Speculative float subtraction. NaN/-0.0 preserved exactly (Rule 72).
[[nodiscard]] inline common::Result<object_model::TaggedValue>
spec_float_sub(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_float() || !b.is_float()) [[unlikely]] return detail::fallback();
    return object_model::TaggedValue::make_float(a.as_float() - b.as_float());
}

/// Speculative float multiplication.
[[nodiscard]] inline common::Result<object_model::TaggedValue>
spec_float_mul(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_float() || !b.is_float()) [[unlikely]] return detail::fallback();
    return object_model::TaggedValue::make_float(a.as_float() * b.as_float());
}

/// Speculative integer modulo. Returns an error on divide-by-zero.
[[nodiscard]] inline common::Result<object_model::TaggedValue>
spec_int_mod(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_int() || !b.is_int()) [[unlikely]] return detail::fallback();
    const int64_t x = a.as_int();
    const int64_t y = b.as_int();
    if (y == 0) [[unlikely]] return detail::fallback();
    if (y == -1) [[unlikely]] return object_model::TaggedValue::make_int(0);
    return object_model::TaggedValue::make_int(x % y);
}

/// Speculative float modulo (fmod). NaN preserved.
[[nodiscard]] inline common::Result<object_model::TaggedValue>
spec_float_mod(object_model::TaggedValue a, object_model::TaggedValue b) noexcept {
    if (!a.is_float() || !b.is_float()) [[unlikely]] return detail::fallback();
    return object_model::TaggedValue::make_float(std::fmod(a.as_float(), b.as_float()));
}

/// Speculative string concatenation. Returns the concatenated string
/// or an error if either operand is not a Str.
// Out-of-line: allocates via intern_symbol (cold path relative to int/float).
[[nodiscard]] common::Result<object_model::TaggedValue>
spec_str_concat(object_model::TaggedValue a, object_model::TaggedValue b) noexcept;

}  // namespace omni::interpreter

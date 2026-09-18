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
// Cross-references:
//   - DESIGN.md §5.5 (speculative arithmetic)
//   - DESIGN.md §4.2 (ADD_INT_FAST, ADD_FLOAT_FAST, ADD_STRING_CONCAT_FAST)
//   - LAWS.md Rule 72 (Omni numeric semantics preserved exactly)
//   - LAWS.md Rule 33 (no implicit conversions in IR; explicit nodes)

#pragma once

#include "core/common/result.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::interpreter {

/// Speculative integer addition. Returns the sum or an error if either
/// operand is not an int, or if the addition overflows int64_t.
[[nodiscard]] common::Result<object_model::TaggedValue>
spec_int_add(object_model::TaggedValue a, object_model::TaggedValue b) noexcept;

/// Speculative float addition. Returns the sum or an error if either
/// operand is not a float. NaN propagation is preserved exactly.
[[nodiscard]] common::Result<object_model::TaggedValue>
spec_float_add(object_model::TaggedValue a, object_model::TaggedValue b) noexcept;

/// Speculative integer subtraction.
[[nodiscard]] common::Result<object_model::TaggedValue>
spec_int_sub(object_model::TaggedValue a, object_model::TaggedValue b) noexcept;

/// Speculative integer multiplication.
[[nodiscard]] common::Result<object_model::TaggedValue>
spec_int_mul(object_model::TaggedValue a, object_model::TaggedValue b) noexcept;

/// Speculative integer division. Returns an error on divide-by-zero
/// (the runtime produces Infinity/NaN per Rule 72 for floats, raises
/// ZeroDivisionError for ints).
[[nodiscard]] common::Result<object_model::TaggedValue>
spec_int_div(object_model::TaggedValue a, object_model::TaggedValue b) noexcept;

/// Speculative string concatenation. Returns the concatenated string
/// or an error if either operand is not a Str.
[[nodiscard]] common::Result<object_model::TaggedValue>
spec_str_concat(object_model::TaggedValue a, object_model::TaggedValue b) noexcept;

}  // namespace omni::interpreter

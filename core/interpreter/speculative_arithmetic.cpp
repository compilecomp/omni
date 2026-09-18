// core/interpreter/speculative_arithmetic.cpp

#include "core/interpreter/speculative_arithmetic.hpp"

#include <cerrno>
#include <climits>
#include <string>

#include "core/common/symbol_table.hpp"
#include "core/common/types.hpp"

namespace omni::interpreter {

using namespace common;
using namespace object_model;

namespace {

/// Error category for "fast path failed, fall back to semantic".
/// This is not a hard error — it's a signal that the interpreter should
/// re-dispatch through the generic handler.
constexpr SymbolId ERR_FALLBACK = NULL_SYMBOL;

Result<TaggedValue> fallback() noexcept {
    return make_error(ErrorCategory::Numeric, ERR_FALLBACK);
}

}  // namespace

Result<TaggedValue> spec_int_add(TaggedValue a, TaggedValue b) noexcept {
    if (!a.is_int() || !b.is_int()) [[unlikely]] {
        return fallback();
    }
    const int64_t x = a.as_int();
    const int64_t y = b.as_int();
    // Check for overflow (Rule 72: integer overflow must fall back to
    // BigInt, not silently wrap).
    if ((y > 0 && x > INT64_MAX - y) || (y < 0 && x < INT64_MIN - y)) [[unlikely]] {
        return fallback();  // BigInt path
    }
    return TaggedValue::make_int(x + y);
}

Result<TaggedValue> spec_float_add(TaggedValue a, TaggedValue b) noexcept {
    if (!a.is_float() || !b.is_float()) [[unlikely]] {
        return fallback();
    }
    // Plain float addition. NaN propagation is automatic (IEEE 754);
    // we do NOT fold -0.0 + 0.0 = 0.0 (Rule 72: preserve negative zero).
    return TaggedValue::make_float(a.as_float() + b.as_float());
}

Result<TaggedValue> spec_int_sub(TaggedValue a, TaggedValue b) noexcept {
    if (!a.is_int() || !b.is_int()) [[unlikely]] return fallback();
    const int64_t x = a.as_int();
    const int64_t y = b.as_int();
    if ((y < 0 && x > INT64_MAX + y) || (y > 0 && x < INT64_MIN + y)) [[unlikely]] {
        return fallback();
    }
    return TaggedValue::make_int(x - y);
}

Result<TaggedValue> spec_int_mul(TaggedValue a, TaggedValue b) noexcept {
    if (!a.is_int() || !b.is_int()) [[unlikely]] return fallback();
    const int64_t x = a.as_int();
    const int64_t y = b.as_int();
    // Overflow detection for multiplication.
    if (x != 0 && y != 0) {
        if ((x == INT64_MIN && y == -1) || (y == INT64_MIN && x == -1)) return fallback();
        if (x > 0) {
            if (y > 0) { if (x > INT64_MAX / y) return fallback(); }
            else { if (y < INT64_MIN / x) return fallback(); }
        } else {
            if (y > 0) { if (x < INT64_MIN / y) return fallback(); }
            else { if (y != 0 && x < INT64_MAX / y) return fallback(); }
        }
    }
    return TaggedValue::make_int(x * y);
}

Result<TaggedValue> spec_int_div(TaggedValue a, TaggedValue b) noexcept {
    if (!a.is_int() || !b.is_int()) [[unlikely]] return fallback();
    const int64_t x = a.as_int();
    const int64_t y = b.as_int();
    if (y == 0) [[unlikely]] {
        // Rule 72: division by zero raises ZeroDivisionError for ints
        // (Infinity for floats).
        return make_error(ErrorCategory::Numeric, ERR_FALLBACK);
    }
    // INT64_MIN / -1 overflows.
    if (x == INT64_MIN && y == -1) [[unlikely]] return fallback();
    return TaggedValue::make_int(x / y);
}

Result<TaggedValue> spec_str_concat(TaggedValue a, TaggedValue b) noexcept {
    if (!a.is_str() || !b.is_str()) [[unlikely]] return fallback();
    // For string concat we need to allocate a new string and intern it.
    // This is the slow path; in a real implementation we would have a
    // string-builder helper that avoids the intermediate std::string.
    std::string_view sa = common::resolve_symbol(a.as_str());
    std::string_view sb = common::resolve_symbol(b.as_str());
    std::string combined;
    combined.reserve(sa.size() + sb.size());
    combined.append(sa);
    combined.append(sb);
    auto r = common::intern_symbol(combined);
    if (!r.has_value()) [[unlikely]] return std::unexpected(r.error());
    return TaggedValue::make_str(*r);
}

}  // namespace omni::interpreter

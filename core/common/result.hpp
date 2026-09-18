// core/common/result.hpp
//
// Result type and zero-cost error-propagation helpers.
//
// Purpose:
//   Implements Laws Rule 22 (zero-cost error propagation): use std::expected
//   and monadic operations or a custom TRY() macro that compiles down to a
//   single branch. Combined with Rule 48 ([[nodiscard]] on all Result types),
//   this keeps the hot path branch-predictor-friendly and forces callers
//   to handle errors explicitly.
//
// Invariants:
//   - Result<T> = std::expected<T, Error>
//   - Error is a tagged-variant over error categories with an attached
//     SymbolId for the message (Rule 16: no std::string).
//   - TRY() is a single branch on the unexpected state; on the success
//     path it just unwraps the value.
//
// Cross-references:
//   - LAWS.md Rule 22 (Zero-Cost Error Propagation)
//   - LAWS.md Rule 47 (Actionable Compiler Diagnostics)
//   - LAWS.md Rule 48 ([[nodiscard]] on all Result types)

#pragma once

#include <cstdint>
#include <expected>
#include <utility>

#include "core/common/types.hpp"

namespace omni::common {

enum class ErrorCategory : uint8_t {
    None = 0,
    /// Bytecode-level: malformed instruction, bad register, etc.
    Bytecode = 1,
    /// Type-shape mismatch at a guard or IC miss.
    TypeShape = 2,
    /// Numeric: overflow, NaN-where-expected-int, BigInt mixing.
    Numeric = 3,
    /// Object model: bad shape transition, missing capability.
    ObjectModel = 4,
    /// Stack overflow or recursion limit hit.
    Stack = 5,
    /// GC or memory allocation failure.
    Memory = 6,
    /// Concurrency: deadlock, task cancellation, sync violation.
    Concurrency = 7,
    /// FFI / native interop failure.
    FFI = 8,
    /// Verification gate failure (IR/region/machine/vector).
    Verify = 9,
    /// Code cache budget exceeded.
    CodeCacheBudget = 10,
    /// Internal invariant violation; should not happen.
    Internal = 11,
};

/// Error value. Designed to be cheap to copy (16 bytes).
/// The message is interned (Rule 16: no std::string).
struct Error {
    ErrorCategory category{ErrorCategory::None};
    /// Interned error message symbol. Resolved by the Explainable Error
    /// Engine (Laws Rule 47, DESIGN.md §21.3).
    SymbolId message_symbol{NULL_SYMBOL};
    /// Optional site id where the error originated (for diagnostics).
    SiteId site_id{0};

    constexpr Error() noexcept = default;
    constexpr Error(ErrorCategory c, SymbolId msg, SiteId site = 0) noexcept
        : category(c), message_symbol(msg), site_id(site) {}

    [[nodiscard]] constexpr bool is_none() const noexcept {
        return category == ErrorCategory::None;
    }
};

template <typename T>
using Result = std::expected<T, Error>;

/// Helper to construct an unexpected Error.
constexpr auto make_error(ErrorCategory c, SymbolId msg, SiteId site = 0) noexcept {
    return std::unexpected<Error>(Error{c, msg, site});
}

}  // namespace omni::common

// Note (B3 fix): the previous OMNI_TRY and OMNI_TRY_INPLACE macros have
// been removed. Rule 49 forbids #define macros for control flow. Use
// std::expected's monadic operations (and_then, transform, or_else)
// or constexpr helpers instead.
//
// Example pattern:
//   Result<int> r = compute();
//   int v = r.value_or(0);  // if a default is acceptable
//
//   // Or with propagation:
//   Result<int> r = compute();
//   if (!r) return std::unexpected(r.error());  // single branch
//   int v = *r;


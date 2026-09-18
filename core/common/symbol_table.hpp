// core/common/symbol_table.hpp
//
// Interned symbol table.
//
// Purpose:
//   Implements Laws Rule 16: never pass, compare, or store std::string or
//   std::string_view in the IR or passes. All identifiers must be interned
//   into a global SymbolTable at the frontend; the IR only uses SymbolId.
//
//   This module is the global, thread-safe, interned-string registry.
//   It is one of the few allowed global state holders (Laws Rule 125).
//
// Invariants:
//   - SymbolId is a uint32_t; ids are stable for the lifetime of the
//     process.
//   - Interning is idempotent: the same string always returns the same
//     SymbolId.
//   - The table is concurrent-read safe; writes are synchronized with a
//     single mutex on the slow path. Hot paths only read.
//   - SymbolId NULL_SYMBOL (0) is reserved.
//   - Returned string_views remain valid for the process lifetime
//     (B19 fix: previously a vector reallocation could dangle them).
//
// Edge cases:
//   - Empty string is interned separately (returns EMPTY_SYMBOL).
//   - Strings longer than MAX_SYMBOL_LENGTH are rejected with an error
//     (Rule 63: resilient to malformed input).
//
// Cross-references:
//   - LAWS.md Rule 16 (Interned Symbols)
//   - LAWS.md Rule 125 (allowed global state: interned symbol tables)
//   - LAWS.md Rule 61 (hot-path: no allocations; lookup is read-only)
//   - LAWS.md Rule 17 (use cache-friendly hash maps on hot paths)
//   - LAWS.md Rule 59 (no linear search where O(1) is feasible)

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/common/result.hpp"
#include "core/common/types.hpp"

namespace omni::common {

/// Maximum symbol length. Long identifiers are usually a frontend bug;
/// we cap at 4096 to keep allocations bounded (Rule 23).
constexpr unsigned MAX_SYMBOL_LENGTH = 4096;

/// Reserved SymbolId for the empty string (the first interned symbol).
constexpr SymbolId EMPTY_SYMBOL = 1;

class SymbolTable {
public:
    static SymbolTable& instance() {
        static SymbolTable table;
        return table;
    }

    /// Intern a string. Returns the existing SymbolId if the string is
    /// already present, or allocates a new one. Thread-safe.
    [[nodiscard]] Result<SymbolId> intern(std::string_view s) {
        if (s.size() > MAX_SYMBOL_LENGTH) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, NULL_SYMBOL);
        }
        if (s.empty()) {
            return EMPTY_SYMBOL;
        }
        {
            std::shared_lock lock(mutex_);
            if (auto id = lookup_locked(s); id.has_value()) {
                return *id;
            }
        }
        std::unique_lock lock(mutex_);
        // Re-check under exclusive lock (double-checked locking).
        if (auto id = lookup_locked(s); id.has_value()) {
            return *id;
        }
        // B19 fix: store the string in a stable arena (std::deque) whose
        // prior allocations do not move on growth. The returned
        // string_view remains valid for the process lifetime.
        // We use a deque-of-chunks approach: allocate a single string per
        // entry; deque guarantees stable element addresses across push_back.
        // B20 fix: maintain a hash index for O(1) lookup instead of the
        // previous O(n) linear scan.
        std::string owned{s};
        std::string_view stable_view{owned};
        SymbolId id = next_id_.fetch_add(1, std::memory_order_relaxed);
        strings_.push_back(std::move(owned));
        // Reconstruct the view into the deque-stored string.
        stable_view = std::string_view{strings_.back()};
        index_.emplace(stable_view, id);
        return id;
    }

    /// Resolve a SymbolId back to its string. Thread-safe.
    /// Returns std::string_view{} if the id is unknown.
    /// The returned view remains valid for the process lifetime (B19 fix).
    [[nodiscard]] std::string_view resolve(SymbolId id) const noexcept {
        if (id == NULL_SYMBOL) return {};
        std::shared_lock lock(mutex_);
        // EMPTY_SYMBOL is stored at index 0 in strings_.
        const size_t idx = static_cast<size_t>(id) - 1;
        if (idx >= strings_.size()) return {};
        return std::string_view{strings_[idx]};
    }

    /// Number of interned symbols. Used by the verifier and telemetry.
    [[nodiscard]] SymbolId size() const noexcept {
        return next_id_.load(std::memory_order_acquire);
    }

private:
    SymbolTable() {
        // Pre-intern the empty string (EMPTY_SYMBOL = 1).
        strings_.push_back(std::string{});
        index_.emplace(std::string_view{strings_.back()}, EMPTY_SYMBOL);
        next_id_.store(EMPTY_SYMBOL + 1, std::memory_order_release);
    }

    /// Caller must hold mutex_ (shared or exclusive).
    [[nodiscard]] std::optional<SymbolId> lookup_locked(std::string_view s) const {
        auto it = index_.find(s);
        if (it == index_.end()) return std::nullopt;
        return it->second;
    }

    /// Stable storage: std::deque guarantees that prior element addresses
    /// are stable across push_back (B19 fix). We use a vector-of-strings
    /// where each string owns its own buffer; the vector-of-strings
    /// reallocation does NOT invalidate the strings' internal buffers
    /// because std::string's heap allocation is independent of the
    /// vector's storage. The view we return points into the string's
    /// internal buffer, which is stable as long as the string object
    /// itself is not moved or destroyed — and the vector never moves a
    /// string after it has been constructed in place (we use push_back
    /// of an rvalue, which constructs in place at the new back).
    ///
    /// HOWEVER, vector reallocation DOES move the string objects
    /// themselves (move-constructing them at the new location), which
    /// invalidates views into the old string objects. To be safe, we
    /// use std::deque here instead, whose elements are NOT moved on
    /// push_back.
    std::deque<std::string> strings_;

    /// Hash index for O(1) lookup (B20 fix). std::unordered_map is
    /// acceptable here because the symbol table is on the cold path
    /// (frontend / module load). The hot path (IR / passes) only uses
    /// SymbolId, never the map.
    std::unordered_map<std::string_view, SymbolId> index_;

    mutable std::shared_mutex mutex_;
    std::atomic<SymbolId> next_id_{1};
};

/// Convenience: intern a string literal at call time.
[[nodiscard]] inline Result<SymbolId> intern_symbol(std::string_view s) {
    return SymbolTable::instance().intern(s);
}

/// Convenience: resolve a SymbolId to a string_view.
/// The returned view is stable for the process lifetime.
[[nodiscard]] inline std::string_view resolve_symbol(SymbolId id) {
    return SymbolTable::instance().resolve(id);
}

}  // namespace omni::common

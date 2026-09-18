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
//
// Edge cases:
//   - Empty string is interned separately (returns a specific id, not 0).
//   - Strings longer than MAX_SYMBOL_LENGTH are rejected with an error
//     (Rule 63: resilient to malformed input).
//
// Cross-references:
//   - LAWS.md Rule 16 (Interned Symbols)
//   - LAWS.md Rule 125 (allowed global state: interned symbol tables)
//   - LAWS.md Rule 61 (hot-path: no allocations; lookup is read-only)

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
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
        if (auto id = lookup_locked(s); id.has_value()) {
            return *id;
        }
        // Allocate a new id and copy the string.
        SymbolId id = next_id_.fetch_add(1, std::memory_order_relaxed);
        entries_.push_back(Entry{std::string{s}, id});
        return id;
    }

    /// Resolve a SymbolId back to its string. Thread-safe.
    /// Returns std::string_view{} if the id is unknown.
    [[nodiscard]] std::string_view resolve(SymbolId id) const {
        if (id == NULL_SYMBOL) return {};
        std::shared_lock lock(mutex_);
        if (id >= next_id_.load(std::memory_order_acquire)) return {};
        return entries_[id - 1].text;
    }

    /// Number of interned symbols. Used by the verifier and telemetry.
    [[nodiscard]] SymbolId size() const noexcept {
        return next_id_.load(std::memory_order_acquire);
    }

private:
    SymbolTable() {
        // Pre-intern the empty symbol.
        entries_.push_back(Entry{std::string{}, EMPTY_SYMBOL});
        next_id_.store(EMPTY_SYMBOL + 1, std::memory_order_release);
    }

    std::optional<SymbolId> lookup_locked(std::string_view s) const {
        for (const auto& e : entries_) {
            if (e.text == s) return e.id;
        }
        return std::nullopt;
    }

    struct Entry {
        std::string text;
        SymbolId id;
    };

    mutable std::shared_mutex mutex_;
    std::vector<Entry> entries_;
    std::atomic<SymbolId> next_id_{1};
};

/// Convenience: intern a string literal at call time.
[[nodiscard]] inline Result<SymbolId> intern_symbol(std::string_view s) {
    return SymbolTable::instance().intern(s);
}

/// Convenience: resolve a SymbolId to a string_view.
[[nodiscard]] inline std::string_view resolve_symbol(SymbolId id) {
    return SymbolTable::instance().resolve(id);
}

}  // namespace omni::common

// core/object_model/shape_registry.hpp
//
// Global shape registry.
//
// Purpose:
//   Implements a minimal shape registry for Tier 0. The full shape-transition
//   tree (DESIGN.md §3.6) is the long-term design; this registry is a
//   pragmatic stand-in that lets the interpreter associate objects with
//   shape descriptors (slot_count + name→slot_index map) so that inline
//   caches can key on (shape_id, shape_version).
//
//   Shapes returned by the registry are immutable and live for the process
//   lifetime. ShapeId values are stable. ShapeVersion starts at
//   INITIAL_SHAPE_VERSION and bumps only when a transition is recorded
//   (which the full shape-transition engine would do; for now we treat
//   each registry-created shape as a leaf with no further transitions).
//
// Invariants:
//   - Thread-safe: lookup and creation use a shared mutex.
//   - ShapeId values are assigned monotonically starting at 1.
//     ShapeId 0 is NULL_SHAPE (the empty shape: zero slots, no properties).
//   - Two shapes with the same (slot_count, ordered property-name list)
//     are the same shape (deduplicated by the registry).
//   - The registry is one of the allowed global state holders
//     (Laws Rule 125).
//
// Cross-references:
//   - DESIGN.md §3.2 (OmniShape)
//   - DESIGN.md §3.6 (shape transitions — future work)
//   - LAWS.md Rule 71 (specialization requires versioned deps)
//   - LAWS.md Rule 95 (shape mutation must invalidate)
//   - LAWS.md Rule 125 (allowed global state)

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/common/result.hpp"
#include "core/common/symbol_table.hpp"
#include "core/common/types.hpp"
#include "core/object_model/omni_shape.hpp"

namespace omni::object_model {

/// A shape registry key: (slot_count, ordered property-name symbol ids).
/// Property names are stored in insertion order; two shapes with the same
/// slot_count and same ordered names are the same shape.
struct ShapeKey {
    uint32_t slot_count;
    std::vector<common::SymbolId> property_names;  // in slot order

    bool operator==(const ShapeKey& other) const noexcept {
        return slot_count == other.slot_count
            && property_names == other.property_names;
    }
};

struct ShapeKeyHash {
    size_t operator()(const ShapeKey& k) const noexcept {
        // FNV-1a 64-bit. Stable across runs (no randomization — Rule 79).
        uint64_t h = 1469598103934665603ull;
        h ^= k.slot_count;
        h *= 1099511628211ull;
        for (auto s : k.property_names) {
            h ^= s;
            h *= 1099511628211ull;
        }
        return static_cast<size_t>(h);
    }
};

class ShapeRegistry {
public:
    static ShapeRegistry& instance() {
        static ShapeRegistry r;
        return r;
    }

    /// Look up (or create) a shape with the given slot count and property
    /// name→slot-index mapping. The property_names vector must be in
    /// slot-index order: property_names[i] is the name of slot i.
    /// Returns the shape pointer (owned by the registry; live for the
    /// process lifetime) and its ShapeId.
    [[nodiscard]] OmniShape* intern(uint32_t slot_count,
                                     std::vector<common::SymbolId> property_names) {
        ShapeKey key{slot_count, std::move(property_names)};
        {
            std::shared_lock lock(mutex_);
            auto it = by_key_.find(key);
            if (it != by_key_.end()) return it->second;
        }
        std::unique_lock lock(mutex_);
        // Re-check under exclusive lock.
        auto it = by_key_.find(key);
        if (it != by_key_.end()) return it->second;

        const common::ShapeId id = next_id_.fetch_add(1, std::memory_order_relaxed);
        auto shape = std::make_unique<OmniShape>(id, common::INITIAL_SHAPE_VERSION,
                                                   LayoutKind::FixedStruct);
        // Populate the shape's property entries.
        for (uint32_t i = 0; i < key.property_names.size() && i < key.slot_count; ++i) {
            PropertyEntry pe;
            pe.name = key.property_names[i];
            pe.slot_index = i;
            pe.offset = i;
            pe.is_accessor = false;
            pe.getter = nullptr;
            pe.setter = nullptr;
            shape->mutable_properties().push_back(pe);
        }
        OmniShape* raw = shape.get();
        by_key_.emplace(std::move(key), raw);
        by_id_.push_back(std::move(shape));
        return raw;
    }

    /// Convenience: look up a shape by id. Returns nullptr if unknown.
    [[nodiscard]] OmniShape* lookup(common::ShapeId id) const noexcept {
        std::shared_lock lock(mutex_);
        const size_t idx = static_cast<size_t>(id) - 1;
        if (idx >= by_id_.size()) return nullptr;
        return by_id_[idx].get();
    }

    /// Find the slot index for a property name within a shape. Returns
    /// common::INVALID_PC (cast to uint32_t) if not found. (We avoid
    /// introducing a new sentinel here; INVALID_PC is a uint32_t max value
    /// which is also a usable "not found" marker for slot indices since
    /// slot_count is bounded by MAX_BYTECODE_LENGTH.)
    [[nodiscard]] static uint32_t find_slot(const OmniShape& shape,
                                              common::SymbolId name) noexcept {
        for (const auto& pe : shape.properties()) {
            if (pe.name == name) return pe.slot_index;
        }
        return common::INVALID_PC;
    }

    /// Number of shapes in the registry (telemetry).
    [[nodiscard]] common::ShapeId size() const noexcept {
        return next_id_.load(std::memory_order_acquire);
    }

private:
    ShapeRegistry() = default;

    mutable std::shared_mutex mutex_;
    std::unordered_map<ShapeKey, OmniShape*, ShapeKeyHash> by_key_;
    std::vector<std::unique_ptr<OmniShape>> by_id_;
    std::atomic<common::ShapeId> next_id_{1};
};

}  // namespace omni::object_model

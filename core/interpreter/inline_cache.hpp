// core/interpreter/inline_cache.hpp
//
// Inline caches for property access, method calls, and operator dispatch.
//
// Purpose:
//   Implements DESIGN.md §5.6: IC kinds (monomorphic, polymorphic,
//   megamorphic, prototype-chain, trait-capability, operator-pair,
//   coercion, iterator) and IC uses (property access, method call,
//   operator dispatch, iterator protocol, coercion, pattern matching).
//
// Invariants:
//   - An IC entry stores a (shape_id, shape_version, offset/method) tuple
//     keyed on (pc, accessed_symbol).
//   - Monomorphic ICs are stored inline in the SiteProfile (one slot).
//   - Polymorphic ICs (up to IC_POLYMORPHIC_CAPACITY entries) are stored
//     in the SiteProfile's poly_entries vector.
//   - Megamorphic ICs fall back to a dictionary lookup on the shape's
//     property table; no per-site storage is used.
//   - IC updates are atomic under the no-GIL model (DESIGN.md §5.8):
//     updates use CAS on a 64-bit packed slot.
//
// Edge cases:
//   - IC miss triggers a fallback to the generic handler (Rule 96).
//   - If the shape version changes (rule 95: trait mutation, etc.), the
//     IC entry is invalidated.
//   - Megamorphic ICs may transition back to polymorphic if the shape
//     churn settles (but this is rare and opt-in via telemetry).
//
// Cross-references:
//   - DESIGN.md §5.6 (IC kinds and uses)
//   - DESIGN.md §5.8 (no-GIL rules: IC tables lock-free or sharded)
//   - LAWS.md Rule 71 (specialization requires versioned deps)
//   - LAWS.md Rule 95 (shape/trait mutation must invalidate)
//   - LAWS.md Rule 118 (no GIL on hot paths; IC updates are lock-free)

#pragma once

#include <atomic>
#include <cstdint>

#include "core/common/result.hpp"
#include "core/common/types.hpp"
#include "core/object_model/object.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::interpreter {

enum class ICKind : uint8_t {
    Monomorphic      = 0,
    Polymorphic      = 1,
    Megamorphic      = 2,
    PrototypeChain   = 3,
    TraitCapability  = 4,
    OperatorPair     = 5,
    Coercion         = 6,
    Iterator         = 7,
};

/// Packed 64-bit IC slot for monomorphic caches.
/// Layout:
///   bits  0..31  shape_id
///   bits 32..47  shape_version (low 16 bits; high bits in epoch)
///   bits 48..63  offset (byte offset of the field in the payload, or
///                slot index for FixedStruct)
struct ICSlot {
    std::atomic<uint64_t> packed{0};

    constexpr ICSlot() = default;

    void store(common::ShapeId sid, common::ShapeVersion sv, uint16_t offset) noexcept {
        uint64_t v = uint64_t{sid}
                   | (uint64_t{sv & 0xFFFFu} << 32)
                   | (uint64_t{offset} << 48);
        packed.store(v, std::memory_order_release);
    }
    [[nodiscard]] uint64_t load() const noexcept {
        return packed.load(std::memory_order_acquire);
    }
    [[nodiscard]] static common::ShapeId shape_id_of(uint64_t v) noexcept {
        return static_cast<common::ShapeId>(v & 0xFFFFFFFFu);
    }
    [[nodiscard]] static common::ShapeVersion version_of(uint64_t v) noexcept {
        return static_cast<common::ShapeVersion>((v >> 32) & 0xFFFFu);
    }
    [[nodiscard]] static uint16_t offset_of(uint64_t v) noexcept {
        return static_cast<uint16_t>(v >> 48);
    }
    /// Returns true if the slot's shape id matches the given one.
    [[nodiscard]] bool matches(common::ShapeId sid) const noexcept {
        return shape_id_of(load()) == sid;
    }
};

/// Per-site IC state. Holds the monomorphic slot plus a pointer to the
/// polymorphic entries (which live in the SiteProfile).
class InlineCache {
public:
    InlineCache() = default;

    /// Try a monomorphic property-access lookup. Returns the value if
    /// the cache hits; returns nullopt (ic_miss) otherwise.
    [[nodiscard]] std::optional<object_model::TaggedValue>
    try_monomorphic_property(const object_model::Object& obj,
                              common::SymbolId /*prop_name*/) const noexcept {
        const uint64_t v = slot_.load();
        if (v == 0) return std::nullopt;
        const auto sid = ICSlot::shape_id_of(v);
        const auto* shape = obj.shape();
        if (!shape || shape->shape_id() != sid) return std::nullopt;
        // Hit: read the slot at the cached offset.
        const uint16_t offset = ICSlot::offset_of(v);
        const auto& pl = obj.payload;
        // For FixedStruct: offset is a slot index. We assume FixedStruct
        // here; the runtime checks layout_kind() before calling.
        if (offset >= pl.fixed_struct.slot_count) return std::nullopt;
        return pl.fixed_struct.slots[offset];
    }

    /// Record a monomorphic property-access observation. Called on IC miss
    /// after the slow path has resolved the property.
    void record_monomorphic_property(common::ShapeId sid,
                                      common::ShapeVersion sv,
                                      uint16_t offset) noexcept {
        slot_.store(sid, sv, offset);
    }

    /// Invalidate the cache. Called by the dependency system when a shape
    /// version bumps (Rule 95).
    void invalidate() noexcept {
        slot_.packed.store(0, std::memory_order_release);
    }

private:
    ICSlot slot_;
};

}  // namespace omni::interpreter

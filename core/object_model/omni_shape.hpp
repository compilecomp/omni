// core/object_model/omni_shape.hpp
//
// OmniShape descriptor.
//
// Purpose:
//   Implements DESIGN.md §3.2: an OmniShape describes physical layout,
//   dynamic traits, protocol capabilities, method resolution, transition
//   behavior, synchronization policy, GC scanning behavior, and debugging
//   metadata for a class of objects.
//
// Invariants:
//   - Shapes are immutable once published. A new shape version is created
//     on every transition (DESIGN.md §3.6).
//   - shape_id is stable across versions of the same lineage.
//   - shape_version is monotonically increasing per shape_id.
//   - epoch is a global monotonic counter; bumps on any transition.
//
// Edge cases:
//   - Multiple shapes may share a shape_id (different versions). The
//     (shape_id, shape_version) pair is the unique key.
//   - VirtualShape instances live only in the compiler; the runtime never
//     sees them.
//
// Cross-references:
//   - DESIGN.md §3.2 (OmniShape descriptor fields)
//   - DESIGN.md §3.6 (shape transitions)
//   - DESIGN.md §3.8 (stability score)
//   - LAWS.md Rule 71 (specialization requires versioned, invalidatable deps)
//   - LAWS.md Rule 95 (shape/trait mutation must invalidate code)

#pragma once

#include <atomic>
#include <cstdint>

#include "core/common/flags.hpp"
#include "core/common/small_vector.hpp"
#include "core/common/types.hpp"
#include "core/object_model/capability_bits.hpp"
#include "core/object_model/layout_kind.hpp"

namespace omni::object_model {

/// Method resolution entry. Trait methods override prototype methods
/// by appearing later in the method table.
struct MethodEntry {
    common::SymbolId name;        // interned method name
    common::SymbolId trait_id;     // owning trait (or 0 for prototype)
    uint32_t trait_version;        // version of the trait at insertion
    /// Function object pointer. May be null during shape construction.
    void* function_ptr;
};

/// Property entry. Fields may be slots (for FixedStruct) or dictionary
/// entries (for FlexibleDictionary).
struct PropertyEntry {
    common::SymbolId name;
    uint32_t slot_index;        // for FixedStruct
    uint32_t offset;            // byte offset in payload (for DenseArray etc)
    bool is_accessor;           // getter/setter pair vs. plain data
    void* getter;
    void* setter;
};

/// Transition edge in the shape tree. (from_shape, action, key) -> to_shape.
struct TransitionEdge {
    enum class Action : uint8_t {
        AddField         = 1,
        RemoveField      = 2,
        RenameField      = 3,
        ChangeFieldType  = 4,
        AddTrait         = 5,
        RemoveTrait      = 6,
        OverrideMethod   = 7,
        ChangePrototype  = 8,
        MorphToClass     = 9,
        Freeze           = 10,
        Seal             = 11,
        Share            = 12,
        Asyncify         = 13,
        Dispose          = 14,
    };

    Action action;
    common::SymbolId key;        // field name, trait id, etc.
    uint32_t to_shape_version;   // new shape_version after this transition
};

/// Stability score for a shape lineage (DESIGN.md §3.8).
/// Stored as fixed-point fractions over 1024 for cheap comparison.
struct ShapeStability {
    uint32_t transitions_per_second_q8 = 0;  // transitions/sec << 8
    uint32_t trait_injection_rate_q8   = 0;
    uint32_t method_override_rate_q8    = 0;
    uint32_t field_churn_q8             = 0;  // add+remove per second << 8
    uint32_t polymorphism_level         = 0;  // 0..1024 (0 = mono, 1024 = mega)
    uint32_t operator_cache_hit_rate    = 0;  // 0..1024
    uint32_t deopt_rate_q8             = 0;  // deopts/sec << 8

    /// Composite stability score in [0, 1024]. Higher is more stable.
    /// Used as the T2->T3 promotion criterion (DESIGN.md tier ladder).
    [[nodiscard]] uint32_t score() const noexcept {
        // Inverse-weighted: more churn = lower score.
        uint32_t churn = transitions_per_second_q8 + trait_injection_rate_q8
                       + method_override_rate_q8 + field_churn_q8 + deopt_rate_q8;
        // Saturating subtract; clamp at 0.
        uint32_t s = churn > 1024u << 8 ? 0 : (1024u << 8) - churn;
        return s >> 8;  // back to [0, 1024]
    }
};

class OmniShape {
public:
    OmniShape(common::ShapeId id, common::ShapeVersion version, LayoutKind layout)
        : shape_id_(id), shape_version_(version), layout_kind_(layout) {}

    // --- Identity ---
    [[nodiscard]] common::ShapeId shape_id() const noexcept { return shape_id_; }
    [[nodiscard]] common::ShapeVersion shape_version() const noexcept { return shape_version_; }
    [[nodiscard]] common::Epoch epoch() const noexcept {
        return epoch_.load(std::memory_order_acquire);
    }

    // --- Layout ---
    [[nodiscard]] LayoutKind layout_kind() const noexcept { return layout_kind_; }

    // --- Capabilities ---
    [[nodiscard]] CapabilityBits capabilities() const noexcept { return capabilities_; }
    [[nodiscard]] bool has(Capability c) const noexcept { return capabilities_.has(c); }

    // --- Methods / properties ---
    [[nodiscard]] const common::SmallVector<MethodEntry, 4>& methods() const noexcept {
        return methods_;
    }
    [[nodiscard]] const common::SmallVector<PropertyEntry, 4>& properties() const noexcept {
        return properties_;
    }

    // --- Transitions ---
    [[nodiscard]] const common::SmallVector<TransitionEdge, 2>& transitions() const noexcept {
        return transitions_;
    }

    // --- Stability ---
    [[nodiscard]] ShapeStability stability() const noexcept { return stability_; }
    void set_stability(ShapeStability s) noexcept { stability_ = s; }

    // --- Bumping the epoch (called by the dependency system on transition) ---
    void bump_epoch() noexcept {
        epoch_.fetch_add(1, std::memory_order_acq_rel);
    }

private:
    common::ShapeId shape_id_;
    common::ShapeVersion shape_version_;
    LayoutKind layout_kind_;
    std::atomic<common::Epoch> epoch_{common::INITIAL_EPOCH};

    CapabilityBits capabilities_{};
    common::SmallVector<MethodEntry, 4> methods_{};
    common::SmallVector<PropertyEntry, 4> properties_{};
    common::SmallVector<TransitionEdge, 2> transitions_{};
    ShapeStability stability_{};
};

}  // namespace omni::object_model

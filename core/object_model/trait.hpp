// core/object_model/trait.hpp
//
// Trait — injectable behavior unit.
//
// Purpose:
//   Implements DESIGN.md §3.5: traits provide methods, properties, and
//   required/provided capabilities. Trait injection creates a new shape
//   version: old_shape + trait -> new_shape.
//
// Invariants:
//   - trait_id is stable; trait_version bumps when the trait's methods
//     or properties change.
//   - Conflict policy determines what happens when injected traits
//     provide overlapping methods.
//   - Trait injection is atomic at the shape level (Rule 110).
//
// Edge cases:
//   - Injecting the same trait twice into the same shape is a no-op
//     (idempotent) but still bumps the shape version.
//   - Removing a trait that is required by another injected trait
//     fails with a ConflictError.
//
// Cross-references:
//   - DESIGN.md §3.5 (Trait structure)
//   - DESIGN.md §3.6 (AddTrait transition)
//   - LAWS.md Rule 70 (dynamic features are first-class correctness)
//   - LAWS.md Rule 95 (trait mutation must invalidate code)

#pragma once

#include <cstdint>

#include "core/common/flags.hpp"
#include "core/common/small_vector.hpp"
#include "core/common/types.hpp"
#include "core/object_model/capability_bits.hpp"

namespace omni::object_model {

/// What to do when injected traits provide overlapping methods.
enum class TraitConflictPolicy : uint8_t {
    /// First-in-wins: earlier trait's method takes precedence.
    FirstWins   = 0,
    /// Last-in-wins: later trait's method takes precedence.
    LastWins    = 1,
    /// Reject the injection with a conflict error.
    Reject      = 2,
    /// Compose: wrap both methods in a chained super-call.
    Compose     = 3,
};

/// Trait method descriptor.
struct TraitMethod {
    common::SymbolId name;       // method name (interned)
    common::SymbolId provides;   // capability this method satisfies (or 0)
    void* function_ptr;
    uint32_t version;
};

/// Trait property descriptor.
struct TraitProperty {
    common::SymbolId name;
    common::SymbolId provides;    // capability this property satisfies
    uint32_t slot_index;         // assigned slot in the host shape
    uint32_t version;
};

class Trait {
public:
    Trait(common::SymbolId id, common::SymbolId trait_name, TraitConflictPolicy policy)
        : trait_id_(id), trait_name_(trait_name), conflict_policy_(policy) {}

    [[nodiscard]] common::SymbolId trait_id() const noexcept { return trait_id_; }
    [[nodiscard]] common::SymbolId trait_name() const noexcept { return trait_name_; }
    [[nodiscard]] TraitConflictPolicy conflict_policy() const noexcept { return conflict_policy_; }

    void add_method(TraitMethod m) { methods_.push_back(m); }
    void add_property(TraitProperty p) { properties_.push_back(p); }

    void add_required_capability(Capability c) { required_caps_.set(c); }
    void add_provided_capability(Capability c) { provided_caps_.set(c); }

    [[nodiscard]] const common::SmallVector<TraitMethod, 4>& methods() const noexcept {
        return methods_;
    }
    [[nodiscard]] const common::SmallVector<TraitProperty, 4>& properties() const noexcept {
        return properties_;
    }
    [[nodiscard]] CapabilityBits required_capabilities() const noexcept { return required_caps_; }
    [[nodiscard]] CapabilityBits provided_capabilities() const noexcept { return provided_caps_; }

private:
    common::SymbolId trait_id_;
    common::SymbolId trait_name_;
    TraitConflictPolicy conflict_policy_;
    common::SmallVector<TraitMethod, 4> methods_{};
    common::SmallVector<TraitProperty, 4> properties_{};
    CapabilityBits required_caps_{};
    CapabilityBits provided_caps_{};
};

}  // namespace omni::object_model

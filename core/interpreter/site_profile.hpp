// core/interpreter/site_profile.hpp
//
// Site profile metadata for one bytecode site.
//
// Purpose:
//   Implements DESIGN.md §5.3: every bytecode site has a SiteProfile that
//   records type feedback, call targets, branch bias, and other profiling
//   state used by adaptive quickening and the JIT tiers.
//
// Invariants:
//   - SiteProfile is owned by the InterpFrame, not the BytecodeModule.
//     Multiple frames executing the same module may have their own
//     profile snapshots (DESIGN.md §5.8: "running tasks may temporarily
//     use old quickened state").
//   - State transitions are one-way (cold -> generic -> quickened ->
//     polymorphic -> megamorphic -> disabled), except for the disabled
//     state which is terminal (DESIGN.md §5.3 states list).
//   - Access to a SiteProfile is by single-writer (the executing thread).
//     Cross-thread invalidation bumps the shape epoch atomically and
//     the executing thread observes the bump at its next safepoint
//     (DESIGN.md §5.8).
//
// Edge cases:
//   - A site that has never executed has counter == 0 and state == Generic.
//   - A site that has deopted may be permanently Disabled.
//   - Polymorphic ICs have up to IC_POLYMORPHIC_CAPACITY entries before
//     transitioning to megamorphic (which falls back to a dictionary lookup).
//
// Cross-references:
//   - DESIGN.md §5.3 (SiteProfile fields and states)
//   - DESIGN.md §5.4 (adaptive quickening rules)
//   - DESIGN.md §5.6 (IC kinds)
//   - LAWS.md Rule 44 (no profile data without confidence)
//   - LAWS.md Rule 71 (specialization requires versioned deps)

#pragma once

#include <atomic>
#include <cstdint>

#include "core/common/flags.hpp"
#include "core/common/small_vector.hpp"
#include "core/common/types.hpp"
#include "core/object_model/capability_bits.hpp"

namespace omni::interpreter {

enum class SiteState : uint8_t {
    Generic      = 0,
    Quickened    = 1,
    Polymorphic  = 2,
    Megamorphic  = 3,
    Fused        = 4,
    Disabled     = 5,
};

/// Branch-bias accumulator. Tracks how often a branch was taken vs not.
/// Uses saturating arithmetic to avoid pathological overflow (Rule 114).
struct BranchBias {
    uint32_t taken_count{0};
    uint32_t not_taken_count{0};

    void record_taken() noexcept {
        // Saturating add: never wrap past UINT32_MAX (Rule 114).
        if (taken_count < UINT32_MAX) ++taken_count;
    }
    void record_not_taken() noexcept {
        if (not_taken_count < UINT32_MAX) ++not_taken_count;
    }
    [[nodiscard]] uint32_t total() const noexcept {
        // Saturating add: never overflow past UINT32_MAX (Rule 114, B29 fix).
        uint64_t t = uint64_t{taken_count} + uint64_t{not_taken_count};
        return t > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(t);
    }
    /// Bias as numerator over BIAS_RESOLUTION (fixed-point, 0..1024).
    [[nodiscard]] uint32_t bias_taken_q10() const noexcept {
        uint32_t t = total();
        if (t < common::BRANCH_BIAS_SAMPLE_MIN) return 0;
        return static_cast<uint32_t>((uint64_t{taken_count} << common::BIAS_RESOLUTION_BITS) / t);
    }
};

struct SiteProfile {
    common::BytecodePC pc{common::INVALID_PC};
    std::atomic<uint32_t> counter{0};
    SiteState state{SiteState::Generic};

    // Last observed shape (for monomorphic IC).
    common::ShapeId shape_id{common::NULL_SHAPE};
    common::ShapeVersion shape_version{common::INITIAL_SHAPE_VERSION};
    common::Epoch shape_epoch{common::INITIAL_EPOCH};

    // Type feedback. Tag bits record which Tag types have been seen.
    // We use a bit per tag (8 bits for 8 possible Tag values).
    uint8_t type_feedback_bits{0};

    // Polymorphic IC entries.
    struct PolyEntry {
        common::ShapeId shape_id;
        common::ShapeVersion shape_version;
        uint32_t hit_count;
    };
    common::SmallVector<PolyEntry, common::IC_POLYMORPHIC_CAPACITY> poly_entries{};

    // Call targets.
    struct CallTarget {
        uint32_t module_id;
        uint32_t function_index;
        uint32_t hit_count;
    };
    common::SmallVector<CallTarget, common::IC_POLYMORPHIC_CAPACITY> call_targets{};

    // Branch bias.
    BranchBias branch_bias{};

    // Failure count (IC misses, guard failures).
    std::atomic<uint32_t> failure_count{0};

    // Fusion group id; 0 means not fused.
    uint32_t fusion_group{0};

    // Dependency keys for this site (Rule 71).
    // Sparse representation: a small inline vector of dep keys.
    struct DepKey {
        common::SymbolId entity;       // shape/trait/global id
        common::Epoch version;
    };
    common::SmallVector<DepKey, 4> dependency_keys{};

    /// Atomics in this struct prevent implicit move/copy. We provide an
    /// explicit move constructor that copies the atomic values via
    /// load/store (relaxed is safe: the source is being destroyed).
    SiteProfile() = default;
    SiteProfile(const SiteProfile&) = delete;
    SiteProfile& operator=(const SiteProfile&) = delete;
    SiteProfile(SiteProfile&& other) noexcept
        : pc(other.pc),
          counter(other.counter.load(std::memory_order_relaxed)),
          state(other.state),
          shape_id(other.shape_id),
          shape_version(other.shape_version),
          shape_epoch(other.shape_epoch),
          type_feedback_bits(other.type_feedback_bits),
          poly_entries(std::move(other.poly_entries)),
          call_targets(std::move(other.call_targets)),
          branch_bias(other.branch_bias),
          failure_count(other.failure_count.load(std::memory_order_relaxed)),
          fusion_group(other.fusion_group),
          dependency_keys(std::move(other.dependency_keys)) {}
    SiteProfile& operator=(SiteProfile&& other) noexcept {
        if (this != &other) {
            pc = other.pc;
            counter.store(other.counter.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
            state = other.state;
            shape_id = other.shape_id;
            shape_version = other.shape_version;
            shape_epoch = other.shape_epoch;
            type_feedback_bits = other.type_feedback_bits;
            poly_entries = std::move(other.poly_entries);
            call_targets = std::move(other.call_targets);
            branch_bias = other.branch_bias;
            failure_count.store(other.failure_count.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
            fusion_group = other.fusion_group;
            dependency_keys = std::move(other.dependency_keys);
        }
        return *this;
    }

    /// Record type feedback for a single TaggedValue's tag.
    void record_type_tag(uint8_t tag_bit) noexcept {
        type_feedback_bits |= tag_bit;
    }

    /// Check if a site is hot enough to quicken (DESIGN.md §5.4).
    [[nodiscard]] bool is_hot() const noexcept {
        return counter.load(std::memory_order_relaxed) >= common::SITE_HOT_THRESHOLD;
    }

    /// Check if a site is monomorphic (exactly one shape observed).
    /// An empty cache is NOT monomorphic (B17 fix: don't quicken
    /// unprofiled sites — that would install GetPropMono with no
    /// shape data, leading to a guaranteed IC miss).
    [[nodiscard]] bool is_monomorphic() const noexcept {
        return poly_entries.size() == common::IC_MONOMORPHIC_CAPACITY;
    }

    /// Check if a site is polymorphic but not yet megamorphic.
    [[nodiscard]] bool is_polymorphic() const noexcept {
        return poly_entries.size() > common::IC_MONOMORPHIC_CAPACITY
            && poly_entries.size() <= common::IC_POLYMORPHIC_CAPACITY;
    }

    /// Increment the failure counter; if it exceeds the threshold,
    /// disable the site. Uses the Tier-0-specific threshold (B28 fix),
    /// not the Tier-2 blacklist threshold.
    void record_failure() noexcept {
        uint32_t f = failure_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (f >= common::T0_SITE_DISABLE_FAILURE_THRESHOLD
            && state != SiteState::Disabled) {
            state = SiteState::Disabled;
        }
    }
};

}  // namespace omni::interpreter

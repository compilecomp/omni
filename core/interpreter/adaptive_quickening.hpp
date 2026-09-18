// core/interpreter/adaptive_quickening.hpp
//
// Adaptive quickening engine.
//
// Purpose:
//   Implements DESIGN.md §5.4: hot, monomorphic sites are quickened; hot,
//   polymorphic sites use inline caches; unstable sites stay generic or
//   are disabled.
//
//   The quickening engine runs at safepoints (DESIGN.md §5.8) and on
//   Tier 0 -> Tier 1 transitions. It examines each SiteProfile in the
//   current frame and rewrites the bytecode at that site to use the
//   quickened form when conditions are met.
//
// Invariants:
//   - Quickening is atomic at the site level (DESIGN.md §5.8: "quickening
//     overlay atomically updated"). Other threads may observe the old
//     instruction until they next reload the module's instruction stream;
//     they will see the new instruction at the next safepoint.
//   - Quickened opcodes always have a fallback to the semantic opcode
//     (DESIGN.md §4.2: "Quickened bytecode is speculative. It must
//     always be able to fall back to semantic bytecode.").
//   - A site that has been Disabled (DESIGN.md §5.3 states) is never
//     re-quickenened.
//
// Cross-references:
//   - DESIGN.md §5.3 (SiteProfile states)
//   - DESIGN.md §5.4 (quickening rules)
//   - LAWS.md Rule 42 (no assumption without invalidation)
//   - LAWS.md Rule 43 (no specialization without fallback)
//   - LAWS.md Rule 44 (no profile data without confidence)
//   - LAWS.md Rule 71 (specialization requires versioned deps)

#pragma once

#include "core/bytecode/bytecode_module.hpp"
#include "core/common/result.hpp"
#include "core/interpreter/interp_frame.hpp"
#include "core/interpreter/interpreter.hpp"
#include "core/interpreter/site_profile.hpp"

namespace omni::interpreter {

class AdaptiveQuickening {
public:
    /// Visit all SiteProfiles in the frame and attempt to quicken hot,
    /// monomorphic sites. Called at safepoints.
    ///
    /// Returns the number of sites quickened (for telemetry).
    static uint32_t visit_frame(InterpFrame& frame,
                                 bytecode::BytecodeModule& module,
                                 Interpreter& interp) noexcept;

    /// Quickening rule for a GET_PROP site (DESIGN.md §5.4 example).
    /// Rewrites GET_PROP -> GET_PROP_MONO with the observed shape id,
    /// field offset, and shape version.
    static bool try_quicken_get_prop(SiteProfile& profile,
                                       bytecode::BytecodeModule& module) noexcept;

    /// Quickening rule for an ADD site.
    /// Rewrites ADD -> ADD_INT_FAST, ADD_FLOAT_FAST, or ADD_STRING_CONCAT_FAST
    /// based on the observed type feedback.
    static bool try_quicken_add(SiteProfile& profile,
                                  bytecode::BytecodeModule& module) noexcept;

    /// Quickening rule for a BRANCH site.
    /// Rewrites BRANCH -> BRANCH_TAKEN_FAST or BRANCH_NOT_TAKEN_FAST
    /// if the branch bias exceeds the threshold (BRANCH_BIAS_NUM/DEN).
    static bool try_quicken_branch(SiteProfile& profile,
                                    bytecode::BytecodeModule& module) noexcept;

    /// Quickening rule for a CALL site.
    /// Rewrites CALL -> CALL_MONO if the call target is monomorphic.
    static bool try_quicken_call(SiteProfile& profile,
                                   bytecode::BytecodeModule& module) noexcept;

    /// Demote a quickened site back to its semantic form. Called when
    /// a guard fails repeatedly (Rule 84: deopt loops must be throttled).
    static void demote_to_semantic(SiteProfile& profile,
                                    bytecode::BytecodeModule& module) noexcept;

private:
    AdaptiveQuickening() = delete;
};

}  // namespace omni::interpreter

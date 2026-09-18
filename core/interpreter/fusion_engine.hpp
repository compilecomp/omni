// core/interpreter/fusion_engine.hpp
//
// Bytecode fusion engine — combines hot sequences into superinstructions.
//
// Purpose:
//   Implements DESIGN.md §5.7: hot sequences are fused. Fusion is
//   conservative: it never crosses arbitrary calls, await points,
//   sync exits, exception handlers, dynamic eval, trait injection,
//   class mutation, or task spawn (DESIGN.md §5.7 "Do not initially
//   fuse across").
//
//   Each fused opcode carries a FusedOp metadata record that records
//   the original PCs, subop references, guard info, profile, dependency
//   keys, fallback PC, debug info, and GC map. This allows the runtime
//   to deopt back to the original bytecode sequence (Rule 75: frames
//   reconstructible on demand).
//
// Invariants:
//   - Fusion runs at safepoints, after adaptive quickening.
//   - A fused opcode is only installed when ALL of the following hold:
//       1. The sequence is hot (counter >= SITE_HOT_THRESHOLD for every
//          site in the sequence).
//       2. Side effects are closed (no observable effect between subops
//          that would be lost by fusion).
//       3. No arbitrary call in the middle (calls may deopt; we can't
//          fuse across them in T0).
//       4. No exception edge in the middle (try_begin/try_end are
//          sequence boundaries).
//       5. No GC-sensitive publication in the middle (allocation alone
//          is fine; publishing a reference to a new object via a store
//          to a global is not).
//       6. Branch behavior is simple (no conditional jumps inside the
//          fused sequence).
//       7. All subops have compatible fallback PCs (for deopt).
//
// Cross-references:
//   - DESIGN.md §5.7 (fusion conditions, fused op metadata)
//   - DESIGN.md §4.3 (fused opcodes: AddIntRR, GetPropAddIntConst, etc.)
//   - LAWS.md Rule 42 (no assumption without invalidation)
//   - LAWS.md Rule 43 (no specialization without fallback)
//   - LAWS.md Rule 75 (frames reconstructible on demand)

#pragma once

#include "core/bytecode/bytecode_module.hpp"
#include "core/common/result.hpp"
#include "core/common/small_vector.hpp"
#include "core/common/types.hpp"
#include "core/interpreter/interp_frame.hpp"
#include "core/interpreter/interpreter.hpp"

namespace omni::interpreter {

/// Metadata for a fused opcode. Stored in a side table keyed by (pc, opcode).
/// DESIGN.md §5.7 FusedOp fields.
struct FusedOp {
    uint32_t fused_id;                                 // unique id
    common::SmallVector<common::BytecodePC, 4> original_pcs;  // for deopt
    common::SmallVector<bytecode::Opcode, 4> subop_refs;
    uint32_t guard_info;                                // packed guard bits
    uint32_t profile_handle;                            // index into profile table
    common::SmallVector<uint32_t, 4> dependency_keys;
    common::BytecodePC fallback_pc;                     // first subop's pc
    common::SymbolId debug_info;                         // source-location symbol
    uint64_t gc_map_low;                                // GC map for the fused sequence
    uint64_t gc_map_high;
};

class FusionEngine {
public:
    /// Visit all SiteProfiles in the frame and attempt to fuse hot
    /// sequences. Returns the number of fused sequences.
    static uint32_t visit_frame(InterpFrame& frame,
                                  bytecode::BytecodeModule& module,
                                  Interpreter& interp) noexcept;

    /// Try to fuse a LOAD_LOCAL + ADD + STORE_LOCAL sequence into LOAD_ADD_STORE.
    /// DESIGN.md §4.3.
    static bool try_fuse_load_add_store(bytecode::BytecodeModule& module,
                                          common::BytecodePC pc) noexcept;

    /// Try to fuse a GET_PROP + ADD (int) + const into GET_PROP_ADD_INT_CONST.
    static bool try_fuse_get_prop_add_int_const(bytecode::BytecodeModule& module,
                                                  common::BytecodePC pc) noexcept;

    /// Try to fuse a GET_PROP + CALL_MONO into GET_PROP_CALL_MONO.
    static bool try_fuse_get_prop_call_mono(bytecode::BytecodeModule& module,
                                              common::BytecodePC pc) noexcept;

    /// Try to fuse an ITER_NEXT + BRANCH into ITER_NEXT_BRANCH.
    static bool try_fuse_iter_next_branch(bytecode::BytecodeModule& module,
                                            common::BytecodePC pc) noexcept;

    /// Check if a sequence is fusible at the given pc.
    /// Implements the conditions in DESIGN.md §5.7.
    [[nodiscard]] static bool is_fusible(bytecode::BytecodeModule& module,
                                            common::BytecodePC pc,
                                            unsigned sequence_length) noexcept;

    /// Register a FusedOp in the side table. Returns the fused_id.
    uint32_t register_fused_op(FusedOp op) {
        uint32_t id = static_cast<uint32_t>(fused_ops_.size());
        fused_ops_.push_back(std::move(op));
        return id;
    }
    [[nodiscard]] const FusedOp& get_fused_op(uint32_t id) const noexcept {
        return fused_ops_[id];
    }

private:
    std::vector<FusedOp> fused_ops_;
};

}  // namespace omni::interpreter

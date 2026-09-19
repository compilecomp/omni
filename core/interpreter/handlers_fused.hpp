// core/interpreter/handlers_fused.hpp
//
// Handlers for fused (superinstruction) opcodes (DESIGN.md §4.3).
//
// Purpose:
//   Each fused opcode combines a hot sequence of 2-3 semantic opcodes
//   into a single instruction. The fused handler performs the combined
//   operation and advances the pc past all absorbed slots (which are
//   marked Opcode::Nop in the instruction stream).
//
//   Each fused opcode has an associated FusedOp metadata record (B12 fix)
//   that stores the original PCs, subop refs, and fallback information
//   so that deopt can reconstruct the original execution state (Rule 75).
//
// Cross-references:
//   - DESIGN.md §4.3 (fused opcodes)
//   - DESIGN.md §5.7 (fusion engine, FusedOp metadata)
//   - LAWS.md Rule 75 (frames reconstructible on demand)

#pragma once

#include "core/interpreter/dispatch.hpp"
#include "core/interpreter/interp_frame.hpp"
#include "core/interpreter/interpreter.hpp"

namespace omni::interpreter {

namespace handlers_fused {

/// Register all fused handlers with the interpreter's dispatch table.
/// Called from Interpreter::Interpreter().
void register_all(Interpreter& interp) noexcept;

}  // namespace handlers_fused

}  // namespace omni::interpreter

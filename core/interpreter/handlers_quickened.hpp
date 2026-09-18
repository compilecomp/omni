// core/interpreter/handlers_quickened.hpp
//
// Handlers for quickened opcodes (DESIGN.md §4.2).
//
// Purpose:
//   Each quickened opcode is a speculative specialization of a semantic
//   opcode. It includes a guard that checks the assumption; on guard
//   failure it falls back to the semantic handler (Rule 96: Tier 0
//   universal fallback) and records a failure (Rule 84: deopt loops
//   must be throttled).
//
// Cross-references:
//   - DESIGN.md §4.2 (quickened opcode list)
//   - DESIGN.md §5.4 (adaptive quickening rules)
//   - DESIGN.md §5.5 (speculative arithmetic)
//   - LAWS.md Rule 43 (no specialization without fallback)
//   - LAWS.md Rule 96 (Tier 0 universal fallback)

#pragma once

#include "core/interpreter/dispatch.hpp"
#include "core/interpreter/interp_frame.hpp"
#include "core/interpreter/interpreter.hpp"

namespace omni::interpreter {

namespace handlers_quickened {

/// Register all quickened handlers with the interpreter's dispatch table.
/// Called from Interpreter::Interpreter().
void register_all(Interpreter& interp) noexcept;

}  // namespace handlers_quickened

}  // namespace omni::interpreter

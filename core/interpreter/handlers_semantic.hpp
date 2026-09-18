// core/interpreter/handlers_semantic.hpp
//
// Handlers for semantic opcodes (DESIGN.md §4.1).
//
// Purpose:
//   Implements the Tier 0 behavior for every semantic opcode. Each handler
//   is a free function with the Handler signature
//   `void(InterpFrame&, Interpreter&)`. They are registered with the
//   Interpreter's semantic dispatch table at construction.
//
//   Per the "each pass gets its own file" rule, these handlers live in
//   their own translation unit(s). Quickened handlers and fused handlers
//   have their own files (handlers_quickened.{hpp,cpp},
//   handlers_fused.{hpp,cpp}).
//
// Invariants:
//   - Each handler reads its operands from the current instruction at
//     frame.pc() and advances the pc by 1 (for compact instructions) or
//     by 2 (for extended instructions).
//   - Handlers never throw (Laws Rule 61: no exceptions in hot path).
//     Errors are recorded via frame.set_exception() and propagate via
//     a control-flow opcode (Raise) at the next opportunity.
//   - Handlers may allocate (for MakeObject, MakeClosure, etc.) on the
//     slow path; allocation failures are recorded as exceptions.
//
// Cross-references:
//   - DESIGN.md §4.1 (semantic opcode list)
//   - DESIGN.md §5.1 (interpreter frame, register file)
//   - DESIGN.md §5.2 (dispatch model)
//   - LAWS.md Rule 61 (no allocations/exceptions in hot path)
//   - LAWS.md Rule 90 (recursion limits)
//   - LAWS.md Rule 96 (Tier 0 universal fallback)

#pragma once

#include "core/interpreter/dispatch.hpp"
#include "core/interpreter/interp_frame.hpp"
#include "core/interpreter/interpreter.hpp"

namespace omni::interpreter {

namespace handlers_semantic {

/// Register all semantic handlers with the interpreter's dispatch table.
void register_all(Interpreter& interp) noexcept;

// Individual handlers (declared here, defined in .cpp). Each is public
// for testing.
void handle_load_arg(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_load_const(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_load_local(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_store_local(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_get_prop(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_set_prop(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_add(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_sub(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_mul(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_div(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_mod(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_eq(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_lt(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_gt(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_le(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_ge(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_ne(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_call(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_jump(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_branch(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_return(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_make_object(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_make_closure(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_get_iter(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_next(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_spawn(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_await(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_sync_enter(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_sync_exit(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_chan_send(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_chan_recv(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_raise(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_try_begin(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_try_end(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_match(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_using(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_defer(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_dup(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_pop(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_is_null(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_is_int(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_is_float(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_is_str(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_is_object(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_coerce(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_inject_trait(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_remove_trait(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_get_field(InterpFrame& frame, Interpreter& interp) noexcept;
void handle_set_field(InterpFrame& frame, Interpreter& interp) noexcept;

}  // namespace handlers_semantic

}  // namespace omni::interpreter
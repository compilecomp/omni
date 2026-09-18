// core/interpreter/handlers_semantic.cpp
//
// Implementation of all semantic opcode handlers.
//
// Each handler reads its operands from the current instruction at
// frame.pc(), performs the operation, and advances the pc by 1 (or 2
// for extended instructions).
//
// Error convention: handlers do not throw. On error they store an
// exception value in the frame and jump to the nearest handler entry.
// If no handler is registered, the exception propagates to the caller
// via the Interpreter::execute return value (Rule 96).

#include "core/interpreter/handlers_semantic.hpp"

#include "core/bytecode/instruction.hpp"
#include "core/bytecode/opcode.hpp"
#include "core/common/result.hpp"
#include "core/common/types.hpp"
#include "core/object_model/object.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::interpreter {

using namespace bytecode;
using namespace common;
using namespace object_model;
namespace hs = handlers_semantic;

namespace handlers_semantic {

// --- Helper: read current instruction and advance pc ---
[[nodiscard]] static inline Instruction current_inst(const InterpFrame& frame,
                                                       const Interpreter& interp) noexcept {
    // We assume the caller has the module loaded; for the prototype we
    // store the code inline by reading it from the interpreter.
    // In a production version this would go through Interpreter's
    // module registry to avoid the indirection.
    (void)interp;
    // For now we return a placeholder; the actual implementation
    // retrieves the instruction from the current BytecodeModule.
    // See Interpreter::execute for the real dispatch path.
    Instruction i{};
    (void)frame;
    return i;
}

// --- Load / store ---

void handle_load_arg(InterpFrame& frame, Interpreter& interp) noexcept {
    // LOAD_ARG rdst, arg_idx
    // Copies the arg_idx-th argument from the frame's arg registers into
    // rdst. Args are pre-loaded into registers 0..param_count-1 by
    // Interpreter::execute, so LOAD_ARG is effectively a register copy.
    (void)interp;
    // Real impl: read operands from the instruction at frame.pc().
    frame.advance_pc();
}

void handle_load_const(InterpFrame& frame, Interpreter& interp) noexcept {
    // LOAD_CONST rdst, const_idx
    // Loads a constant from the module's constant pool.
    (void)interp;
    frame.advance_pc();
}

void handle_load_local(InterpFrame& frame, Interpreter& interp) noexcept {
    // LOAD_LOCAL rdst, src_reg
    // Copies one register to another (locals are stored in registers
    // in the Tier 0 model; LOAD_LOCAL_FAST is the quickened version
    // that skips the indirection).
    (void)interp;
    frame.advance_pc();
}

void handle_store_local(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

// --- Property access ---

void handle_get_prop(InterpFrame& frame, Interpreter& interp) noexcept {
    // GET_PROP rdst, obj_reg, prop_name_symbol
    // The full implementation:
    //   1. Load obj_reg from frame.
    //   2. If obj is not ObjectRef, fall back to coercion (Rule 70).
    //   3. Look up the property in obj.shape->properties().
    //   4. If not found, fall back to the prototype chain or raise
    //      AttributeError.
    //   5. Update the SiteProfile's IC (monomorphic slot).
    // For now this is a stub that advances the pc.
    (void)interp;
    frame.advance_pc();
}

void handle_set_prop(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

void handle_get_field(InterpFrame& frame, Interpreter& interp) noexcept {
    // GET_FIELD rdst, obj_reg, slot_idx
    // Direct slot access; skips shape check (caller has already verified).
    (void)interp;
    frame.advance_pc();
}

void handle_set_field(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

// --- Arithmetic ---

void handle_add(InterpFrame& frame, Interpreter& interp) noexcept {
    // ADD rdst, rsrc1, rsrc2
    // Speculative arithmetic (DESIGN.md §5.5): if both operands are int,
    // use int_add; if both float, use float_add; if both str, concat;
    // otherwise dispatch through the operator overload (CALLABLE trait
    // with name "add").
    //
    // Overflow: int_add returns an overflow flag; on overflow we fall
    // back to BigInt (Laws Rule 72: numeric semantics preserved).
    (void)interp;
    frame.advance_pc();
}

void handle_sub(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_mul(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_div(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_mod(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }

// --- Comparison ---

void handle_eq(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_lt(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_gt(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_le(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_ge(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_ne(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }

// --- Control flow ---

void handle_call(InterpFrame& frame, Interpreter& interp) noexcept {
    // CALL rdst, fn_reg, arg_count, [args...]
    // Recursive call into Interpreter::execute for the callee.
    // Checks the recursion limit (Rule 90) before recursing.
    (void)interp;
    frame.advance_pc();
}

void handle_jump(InterpFrame& frame, Interpreter& interp) noexcept {
    // JUMP target
    // Sets pc = pc + delta (delta from operand_ab).
    (void)interp;
    frame.advance_pc();  // actual impl will set pc = pc + delta
}

void handle_branch(InterpFrame& frame, Interpreter& interp) noexcept {
    // BRANCH cond_reg, target
    // If cond_reg is truthy, jump to target; else fall through.
    // Records branch bias in the SiteProfile (DESIGN.md §5.3).
    (void)interp;
    frame.advance_pc();
}

void handle_return(InterpFrame& frame, Interpreter& interp) noexcept {
    // RETURN rsrc
    // Copies rsrc into register 0 (return value convention) and ends
    // execution of this frame. The Interpreter::execute loop checks
    // for a return sentinel after each handler call.
    (void)interp;
    frame.advance_pc();
}

// --- Object construction ---

void handle_make_object(InterpFrame& frame, Interpreter& interp) noexcept {
    // MAKE_OBJECT rdst, shape_id
    // Allocates a new object with the given shape. Allocation fast path
    // must handle failure (Rule 91).
    (void)interp;
    frame.advance_pc();
}

void handle_make_closure(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

// --- Iteration ---

void handle_get_iter(InterpFrame& frame, Interpreter& interp) noexcept {
    // GET_ITER rdst, src_reg
    // Checks src_reg's shape for the ITERABLE capability. If present,
    // returns an iterator object. Otherwise raises TypeError.
    (void)interp;
    frame.advance_pc();
}

void handle_next(InterpFrame& frame, Interpreter& interp) noexcept {
    // NEXT rdst, iter_reg
    // Calls the iterator's next trait. Returns the next value or signals
    // StopIteration (via exception state).
    (void)interp;
    frame.advance_pc();
}

// --- Concurrency (Rule 118: no GIL) ---

void handle_spawn(InterpFrame& frame, Interpreter& interp) noexcept {
    // SPAWN rdst, fn_reg, arg_count, [args...]
    // Schedules a new M:N task on the runtime's task scheduler. The
    // current frame does not block; the new task runs concurrently.
    (void)interp;
    frame.advance_pc();
}

void handle_await(InterpFrame& frame, Interpreter& interp) noexcept {
    // AWAIT rdst, awaitable_reg
    // Suspends the current task until the awaitable resolves. The
    // frame is heap-allocated (Rule 76) and resumed by the scheduler.
    // This is a suspension point: valid deopt and safepoint candidate.
    (void)interp;
    frame.advance_pc();
}

void handle_sync_enter(InterpFrame& frame, Interpreter& interp) noexcept {
    // SYNC_ENTER lock_obj_reg
    // Acquires the thin lock on the object. Pushes a SyncEntry onto
    // the frame's sync_stack for cleanup on exception/return (Rule 118).
    (void)interp;
    frame.advance_pc();
}

void handle_sync_exit(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

void handle_chan_send(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

void handle_chan_recv(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

// --- Exception handling ---

void handle_raise(InterpFrame& frame, Interpreter& interp) noexcept {
    // RAISE exc_reg
    // Sets frame.exception to the value in exc_reg and jumps to the
    // nearest handler entry. If no handler, the exception propagates
    // to the caller.
    (void)interp;
    frame.advance_pc();
}

void handle_try_begin(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

void handle_try_end(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

// --- Pattern matching ---

void handle_match(InterpFrame& frame, Interpreter& interp) noexcept {
    // MATCH rdst, src_reg, pattern_id
    // Destructures src_reg against the pattern. On success, binds
    // variables into the frame's registers and jumps to the pattern's
    // body. On failure, falls through to the next pattern.
    (void)interp;
    frame.advance_pc();
}

// --- Resource management ---

void handle_using(InterpFrame& frame, Interpreter& interp) noexcept {
    // USING rsrc
    // Enters a deterministic resource scope. The object's dispose trait
    // will be called on scope exit (normal or exceptional).
    (void)interp;
    frame.advance_pc();
}

void handle_defer(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

// --- Stack manipulation ---

void handle_dup(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_pop(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }

// --- Type checks ---

void handle_is_null(InterpFrame& frame, Interpreter& interp) noexcept {
    // IS_NULL rdst, rsrc
    // Sets rdst to true if rsrc is the Null tag, false otherwise.
    (void)interp;
    frame.advance_pc();
}

void handle_is_int(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_is_float(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_is_str(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }
void handle_is_object(InterpFrame& frame, Interpreter& interp) noexcept { (void)interp; frame.advance_pc(); }

// --- Coercion / morphing ---

void handle_coerce(InterpFrame& frame, Interpreter& interp) noexcept {
    // COERCE rdst, rsrc, target_type
    // Implicit morphing (DESIGN.md language feature §1). Looks up the
    // to_int / to_float / to_str / to_bool trait on the source's shape
    // and invokes it. Falls back to TypeError if no conversion exists.
    (void)interp;
    frame.advance_pc();
}

// --- Trait injection ---

void handle_inject_trait(InterpFrame& frame, Interpreter& interp) noexcept {
    // INJECT_TRAIT obj_reg, trait_id
    // Transitions obj's shape: old_shape + trait -> new_shape. Bumps the
    // shape epoch (Rule 95). Invalidates dependent ICs at the next
    // safepoint.
    (void)interp;
    frame.advance_pc();
}

void handle_remove_trait(InterpFrame& frame, Interpreter& interp) noexcept {
    (void)interp;
    frame.advance_pc();
}

// --- Registration ---

void register_all(Interpreter& interp) noexcept {
    interp.register_handler(Opcode::LoadArg,        handle_load_arg);
    interp.register_handler(Opcode::LoadConst,      handle_load_const);
    interp.register_handler(Opcode::LoadLocal,      handle_load_local);
    interp.register_handler(Opcode::StoreLocal,     handle_store_local);
    interp.register_handler(Opcode::GetProp,        handle_get_prop);
    interp.register_handler(Opcode::SetProp,        handle_set_prop);
    interp.register_handler(Opcode::Add,            handle_add);
    interp.register_handler(Opcode::Sub,            handle_sub);
    interp.register_handler(Opcode::Mul,            handle_mul);
    interp.register_handler(Opcode::Div,            handle_div);
    interp.register_handler(Opcode::Mod,            handle_mod);
    interp.register_handler(Opcode::Eq,             handle_eq);
    interp.register_handler(Opcode::Lt,             handle_lt);
    interp.register_handler(Opcode::Gt,             handle_gt);
    interp.register_handler(Opcode::Le,             handle_le);
    interp.register_handler(Opcode::Ge,             handle_ge);
    interp.register_handler(Opcode::Ne,             handle_ne);
    interp.register_handler(Opcode::Call,           handle_call);
    interp.register_handler(Opcode::Jump,           handle_jump);
    interp.register_handler(Opcode::Branch,        handle_branch);
    interp.register_handler(Opcode::Return,         handle_return);
    interp.register_handler(Opcode::MakeObject,    handle_make_object);
    interp.register_handler(Opcode::MakeClosure,    handle_make_closure);
    interp.register_handler(Opcode::GetIter,        handle_get_iter);
    interp.register_handler(Opcode::Next,           handle_next);
    interp.register_handler(Opcode::Spawn,          handle_spawn);
    interp.register_handler(Opcode::Await,          handle_await);
    interp.register_handler(Opcode::SyncEnter,     handle_sync_enter);
    interp.register_handler(Opcode::SyncExit,       handle_sync_exit);
    interp.register_handler(Opcode::ChanSend,       handle_chan_send);
    interp.register_handler(Opcode::ChanRecv,       handle_chan_recv);
    interp.register_handler(Opcode::Raise,          handle_raise);
    interp.register_handler(Opcode::TryBegin,       handle_try_begin);
    interp.register_handler(Opcode::TryEnd,         handle_try_end);
    interp.register_handler(Opcode::Match,          handle_match);
    interp.register_handler(Opcode::Using,          handle_using);
    interp.register_handler(Opcode::Defer,          handle_defer);
    interp.register_handler(Opcode::Dup,            handle_dup);
    interp.register_handler(Opcode::Pop,            handle_pop);
    interp.register_handler(Opcode::IsNull,         handle_is_null);
    interp.register_handler(Opcode::IsInt,          handle_is_int);
    interp.register_handler(Opcode::IsFloat,       handle_is_float);
    interp.register_handler(Opcode::IsStr,          handle_is_str);
    interp.register_handler(Opcode::IsObject,       handle_is_object);
    interp.register_handler(Opcode::Coerce,         handle_coerce);
    interp.register_handler(Opcode::InjectTrait,    handle_inject_trait);
    interp.register_handler(Opcode::RemoveTrait,   handle_remove_trait);
    interp.register_handler(Opcode::GetField,       handle_get_field);
    interp.register_handler(Opcode::SetField,       handle_set_field);
}

}  // namespace handlers_semantic

}  // namespace omni::interpreter

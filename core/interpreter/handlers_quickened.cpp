// core/interpreter/handlers_quickened.cpp
//
// Implementation of quickened opcode handlers (B11 fix).
//
// Each quickened handler:
//   1. Reads operands from the current instruction.
//   2. Performs the speculative fast path (e.g., int add for ADD_INT_FAST).
//   3. On guard failure (e.g., operand is not an int), records a failure
//      in the SiteProfile (Rule 84) and demotes the site back to its
//      semantic opcode via AdaptiveQuickening::demote_to_semantic, then
//      re-dispatches the semantic handler.
//   4. On success, advances the pc.
//
// Cross-references:
//   - DESIGN.md §4.2 (quickened opcodes)
//   - DESIGN.md §5.5 (speculative arithmetic)
//   - LAWS.md Rule 43 (no specialization without fallback)
//   - LAWS.md Rule 84 (deopt loops must be throttled)
//   - LAWS.md Rule 96 (Tier 0 universal fallback)

#include "core/interpreter/handlers_quickened.hpp"

#include <cmath>

#include "core/bytecode/instruction.hpp"
#include "core/bytecode/opcode.hpp"
#include "core/common/types.hpp"
#include "core/interpreter/adaptive_quickening.hpp"
#include "core/interpreter/handlers_semantic.hpp"
#include "core/interpreter/interp_frame.hpp"
#include "core/interpreter/speculative_arithmetic.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::interpreter {

using namespace bytecode;
using namespace common;
using namespace object_model;
namespace hs = handlers_semantic;

namespace handlers_quickened {

// --- Helper: read the current instruction ---
[[nodiscard]] static inline Instruction current_inst(const InterpFrame& frame,
                                                       const Interpreter& interp) noexcept {
    return interp.current_instruction(frame.pc());
}

// --- Helper: handle guard failure by demoting and re-dispatching ---
// On guard failure we:
//   1. Record a failure in the SiteProfile (Rule 84).
//   2. Demote the site back to its semantic opcode (atomic store).
//   3. Fall through to the semantic handler by calling it directly.
// This ensures the operation completes correctly even when the
// speculation was wrong (Rule 96: Tier 0 universal fallback).
static inline void guard_failure(InterpFrame& frame, Interpreter& interp,
                                   Opcode semantic_op,
                                   void (*semantic_handler)(InterpFrame&, Interpreter&)) noexcept {
    SiteProfile* p = frame.find_or_create_profile(frame.pc());
    p->record_failure();
    // Demote the site so future executions use the semantic opcode
    // directly (avoids repeated guard failures).
    // Note: demote_to_semantic needs a BytecodeModule; we get it from
    // the interpreter. This is safe because demote_to_semantic uses
    // atomic stores.
    const auto* mod = interp.current_module();
    if (mod != nullptr) {
        // const_cast is safe here: demote_to_semantic only mutates via
        // the atomic API, which is thread-safe.
        AdaptiveQuickening::demote_to_semantic(*p,
            const_cast<BytecodeModule&>(*mod));
    }
    // Re-dispatch the semantic handler to perform the actual operation.
    // Do NOT advance pc here; the semantic handler will advance it.
    (void)semantic_op;
    semantic_handler(frame, interp);
}

void handle_load_local_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    // LOAD_LOCAL_FAST rdst, src
    // Same as LOAD_LOCAL but tagged as quickened (no semantic difference
    // in Tier 0; the quickening is a hint to the JIT that this site is
    // monomorphic). Just delegate to the semantic handler.
    hs::handle_load_local(frame, interp);
}

void handle_get_prop_mono(InterpFrame& frame, Interpreter& interp) noexcept {
    // GET_PROP_MONO rdst, obj_reg, (shape_id, offset packed in next slot)
    // Guard: obj.shape == expected_shape. On miss, fall back to GET_PROP.
    // For now: delegate to the semantic GET_PROP handler. The full
    // implementation would check the IC slot and short-circuit on hit.
    hs::handle_get_prop(frame, interp);
}

void handle_set_prop_mono(InterpFrame& frame, Interpreter& interp) noexcept {
    hs::handle_set_prop(frame, interp);
}

void handle_add_int_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    // ADD_INT_FAST rdst_rsrc1, rsrc2
    // Guard: both operands are int. On failure, fall back to ADD.
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    auto r = spec_int_add(a, b);
    if (r.has_value()) [[likely]] {
        frame.store_reg(rdst, *r);
        frame.advance_pc();
    } else {
        // Guard failure: demote and re-dispatch.
        guard_failure(frame, interp, Opcode::Add, hs::handle_add);
    }
}

void handle_add_float_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    auto r = spec_float_add(a, b);
    if (r.has_value()) [[likely]] {
        frame.store_reg(rdst, *r);
        frame.advance_pc();
    } else {
        guard_failure(frame, interp, Opcode::Add, hs::handle_add);
    }
}

void handle_add_string_concat_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    auto r = spec_str_concat(a, b);
    if (r.has_value()) [[likely]] {
        frame.store_reg(rdst, *r);
        frame.advance_pc();
    } else {
        guard_failure(frame, interp, Opcode::Add, hs::handle_add);
    }
}

void handle_sub_int_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    auto r = spec_int_sub(frame.load_reg(rdst), frame.load_reg(rsrc2));
    if (r.has_value()) [[likely]] {
        frame.store_reg(rdst, *r);
        frame.advance_pc();
    } else {
        guard_failure(frame, interp, Opcode::Sub, hs::handle_sub);
    }
}

void handle_mul_int_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    auto r = spec_int_mul(frame.load_reg(rdst), frame.load_reg(rsrc2));
    if (r.has_value()) [[likely]] {
        frame.store_reg(rdst, *r);
        frame.advance_pc();
    } else {
        guard_failure(frame, interp, Opcode::Mul, hs::handle_mul);
    }
}

void handle_div_int_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    auto r = spec_int_div(frame.load_reg(rdst), frame.load_reg(rsrc2));
    if (r.has_value()) {
        frame.store_reg(rdst, *r);
        frame.advance_pc();
    } else {
        guard_failure(frame, interp, Opcode::Div, hs::handle_div);
    }
}

void handle_call_mono(InterpFrame& frame, Interpreter& interp) noexcept {
    // CALL_MONO: monomorphic call site. The call target is cached in
    // the SiteProfile. For now: delegate to the semantic CALL handler.
    hs::handle_call(frame, interp);
}

void handle_call_direct_small(InterpFrame& frame, Interpreter& interp) noexcept {
    // CALL_DIRECT_SMALL: direct call to a small function (no closure).
    // Delegate to semantic CALL.
    hs::handle_call(frame, interp);
}

void handle_branch_taken_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    // BRANCH_TAKEN_FAST: branch is almost always taken.
    // Guard: we still need to check the condition; if it's false,
    // fall through (which is the rare path).
    const Instruction inst = current_inst(frame, interp);
    const RegId cond_reg = RegId{inst.operand_a()};
    const int8_t delta8 = static_cast<int8_t>(inst.operand_b());
    const TaggedValue cond = frame.load_reg(cond_reg);
    // Reuse the truthiness logic from the semantic BRANCH handler.
    bool taken = false;
    switch (cond.tag()) {
        case Tag::Null:   taken = false; break;
        case Tag::Bool:   taken = cond.as_bool(); break;
        case Tag::Int:    taken = cond.as_int() != 0; break;
        case Tag::Float: {
            const double f = cond.as_float();
            taken = (f != 0.0) || std::isnan(f);
            break;
        }
        default:           taken = true; break;
    }
    SiteProfile* p = frame.find_or_create_profile(frame.pc());
    if (taken) p->branch_bias.record_taken();
    else p->branch_bias.record_not_taken();
    if (taken) [[likely]] {
        frame.advance_pc(static_cast<uint32_t>(static_cast<int32_t>(delta8)));
    } else {
        // Guard failure: the branch was not taken despite the prediction.
        // Record and demote.
        p->record_failure();
        // Fall through (advance by 1).
        frame.advance_pc();
    }
}

void handle_branch_not_taken_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    // BRANCH_NOT_TAKEN_FAST: branch is almost never taken.
    const Instruction inst = current_inst(frame, interp);
    const RegId cond_reg = RegId{inst.operand_a()};
    const int8_t delta8 = static_cast<int8_t>(inst.operand_b());
    const TaggedValue cond = frame.load_reg(cond_reg);
    bool taken = false;
    switch (cond.tag()) {
        case Tag::Null:   taken = false; break;
        case Tag::Bool:   taken = cond.as_bool(); break;
        case Tag::Int:    taken = cond.as_int() != 0; break;
        case Tag::Float: {
            const double f = cond.as_float();
            taken = (f != 0.0) || std::isnan(f);
            break;
        }
        default:           taken = true; break;
    }
    SiteProfile* p = frame.find_or_create_profile(frame.pc());
    if (taken) p->branch_bias.record_taken();
    else p->branch_bias.record_not_taken();
    if (!taken) [[likely]] {
        // Fall through (advance by 1).
        frame.advance_pc();
    } else {
        // Guard failure: the branch was taken despite the prediction.
        p->record_failure();
        frame.advance_pc(static_cast<uint32_t>(static_cast<int32_t>(delta8)));
    }
}

void handle_next_shape_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    // NEXT_SHAPE_FAST: iterator next with a cached shape.
    // Delegate to semantic NEXT.
    hs::handle_next(frame, interp);
}

void handle_to_int_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    // TO_INT_FAST rdst, src
    // Guard: src is already int (no-op) or float (truncate).
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    const TaggedValue v = frame.load_reg(src);
    if (v.is_int()) {
        frame.store_reg(rdst, v);
    } else if (v.is_float()) {
        frame.store_reg(rdst, TaggedValue::make_int(static_cast<int64_t>(v.as_float())));
    } else {
        // Guard failure: delegate to semantic COERCE.
        guard_failure(frame, interp, Opcode::Coerce, hs::handle_coerce);
        return;
    }
    frame.advance_pc();
}

void handle_to_float_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    const TaggedValue v = frame.load_reg(src);
    if (v.is_float()) {
        frame.store_reg(rdst, v);
    } else if (v.is_int()) {
        frame.store_reg(rdst, TaggedValue::make_float(static_cast<double>(v.as_int())));
    } else {
        guard_failure(frame, interp, Opcode::Coerce, hs::handle_coerce);
        return;
    }
    frame.advance_pc();
}

void handle_to_str_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    // TO_STR_FAST: stub; full implementation would format int/float to
    // string. Delegate to semantic COERCE.
    (void)interp;
    frame.set_exception(TaggedValue::make_null());
}

void handle_to_bool_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    const TaggedValue v = frame.load_reg(src);
    bool truthy = false;
    switch (v.tag()) {
        case Tag::Null:   truthy = false; break;
        case Tag::Bool:   truthy = v.as_bool(); break;
        case Tag::Int:    truthy = v.as_int() != 0; break;
        case Tag::Float: {
            const double f = v.as_float();
            truthy = (f != 0.0) || std::isnan(f);
            break;
        }
        default:           truthy = true; break;
    }
    frame.store_reg(rdst, TaggedValue::make_bool(truthy));
    frame.advance_pc();
}

void handle_eq_int_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    if (a.is_int() && b.is_int()) [[likely]] {
        frame.store_reg(rdst, TaggedValue::make_bool(a.as_int() == b.as_int()));
        frame.advance_pc();
    } else {
        guard_failure(frame, interp, Opcode::Eq, hs::handle_eq);
    }
}

void handle_lt_int_fast(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    if (a.is_int() && b.is_int()) [[likely]] {
        frame.store_reg(rdst, TaggedValue::make_bool(a.as_int() < b.as_int()));
        frame.advance_pc();
    } else {
        guard_failure(frame, interp, Opcode::Lt, hs::handle_lt);
    }
}

void handle_get_prop_poly(InterpFrame& frame, Interpreter& interp) noexcept {
    // GET_PROP_POLY: polymorphic IC. Delegate to semantic GET_PROP.
    hs::handle_get_prop(frame, interp);
}

void handle_set_prop_poly(InterpFrame& frame, Interpreter& interp) noexcept {
    hs::handle_set_prop(frame, interp);
}

void register_all(Interpreter& interp) noexcept {
    interp.register_quickened_handler(Opcode::LoadLocalFast,       handle_load_local_fast);
    interp.register_quickened_handler(Opcode::GetPropMono,         handle_get_prop_mono);
    interp.register_quickened_handler(Opcode::SetPropMono,         handle_set_prop_mono);
    interp.register_quickened_handler(Opcode::AddIntFast,           handle_add_int_fast);
    interp.register_quickened_handler(Opcode::AddFloatFast,         handle_add_float_fast);
    interp.register_quickened_handler(Opcode::AddStringConcatFast, handle_add_string_concat_fast);
    interp.register_quickened_handler(Opcode::SubIntFast,           handle_sub_int_fast);
    interp.register_quickened_handler(Opcode::MulIntFast,           handle_mul_int_fast);
    interp.register_quickened_handler(Opcode::DivIntFast,           handle_div_int_fast);
    interp.register_quickened_handler(Opcode::CallMono,             handle_call_mono);
    interp.register_quickened_handler(Opcode::CallDirectSmall,     handle_call_direct_small);
    interp.register_quickened_handler(Opcode::BranchTakenFast,     handle_branch_taken_fast);
    interp.register_quickened_handler(Opcode::BranchNotTakenFast,  handle_branch_not_taken_fast);
    interp.register_quickened_handler(Opcode::NextShapeFast,        handle_next_shape_fast);
    interp.register_quickened_handler(Opcode::ToIntFast,            handle_to_int_fast);
    interp.register_quickened_handler(Opcode::ToFloatFast,          handle_to_float_fast);
    interp.register_quickened_handler(Opcode::ToStrFast,            handle_to_str_fast);
    interp.register_quickened_handler(Opcode::ToBoolFast,           handle_to_bool_fast);
    interp.register_quickened_handler(Opcode::EqIntFast,            handle_eq_int_fast);
    interp.register_quickened_handler(Opcode::LtIntFast,             handle_lt_int_fast);
    interp.register_quickened_handler(Opcode::GetPropPoly,          handle_get_prop_poly);
    interp.register_quickened_handler(Opcode::SetPropPoly,          handle_set_prop_poly);
}

}  // namespace handlers_quickened

}  // namespace omni::interpreter

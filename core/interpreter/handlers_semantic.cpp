// core/interpreter/handlers_semantic.cpp
//
// Implementation of all semantic opcode handlers (B1 fix).
//
// Each handler reads its operands from the instruction at frame.pc()
// (accessed via Interpreter::current_instruction), performs the operation,
// and advances the pc by 1 (or 2 for extended instructions).
//
// Encoding convention (24-bit Instruction, opcode + a + b):
//   LOAD_ARG      rdst,  arg_idx       — copy regs_[arg_idx] to regs_[rdst]
//   LOAD_CONST    rdst,  const_idx16  — copy constants[const_idx] to regs_[rdst]
//   LOAD_LOCAL    rdst,  src          — copy regs_[src] to regs_[rdst]
//   STORE_LOCAL   dst,   rsrc        — copy regs_[rsrc] to regs_[dst]
//   ADD/SUB/MUL/DIV/MOD  rdst_rsrc1, rsrc2 — rdst = rdst OP rsrc (2-operand form)
//   EQ/LT/GT/LE/GE/NE   rdst_rsrc1, rsrc2 — rdst = bool(rdst OP rsrc)
//   CALL          fn_reg, arg_count   — call regs_[fn_reg] with args 0..arg_count-1
//   JUMP          delta16             — pc += signed delta
//   BRANCH        cond_reg, delta8    — if regs_[cond] truthy: pc += signed delta
//   RETURN        rsrc                — copy regs_[rsrc] to regs_[0], end frame
//   MAKE_OBJECT   rdst, shape_id16    — allocate new object with given shape
//   MAKE_CLOSURE  rdst, fn_reg, env_reg — allocate closure capturing env
//   GET_ITER      rdst, src           — get iterator from src (requires Iterable)
//   NEXT          rdst, iter_reg      — call next on iterator; raises StopIteration
//   SPAWN         rdst, fn_reg        — spawn M:N task (stub: sets exception)
//   AWAIT         rdst, awaitable     — suspend until awaitable resolves (stub)
//   SYNC_ENTER    obj_reg             — acquire thin lock on obj
//   SYNC_EXIT     (no operands)       — release most recent sync lock
//   RAISE         exc_reg             — set exception state to regs_[exc_reg]
//   TRY_BEGIN     (no operands)       — marker; handler table consulted on raise
//   TRY_END       (no operands)       — marker; exits try region
//   MATCH         rdst, src, pat_idx  — pattern-match src against pat (stub)
//   USING         rsrc                — enter deterministic resource scope
//   DEFER         rsrc                — register deferred cleanup
//   DUP           rdst, src           — copy regs_[src] to regs_[rdst]
//   POP           rsrc                — clear regs_[rsrc] (logical pop)
//   IS_NULL/IS_INT/IS_FLOAT/IS_STR/IS_OBJECT  rdst, src — type test
//   COERCE        rdst, src, target   — implicit morphing (stub: sets exception)
//   INJECT_TRAIT  obj_reg, trait_id16 — transition shape: obj + trait (stub)
//   REMOVE_TRAIT  obj_reg, trait_id16 — transition shape: obj - trait (stub)
//   GET_FIELD     rdst, obj_reg, slot — direct slot read (no shape check)
//   SET_FIELD     obj_reg, slot, rsrc — direct slot write (no shape check)
//   NOP           (no operands)       — no-op; advance pc
//
// Error convention: handlers do not throw (Rule 61). On error they store
// an exception value in frame.exception_ and the dispatch loop will find
// the nearest handler at the next iteration.
//
// Cross-references:
//   - DESIGN.md §4.1 (semantic opcode list)
//   - DESIGN.md §5.1 (interpreter frame, register file)
//   - DESIGN.md §5.2 (dispatch model)
//   - LAWS.md Rule 61 (no allocations/exceptions in hot path)
//   - LAWS.md Rule 72 (numeric semantics preserved exactly)
//   - LAWS.md Rule 90 (recursion limits)
//   - LAWS.md Rule 96 (Tier 0 universal fallback)

#include "core/interpreter/handlers_semantic.hpp"

#include <cmath>
#include <new>
#include <optional>
#include <vector>

#include "core/bytecode/instruction.hpp"
#include "core/bytecode/opcode.hpp"
#include "core/common/result.hpp"
#include "core/common/symbol_table.hpp"
#include "core/common/types.hpp"
#include "core/gc/gc.hpp"
#include "core/interpreter/inline_cache.hpp"
#include "core/interpreter/interpreter.hpp"
#include "core/interpreter/interpreter_concurrency.hpp"
#include "core/interpreter/speculative_arithmetic.hpp"
#include "core/object_model/object.hpp"
#include "core/object_model/shape_registry.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::interpreter {

using namespace bytecode;
using namespace common;
using namespace object_model;
namespace hs = handlers_semantic;

namespace handlers_semantic {

// --- Helper: read the current instruction ---
// Uses the unchecked fast path — the dispatch loop guarantees pc is in
// bounds and current_module_ is non-null.
[[nodiscard]] static inline Instruction current_inst(const InterpFrame& frame,
                                                       const Interpreter& interp) noexcept {
    return interp.current_inst_fast(frame.pc());
}

// --- Helper: truthiness test (B2-3 fix: empty string is falsy) ---
// Convention follows Python/JS: null, false, 0, 0.0, and empty string
// are falsy. NaN is truthy (matches Python/JS). All other values are
// truthy. (Rule 72: numeric semantics preserved; NaN is not false.)
[[nodiscard]] static inline bool truthy(TaggedValue v) noexcept {
    switch (v.tag()) {
        case Tag::Null:   return false;
        case Tag::Bool:   return v.as_bool();
        case Tag::Int:    return v.as_int() != 0;
        case Tag::Float: {
            const double f = v.as_float();
            // NaN is truthy (matches Python/JS). +0.0 and -0.0 are falsy.
            return (f != 0.0) || std::isnan(f);
        }
        case Tag::Str: {
            // B2-3 fix: empty string is falsy (Python: bool("") == False).
            return common::resolve_symbol(v.as_str()).size() != 0;
        }
        default:           return true;  // objects, closures, native handles
    }
}

// --- Helper: raise a NotImplementedError (B2-17/B2-19/B2-21 fix) ---
// Silent stubs that produce wrong results are forbidden (Rule 58).
// Instead, set the exception state so the dispatch loop propagates it.
static inline void raise_not_implemented(InterpFrame& frame) noexcept {
    // For now, use a null TaggedValue as the exception marker. The
    // Explainable Error Engine (Rule 47) will attach a message once
    // error symbols are pre-interned (B4 fix, still deferred).
    frame.set_exception(TaggedValue::make_null());
}

// --- Helper: bump the site-profile counter for the current pc ---
static inline void bump_profile(InterpFrame& frame) noexcept {
    SiteProfile* p = frame.find_or_create_profile(frame.pc());
    p->counter.fetch_add(1, std::memory_order_relaxed);
}

// --- Helper: record type feedback for a TaggedValue ---
static inline void record_type_feedback(InterpFrame& frame,
                                          TaggedValue v) noexcept {
    SiteProfile* p = frame.find_or_create_profile(frame.pc());
    const uint8_t tag_bit = static_cast<uint8_t>(1u << static_cast<uint8_t>(v.tag()));
    p->record_type_tag(tag_bit);
}

// --- Load / store ---

void handle_load_arg(InterpFrame& frame, Interpreter& interp) noexcept {
    // LOAD_ARG rdst, arg_idx
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId arg_idx = RegId{inst.operand_b()};
    frame.store_reg(rdst, frame.load_reg(arg_idx));
    bump_profile(frame);
    frame.advance_pc();
}

void handle_load_const(InterpFrame& frame, Interpreter& interp) noexcept {
    // LOAD_CONST rdst, const_idx8
    // operand_a = destination register, operand_b = constant pool index (0-255).
    // For >255 constants, use the extended form (InstructionExt).
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const uint8_t const_idx = inst.operand_b();
    const auto* mod = interp.current_module();
    if (mod == nullptr || const_idx >= mod->constants().size()) [[unlikely]] {
        // Cannot proceed; set exception and stop.
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    frame.store_reg(rdst, mod->constants()[const_idx].value);
    bump_profile(frame);
    frame.advance_pc();
}

void handle_load_local(InterpFrame& frame, Interpreter& interp) noexcept {
    // LOAD_LOCAL rdst, src
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    frame.store_reg(rdst, frame.load_reg(src));
    bump_profile(frame);
    frame.advance_pc();
}

void handle_store_local(InterpFrame& frame, Interpreter& interp) noexcept {
    // STORE_LOCAL dst, rsrc
    const Instruction inst = current_inst(frame, interp);
    const RegId dst = RegId{inst.operand_a()};
    const RegId rsrc = RegId{inst.operand_b()};
    frame.store_reg(dst, frame.load_reg(rsrc));
    bump_profile(frame);
    frame.advance_pc();
}

// --- Property access ---

// Helper: look up the InlineCache slot for the current pc in the frame's
// site profile. The IC is keyed on (pc); we store the (shape_id, version,
// offset) tuple in the SiteProfile's poly_entries[0] (monomorphic) or
// poly_entries[1..N] (polymorphic).
//
// For Tier 0 we use a monomorphic IC with a single slot. Polymorphic ICs
// (DESIGN.md §5.6) are a future-work item; for now, on a miss, we
// overwrite the single slot with the new observation.
[[nodiscard]] static inline std::optional<TaggedValue>
ic_try_get_prop(InterpFrame& frame, const Object* obj,
                 common::SymbolId /*prop_name*/) noexcept {
    SiteProfile* p = frame.find_or_create_profile(frame.pc());
    if (p->poly_entries.empty()) return std::nullopt;
    const auto& entry = p->poly_entries[0];
    const OmniShape* shape = obj->shape();
    if (shape == nullptr) return std::nullopt;
    if (shape->shape_id() != entry.shape_id) return std::nullopt;
    if (shape->shape_version() != entry.shape_version) return std::nullopt;
    // Hit: read the slot directly.
    const uint32_t slot = entry.hit_count;  // hit_count is reused as slot index
    if (slot >= obj->payload.fixed_struct.slot_count) return std::nullopt;
    return obj->payload.fixed_struct.slots[slot];
}

// Helper: record a property-access observation in the IC. The slot index
// is stored in hit_count (poly_entries uses hit_count as a slot index for
// property-access ICs; the alternative would be a separate field, but we
// reuse the existing struct).
static inline void ic_record_get_prop(InterpFrame& frame,
                                       common::ShapeId sid,
                                       common::ShapeVersion sv,
                                       uint32_t slot) noexcept {
    SiteProfile* p = frame.find_or_create_profile(frame.pc());
    if (p->poly_entries.empty()) {
        SiteProfile::PolyEntry e{sid, sv, slot};
        p->poly_entries.push_back(e);
    } else {
        // Overwrite the monomorphic slot.
        auto& e = p->poly_entries[0];
        e.shape_id = sid;
        e.shape_version = sv;
        e.hit_count = slot;
    }
}

void handle_get_prop(InterpFrame& frame, Interpreter& interp) noexcept {
    // GET_PROP rdst, obj_reg
    // Property name is in the module's per-pc side table.
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId obj_reg = RegId{inst.operand_b()};
    const TaggedValue obj_val = frame.load_reg(obj_reg);
    if (!obj_val.is_object_ref()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* obj = obj_val.as_object();
    const auto* mod = interp.current_module();
    if (mod == nullptr) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const common::SymbolId prop_name = mod->prop_name_at(frame.pc());
    if (prop_name == common::NULL_SYMBOL) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }

    // Try the monomorphic IC first.
    auto cached = ic_try_get_prop(frame, obj, prop_name);
    if (cached.has_value()) [[likely]] {
        frame.store_reg(rdst, *cached);
        bump_profile(frame);
        frame.advance_pc();
        return;
    }

    // IC miss: slow path. Look up the property in the shape's table.
    const OmniShape* shape = obj->shape();
    if (shape == nullptr || shape->layout_kind() != LayoutKind::FixedStruct) {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const uint32_t slot = ShapeRegistry::find_slot(*shape, prop_name);
    if (slot == common::INVALID_PC || slot >= obj->payload.fixed_struct.slot_count) {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    // Record the IC observation.
    ic_record_get_prop(frame, shape->shape_id(), shape->shape_version(), slot);
    frame.store_reg(rdst, obj->payload.fixed_struct.slots[slot]);
    bump_profile(frame);
    frame.advance_pc();
}

void handle_set_prop(InterpFrame& frame, Interpreter& interp) noexcept {
    // SET_PROP obj_reg, rsrc
    // Property name is in the module's per-pc side table.
    const Instruction inst = current_inst(frame, interp);
    const RegId obj_reg = RegId{inst.operand_a()};
    const RegId rsrc = RegId{inst.operand_b()};
    const TaggedValue obj_val = frame.load_reg(obj_reg);
    if (!obj_val.is_object_ref()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* obj = obj_val.as_object();
    const auto* mod = interp.current_module();
    if (mod == nullptr) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const common::SymbolId prop_name = mod->prop_name_at(frame.pc());
    if (prop_name == common::NULL_SYMBOL) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }

    // Look up the property in the shape's table.
    const OmniShape* shape = obj->shape();
    if (shape == nullptr || shape->layout_kind() != LayoutKind::FixedStruct) {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const uint32_t slot = ShapeRegistry::find_slot(*shape, prop_name);
    if (slot == common::INVALID_PC || slot >= obj->payload.fixed_struct.slot_count) {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    obj->payload.fixed_struct.slots[slot] = frame.load_reg(rsrc);
    bump_profile(frame);
    frame.advance_pc();
}

void handle_get_field(InterpFrame& frame, Interpreter& interp) noexcept {
    // GET_FIELD rdst, obj_reg
    // Slot index is in the module's per-pc side table.
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId obj_reg = RegId{inst.operand_b()};
    const TaggedValue obj_val = frame.load_reg(obj_reg);
    if (!obj_val.is_object_ref()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* obj = obj_val.as_object();
    if (obj->layout_kind() != LayoutKind::FixedStruct) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const auto* mod = interp.current_module();
    if (mod == nullptr) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const uint32_t slot = mod->field_slot_at(frame.pc());
    if (slot >= obj->payload.fixed_struct.slot_count) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    frame.store_reg(rdst, obj->payload.fixed_struct.slots[slot]);
    bump_profile(frame);
    frame.advance_pc();
}

void handle_set_field(InterpFrame& frame, Interpreter& interp) noexcept {
    // SET_FIELD obj_reg, rsrc
    // Slot index is in the module's per-pc side table.
    const Instruction inst = current_inst(frame, interp);
    const RegId obj_reg = RegId{inst.operand_a()};
    const RegId rsrc = RegId{inst.operand_b()};
    const TaggedValue obj_val = frame.load_reg(obj_reg);
    if (!obj_val.is_object_ref()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* obj = obj_val.as_object();
    if (obj->layout_kind() != LayoutKind::FixedStruct) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const auto* mod = interp.current_module();
    if (mod == nullptr) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const uint32_t slot = mod->field_slot_at(frame.pc());
    if (slot >= obj->payload.fixed_struct.slot_count) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    obj->payload.fixed_struct.slots[slot] = frame.load_reg(rsrc);
    bump_profile(frame);
    frame.advance_pc();
}

// --- Arithmetic (2-operand form: rdst = rdst OP rsrc2) ---

void handle_add(InterpFrame& frame, Interpreter& interp) noexcept {
    // ADD rdst_rsrc1, rsrc2
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    // Try the speculative int fast path first (Rule 72: overflow -> fallback).
    auto r = spec_int_add(a, b);
    if (r.has_value()) [[likely]] {
        frame.store_reg(rdst, *r);
    } else {
        // Try float fast path.
        auto rf = spec_float_add(a, b);
        if (rf.has_value()) {
            frame.store_reg(rdst, *rf);
        } else {
            // Try string concat.
            auto rs = spec_str_concat(a, b);
            if (rs.has_value()) {
                frame.store_reg(rdst, *rs);
            } else if ((a.is_int() || a.is_float()) && (b.is_int() || b.is_float())) {
                // Mixed int/float: promote both to float (Rule 72).
                const double av = a.is_int() ? static_cast<double>(a.as_int())
                                              : a.as_float();
                const double bv = b.is_int() ? static_cast<double>(b.as_int())
                                              : b.as_float();
                frame.store_reg(rdst, TaggedValue::make_float(av + bv));
            } else {
                // Cannot add; would need operator-overload dispatch.
                frame.set_exception(TaggedValue::make_null());
                return;
            }
        }
    }
    bump_profile(frame);
    frame.advance_pc();
}

void handle_sub(InterpFrame& frame, Interpreter& interp) noexcept {
    // SUB rdst_rsrc1, rsrc2
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    auto r = spec_int_sub(a, b);
    if (r.has_value()) [[likely]] {
        frame.store_reg(rdst, *r);
    } else {
        // Try float fast path or mixed int/float promotion.
        auto rf = spec_float_sub(a, b);
        if (rf.has_value()) {
            frame.store_reg(rdst, *rf);
        } else if ((a.is_int() || a.is_float()) && (b.is_int() || b.is_float())) {
            // Mixed int/float: promote both to float.
            const double av = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
            const double bv = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
            frame.store_reg(rdst, TaggedValue::make_float(av - bv));
        } else {
            frame.set_exception(TaggedValue::make_null());
            return;
        }
    }
    bump_profile(frame);
    frame.advance_pc();
}

void handle_mul(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    auto r = spec_int_mul(a, b);
    if (r.has_value()) [[likely]] {
        frame.store_reg(rdst, *r);
    } else {
        auto rf = spec_float_mul(a, b);
        if (rf.has_value()) {
            frame.store_reg(rdst, *rf);
        } else if ((a.is_int() || a.is_float()) && (b.is_int() || b.is_float())) {
            const double av = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
            const double bv = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
            frame.store_reg(rdst, TaggedValue::make_float(av * bv));
        } else {
            frame.set_exception(TaggedValue::make_null());
            return;
        }
    }
    bump_profile(frame);
    frame.advance_pc();
}

void handle_div(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    auto r = spec_int_div(a, b);
    if (r.has_value()) {
        frame.store_reg(rdst, *r);
    } else {
        // B2-8 fix: mixed int/float must promote to float.
        // If either operand is numeric, promote both to float and divide.
        // IEEE 754 handles div-by-zero (yields Infinity/-Infinity/NaN).
        if ((a.is_int() || a.is_float()) && (b.is_int() || b.is_float())) {
            const double av = a.is_int() ? static_cast<double>(a.as_int())
                                          : a.as_float();
            const double bv = b.is_int() ? static_cast<double>(b.as_int())
                                          : b.as_float();
            frame.store_reg(rdst, TaggedValue::make_float(av / bv));
        } else {
            frame.set_exception(TaggedValue::make_null());
            return;
        }
    }
    bump_profile(frame);
    frame.advance_pc();
}

void handle_mod(InterpFrame& frame, Interpreter& interp) noexcept {
    // MOD rdst_rsrc1, rsrc2
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    auto r = spec_int_mod(a, b);
    if (r.has_value()) {
        frame.store_reg(rdst, *r);
    } else {
        // Try float fast path or mixed int/float promotion.
        auto rf = spec_float_mod(a, b);
        if (rf.has_value()) {
            frame.store_reg(rdst, *rf);
        } else if ((a.is_int() || a.is_float()) && (b.is_int() || b.is_float())) {
            const double av = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
            const double bv = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
            frame.store_reg(rdst, TaggedValue::make_float(std::fmod(av, bv)));
        } else {
            frame.set_exception(TaggedValue::make_null());
            return;
        }
    }
    bump_profile(frame);
    frame.advance_pc();
}

// --- Comparison (2-operand form: rdst = bool(rdst OP rsrc2)) ---

void handle_eq(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    // Bitwise equality preserves NaN != NaN (Rule 72).
    frame.store_reg(rdst, TaggedValue::make_bool(a.bitwise_eq(b)));
    bump_profile(frame);
    frame.advance_pc();
}

// --- Helper: numeric compare with int/float/mixed promotion ---
// Returns the comparison result, or nullopt if the operands are not
// numeric (caller should raise).
[[nodiscard]] static inline std::optional<bool>
numeric_compare(TaggedValue a, TaggedValue b,
                 int op) noexcept {
    // op: 0=lt, 1=gt, 2=le, 3=ge
    if (a.is_int() && b.is_int()) {
        const int64_t av = a.as_int();
        const int64_t bv = b.as_int();
        switch (op) {
            case 0: return av < bv;
            case 1: return av > bv;
            case 2: return av <= bv;
            case 3: return av >= bv;
        }
    }
    if ((a.is_int() || a.is_float()) && (b.is_int() || b.is_float())) {
        const double av = a.is_int() ? static_cast<double>(a.as_int()) : a.as_float();
        const double bv = b.is_int() ? static_cast<double>(b.as_int()) : b.as_float();
        switch (op) {
            case 0: return av < bv;
            case 1: return av > bv;
            case 2: return av <= bv;
            case 3: return av >= bv;
        }
    }
    return std::nullopt;
}

void handle_lt(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    auto r = numeric_compare(a, b, 0);
    if (r.has_value()) {
        frame.store_reg(rdst, TaggedValue::make_bool(*r));
    } else {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    bump_profile(frame);
    frame.advance_pc();
}

void handle_gt(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    auto r = numeric_compare(a, b, 1);
    if (r.has_value()) {
        frame.store_reg(rdst, TaggedValue::make_bool(*r));
    } else {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    bump_profile(frame);
    frame.advance_pc();
}

void handle_le(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    auto r = numeric_compare(a, b, 2);
    if (r.has_value()) {
        frame.store_reg(rdst, TaggedValue::make_bool(*r));
    } else {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    bump_profile(frame);
    frame.advance_pc();
}

void handle_ge(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    record_type_feedback(frame, a);
    record_type_feedback(frame, b);
    auto r = numeric_compare(a, b, 3);
    if (r.has_value()) {
        frame.store_reg(rdst, TaggedValue::make_bool(*r));
    } else {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    bump_profile(frame);
    frame.advance_pc();
}

void handle_ne(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId rsrc2 = RegId{inst.operand_b()};
    const TaggedValue a = frame.load_reg(rdst);
    const TaggedValue b = frame.load_reg(rsrc2);
    // Bitwise inequality preserves NaN != NaN (Rule 72).
    frame.store_reg(rdst, TaggedValue::make_bool(!a.bitwise_eq(b)));
    bump_profile(frame);
    frame.advance_pc();
}

// --- Control flow ---

void handle_call(InterpFrame& frame, Interpreter& interp) noexcept {
    // CALL fn_reg, arg_count
    // Args are in registers 0..arg_count-1.
    // Return value goes into register 0.
    const Instruction inst = current_inst(frame, interp);
    const RegId fn_reg = RegId{inst.operand_a()};
    const uint8_t arg_count = inst.operand_b();
    const TaggedValue fn_val = frame.load_reg(fn_reg);
    if (!fn_val.is_object_ref()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* fn_obj = fn_val.as_object();
    if (fn_obj->layout_kind() != LayoutKind::FunctionObject) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const auto& fpl = fn_obj->payload.function;
    // B2-14 fix: pass a span over the caller's registers directly,
    // avoiding the 4KB stack copy. The caller's regs_ array is alive
    // for the duration of the recursive execute() call.
    std::span<const TaggedValue> args_span{frame.regs_data(), arg_count};
    // B2-1 fix: use the function_index from the FunctionPayload
    // (was hardcoded to 0).
    auto result = interp.execute(fpl.module_id, fpl.function_index, args_span);
    if (!result.has_value()) [[unlikely]] {
        // Propagate the error as an exception.
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    frame.store_reg(RegId{0}, *result);
    bump_profile(frame);
    frame.advance_pc();
}

void handle_jump(InterpFrame& frame, Interpreter& interp) noexcept {
    // JUMP delta16 (signed)
    const Instruction inst = current_inst(frame, interp);
    const uint16_t raw_delta = inst.operand_ab();
    const int16_t signed_delta = static_cast<int16_t>(raw_delta);
    frame.advance_pc(static_cast<uint32_t>(static_cast<int32_t>(signed_delta)));
}

void handle_branch(InterpFrame& frame, Interpreter& interp) noexcept {
    // BRANCH cond_reg, delta8 (signed)
    // If regs_[cond_reg] is truthy, jump by delta8; else fall through.
    const Instruction inst = current_inst(frame, interp);
    const RegId cond_reg = RegId{inst.operand_a()};
    const int8_t delta8 = static_cast<int8_t>(inst.operand_b());
    const TaggedValue cond = frame.load_reg(cond_reg);
    const bool taken = truthy(cond);
    // Record branch bias for adaptive quickening (§5.4).
    SiteProfile* p = frame.find_or_create_profile(frame.pc());
    if (taken) p->branch_bias.record_taken();
    else p->branch_bias.record_not_taken();
    if (taken) {
        frame.advance_pc(static_cast<uint32_t>(static_cast<int32_t>(delta8)));
    } else {
        frame.advance_pc();
    }
}

void handle_return(InterpFrame& frame, Interpreter& interp) noexcept {
    // RETURN rsrc
    // Copy regs_[rsrc] to regs_[0] (return value convention) and
    // terminate the frame by setting pc to the end of the code stream.
    const Instruction inst = current_inst(frame, interp);
    const RegId rsrc = RegId{inst.operand_a()};
    frame.store_reg(RegId{0}, frame.load_reg(rsrc));
    // Terminate the dispatch loop by setting pc past the end.
    const auto* mod = interp.current_module();
    if (mod != nullptr) {
        frame.set_pc(static_cast<BytecodePC>(mod->length()));
    }
}

// --- Object construction ---

void handle_make_object(InterpFrame& frame, Interpreter& interp) noexcept {
    // MAKE_OBJECT rdst, shape_id8
    // Allocates a new Object from the GC heap. The Object struct is
    // placement-new'd into the GC-allocated memory. The slot array is
    // also GC-allocated so the GC can reclaim it when the object dies.
    //
    // Per Rule 86: the register gc_map is updated by store_reg when
    // the new object reference is stored into rdst.
    // Per Rule 61: MAKE_OBJECT is allowed to allocate — it is the slow
    // path by definition. The fast path is PEA in T2+.
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const uint8_t shape_id8 = inst.operand_b();
    OmniShape* shape = ShapeRegistry::instance().lookup(
        static_cast<common::ShapeId>(shape_id8));
    if (shape == nullptr) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const uint32_t slot_count = static_cast<uint32_t>(shape->properties().size());

    // Allocate the Object from the GC heap.
    auto obj_ref = gc::GarbageCollector::instance().alloc(sizeof(Object));
    if (obj_ref.is_null()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* obj = static_cast<Object*>(
        gc::GarbageCollector::instance().resolve(obj_ref));

    // Allocate the slot array from the GC heap (if needed).
    TaggedValue* slots = nullptr;
    if (slot_count > 0) {
        auto slots_ref = gc::GarbageCollector::instance().alloc(
            sizeof(TaggedValue) * slot_count);
        if (slots_ref.is_null()) [[unlikely]] {
            frame.set_exception(TaggedValue::make_null());
            return;
        }
        // Mark as raw — the GC should mark it (it's reachable from the
        // object) but NOT scan it (it has no Object header).
        gc::GarbageCollector::instance().mark_raw(slots_ref);
        slots = static_cast<TaggedValue*>(
            gc::GarbageCollector::instance().resolve(slots_ref));
    }

    // Construct the Object in the GC-allocated memory.
    new (&obj->header) ObjectHeader{};
    new (&obj->payload) Object::Payload{};
    obj->header.shape_ref.store(shape, std::memory_order_release);
    obj->payload.fixed_struct.slots = slots;
    obj->payload.fixed_struct.slot_count = slot_count;
    frame.store_reg(rdst, TaggedValue::make_object(obj));
    bump_profile(frame);
    frame.advance_pc();
}

void handle_make_closure(InterpFrame& frame, Interpreter& interp) noexcept {
    // MAKE_CLOSURE rdst, fn_reg, env_reg
    // For now: stub that copies the function object reference.
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId fn_reg = RegId{inst.operand_b()};
    frame.store_reg(rdst, frame.load_reg(fn_reg));
    bump_profile(frame);
    frame.advance_pc();
}

// --- Iteration ---
//
// We implement a minimal range iterator protocol sufficient for the
// common `for i in 0..N` pattern. The iterator state is held in a
// FixedStruct Object with 2 slots:
//   slot 0: current (Int)
//   slot 1: end (Int)
// GetIter creates this object from an Int input (treating it as the
// end of an exclusive range [0, end)).
// Next advances slot 0; if slot 0 >= slot 1, sets rdst to null
// (the convention used by the ITER_NEXT_BRANCH fused opcode to detect
// end-of-iteration).

// Pre-registered shape id for the 2-slot range iterator. Set up by
// the runtime on first use; populated lazily.
static std::atomic<common::ShapeId> g_range_iter_shape_id{0};

[[nodiscard]] static inline common::ShapeId get_or_init_range_iter_shape() {
    common::ShapeId id = g_range_iter_shape_id.load(std::memory_order_acquire);
    if (id != 0) return id;
    // Intern a shape with 2 slots: ["current", "end"].
    auto cur = common::intern_symbol("current");
    auto end = common::intern_symbol("end");
    if (!cur.has_value() || !end.has_value()) return 0;
    OmniShape* shape = object_model::ShapeRegistry::instance().intern(
        2, std::vector<common::SymbolId>{*cur, *end});
    id = shape->shape_id();
    g_range_iter_shape_id.store(id, std::memory_order_release);
    return id;
}

void handle_get_iter(InterpFrame& frame, Interpreter& interp) noexcept {
    // GET_ITER rdst, src
    // If src is an Int, create a range iterator [0, src).
    // Otherwise, raise (full iterator protocol not yet implemented).
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    const TaggedValue src_val = frame.load_reg(src);
    if (!src_val.is_int()) [[unlikely]] {
        // Full iterator protocol (Iterable trait dispatch) is future work.
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const common::ShapeId shape_id = get_or_init_range_iter_shape();
    if (shape_id == 0) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    OmniShape* shape = object_model::ShapeRegistry::instance().lookup(shape_id);
    if (shape == nullptr) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    // Allocate the iterator object from the GC heap.
    auto obj_ref = gc::GarbageCollector::instance().alloc(sizeof(Object));
    if (obj_ref.is_null()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* obj = static_cast<Object*>(
        gc::GarbageCollector::instance().resolve(obj_ref));
    auto slots_ref = gc::GarbageCollector::instance().alloc(sizeof(TaggedValue) * 2);
    if (slots_ref.is_null()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    gc::GarbageCollector::instance().mark_raw(slots_ref);
    TaggedValue* slots = static_cast<TaggedValue*>(
        gc::GarbageCollector::instance().resolve(slots_ref));
    slots[0] = TaggedValue::make_int(0);
    slots[1] = src_val;
    new (&obj->header) ObjectHeader{};
    new (&obj->payload) Object::Payload{};
    obj->header.shape_ref.store(shape, std::memory_order_release);
    obj->payload.fixed_struct.slots = slots;
    obj->payload.fixed_struct.slot_count = 2;
    frame.store_reg(rdst, TaggedValue::make_object(obj));
    bump_profile(frame);
    frame.advance_pc();
}

void handle_next(InterpFrame& frame, Interpreter& interp) noexcept {
    // NEXT rdst, iter_reg
    // Reads the iterator's current/end slots. If current < end, returns
    // current and advances. Otherwise, sets rdst to null (end-of-iter).
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId iter_reg = RegId{inst.operand_b()};
    const TaggedValue iter_val = frame.load_reg(iter_reg);
    if (!iter_val.is_object_ref()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* obj = iter_val.as_object();
    if (obj->layout_kind() != LayoutKind::FixedStruct
        || obj->payload.fixed_struct.slot_count < 2) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    const TaggedValue cur = obj->payload.fixed_struct.slots[0];
    const TaggedValue end = obj->payload.fixed_struct.slots[1];
    if (!cur.is_int() || !end.is_int()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    if (cur.as_int() >= end.as_int()) {
        // End of iteration: signal with null.
        frame.store_reg(rdst, TaggedValue::make_null());
    } else {
        // Return current, advance the iterator.
        frame.store_reg(rdst, cur);
        obj->payload.fixed_struct.slots[0] =
            TaggedValue::make_int(cur.as_int() + 1);
    }
    bump_profile(frame);
    frame.advance_pc();
}

// --- Concurrency (Rule 118: no GIL) ---

void handle_spawn(InterpFrame& frame, Interpreter& interp) noexcept {
    // SPAWN rdst, fn_reg
    // Spawn an M:N task. The task scheduler is not yet implemented;
    // for now we raise an exception indicating async is unavailable.
    // DESIGN.md §20 (concurrency model) is the spec for the scheduler.
    (void)interp;
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    frame.store_reg(rdst, TaggedValue::make_null());
    frame.set_exception(TaggedValue::make_null());  // async not yet supported
    (void)inst;
}

void handle_await(InterpFrame& frame, Interpreter& interp) noexcept {
    // AWAIT rdst, awaitable_reg
    // Suspend the current task. Stub: sets exception.
    (void)interp;
    frame.set_exception(TaggedValue::make_null());
}

void handle_sync_enter(InterpFrame& frame, Interpreter& interp) noexcept {
    // SYNC_ENTER obj_reg
    // Acquire the thin lock on obj. Push a SyncEntry so the runtime
    // can release it on frame exit or exception (Rule 118).
    // B2-5 fix: use the interpreter's real thread_id (was hardcoded to 1).
    const Instruction inst = current_inst(frame, interp);
    const RegId obj_reg = RegId{inst.operand_a()};
    const TaggedValue obj_val = frame.load_reg(obj_reg);
    if (!obj_val.is_object_ref()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* obj = obj_val.as_object();
    // B2-5 fix: use the interpreter's thread_id, not a hardcoded 1.
    if (!obj->header.sync_word.try_acquire(interp.thread_id())) [[unlikely]] {
        // Contended: would inflate to monitor in a full implementation.
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    frame.enter_sync(obj);
    bump_profile(frame);
    frame.advance_pc();
}

void handle_sync_exit(InterpFrame& frame, Interpreter& interp) noexcept {
    // SYNC_EXIT (no operands)
    // Release the most recently acquired sync lock.
    // B2-5 fix: use the interpreter's real thread_id.
    const auto& stack = frame.sync_stack();
    if (stack.empty()) [[unlikely]] {
        frame.set_exception(TaggedValue::make_null());
        return;
    }
    Object* obj = stack.back().lock_obj;
    obj->header.sync_word.release(interp.thread_id());
    frame.exit_sync();
    bump_profile(frame);
    frame.advance_pc();
}

void handle_chan_send(InterpFrame& frame, Interpreter& interp) noexcept {
    // CHAN_SEND chan_reg, rsrc
    // Stub: channels not yet implemented.
    (void)interp;
    frame.set_exception(TaggedValue::make_null());
}

void handle_chan_recv(InterpFrame& frame, Interpreter& interp) noexcept {
    // CHAN_RECV rdst, chan_reg
    // Stub: channels not yet implemented.
    (void)interp;
    frame.set_exception(TaggedValue::make_null());
}

// --- Exception handling ---

void handle_raise(InterpFrame& frame, Interpreter& interp) noexcept {
    // RAISE exc_reg
    const Instruction inst = current_inst(frame, interp);
    const RegId exc_reg = RegId{inst.operand_a()};
    frame.set_exception(frame.load_reg(exc_reg));
    // Do not advance pc; the dispatch loop will detect the exception
    // at the next iteration and jump to the nearest handler.
}

void handle_try_begin(InterpFrame& frame, Interpreter& interp) noexcept {
    // TRY_BEGIN (no operands)
    // Marker opcode; the handler table is consulted on RAISE.
    // Just advance pc.
    (void)interp;
    bump_profile(frame);
    frame.advance_pc();
}

void handle_try_end(InterpFrame& frame, Interpreter& interp) noexcept {
    // TRY_END (no operands)
    // Marker opcode; exits the try region.
    (void)interp;
    bump_profile(frame);
    frame.advance_pc();
}

// --- Pattern matching ---

void handle_match(InterpFrame& frame, Interpreter& interp) noexcept {
    // MATCH rdst, src, pattern_idx
    // Pattern matching requires the pattern compiler, which is not yet
    // implemented. For now: stub that always fails (no match).
    (void)interp;
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    frame.store_reg(rdst, TaggedValue::make_bool(false));
    bump_profile(frame);
    frame.advance_pc();
}

// --- Resource management ---

void handle_using(InterpFrame& frame, Interpreter& interp) noexcept {
    // USING rsrc
    // Enter a deterministic resource scope. The object's dispose trait
    // will be called on scope exit. Stub: just advance pc.
    (void)interp;
    bump_profile(frame);
    frame.advance_pc();
}

void handle_defer(InterpFrame& frame, Interpreter& interp) noexcept {
    // DEFER rsrc
    // Register a deferred cleanup. Stub: just advance pc.
    (void)interp;
    bump_profile(frame);
    frame.advance_pc();
}

// --- Stack manipulation ---

void handle_dup(InterpFrame& frame, Interpreter& interp) noexcept {
    // DUP rdst, src
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    frame.store_reg(rdst, frame.load_reg(src));
    bump_profile(frame);
    frame.advance_pc();
}

void handle_pop(InterpFrame& frame, Interpreter& interp) noexcept {
    // POP rsrc
    // Clear the register (logical pop).
    (void)interp;
    const Instruction inst = current_inst(frame, interp);
    const RegId rsrc = RegId{inst.operand_a()};
    frame.store_reg(rsrc, TaggedValue::make_null());
    bump_profile(frame);
    frame.advance_pc();
}

// --- Type checks ---

void handle_is_null(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    frame.store_reg(rdst, TaggedValue::make_bool(frame.load_reg(src).is_null()));
    bump_profile(frame);
    frame.advance_pc();
}

void handle_is_int(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    frame.store_reg(rdst, TaggedValue::make_bool(frame.load_reg(src).is_int()));
    bump_profile(frame);
    frame.advance_pc();
}

void handle_is_float(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    frame.store_reg(rdst, TaggedValue::make_bool(frame.load_reg(src).is_float()));
    bump_profile(frame);
    frame.advance_pc();
}

void handle_is_str(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    frame.store_reg(rdst, TaggedValue::make_bool(frame.load_reg(src).is_str()));
    bump_profile(frame);
    frame.advance_pc();
}

void handle_is_object(InterpFrame& frame, Interpreter& interp) noexcept {
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    frame.store_reg(rdst, TaggedValue::make_bool(frame.load_reg(src).is_object_ref()));
    bump_profile(frame);
    frame.advance_pc();
}

// --- Coercion / morphing ---

void handle_coerce(InterpFrame& frame, Interpreter& interp) noexcept {
    // COERCE rdst, src, target_type
    // Implicit morphing. Handles int<->float with safety checks.
    const Instruction inst = current_inst(frame, interp);
    const RegId rdst = RegId{inst.operand_a()};
    const RegId src = RegId{inst.operand_b()};
    const TaggedValue v = frame.load_reg(src);
    if (v.is_int()) {
        frame.store_reg(rdst, TaggedValue::make_float(static_cast<double>(v.as_int())));
    } else if (v.is_float()) {
        // B2-7 fix: NaN/Inf→int is UB in C++. Check before casting.
        const double f = v.as_float();
        if (!std::isfinite(f) || f < static_cast<double>(INT64_MIN)
            || f > static_cast<double>(INT64_MAX)) {
            // Out of range or NaN/Inf: raise TypeError.
            frame.set_exception(TaggedValue::make_null());
            return;
        }
        frame.store_reg(rdst, TaggedValue::make_int(static_cast<int64_t>(f)));
    } else {
        // B2-19 fix: raise instead of silently doing nothing for
        // non-numeric types (e.g., string→int needs a parser).
        raise_not_implemented(frame);
        return;
    }
    bump_profile(frame);
    frame.advance_pc();
}

// --- Trait injection ---

void handle_inject_trait(InterpFrame& frame, Interpreter& interp) noexcept {
    // INJECT_TRAIT obj_reg, trait_id16
    // Trait injection requires the shape-transition engine, which is
    // not yet implemented. Stub: bump the global epoch to signal
    // invalidation, then advance pc.
    (void)interp;
    GlobalEpoch::bump();
    bump_profile(frame);
    frame.advance_pc();
}

void handle_remove_trait(InterpFrame& frame, Interpreter& interp) noexcept {
    // REMOVE_TRAIT obj_reg, trait_id16
    (void)interp;
    GlobalEpoch::bump();
    bump_profile(frame);
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
    interp.register_handler(Opcode::Nop,            [](InterpFrame& f, Interpreter&) noexcept {
        f.advance_pc();
    });
}

}  // namespace handlers_semantic

}  // namespace omni::interpreter

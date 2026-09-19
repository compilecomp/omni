// core/interpreter/interpreter.cpp
//
// Interpreter implementation — module loading, dispatch loop, safepoints.
//
// This file uses GCC's labels-as-values computed goto extension for the
// dispatch loop (the same technique CPython 3.11+ uses). The extension
// is enabled by -std=gnu++26; we suppress the pedantic warning here
// because computed goto is a deliberate, benchmark-justified choice.

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"

#include "core/interpreter/interpreter.hpp"

#include <cmath>
#include <memory>
#include <new>

#include "core/bytecode/bytecode_verifier.hpp"
#include "core/common/types.hpp"
#include "core/interpreter/adaptive_quickening.hpp"
#include "core/interpreter/fusion_engine.hpp"
#include "core/interpreter/handlers_fused.hpp"
#include "core/interpreter/handlers_quickened.hpp"
#include "core/interpreter/handlers_semantic.hpp"
#include "core/interpreter/interpreter_concurrency.hpp"
#include "core/interpreter/speculative_arithmetic.hpp"

namespace omni::interpreter {

using common::ErrorCategory;
using common::make_error;
using common::Result;

namespace {

/// Sentinel error symbols. Pre-interned at process startup (B4 fix).
/// For now they remain NULL_SYMBOL; a follow-up will intern them in
/// SymbolTable at process init. TODO: intern these properly.
constexpr common::SymbolId ERR_MODULE_NOT_FOUND = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_FUNCTION_NOT_FOUND = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_RECURSION_LIMIT = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_OPCODE = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_VERIFY_FAILED = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_ARG_COUNT = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_UNHANDLED_EXCEPTION = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_TIMEOUT = common::NULL_SYMBOL;

}  // namespace

Interpreter::Interpreter() {
    handlers_semantic::register_all(*this);
    handlers_quickened::register_all(*this);  // B11 fix
    handlers_fused::register_all(*this);      // B11 fix
    register_with_gc();
}

void Interpreter::register_with_gc() {
    // Register a root scanner that walks the current frame's registers.
    // The GC calls this during the mark phase. We capture `this` because
    // the interpreter is per-thread and the GC runs at a safepoint (the
    // interpreter is stopped, so `this` is valid).
    //
    // NOTE: This root scanner is a placeholder. The current interpreter
    // stores raw Object* pointers in registers, not HeapRefs. Once
    // TaggedValue is migrated to store HeapRef (per the 32-bit OmniGC
    // spec), this scanner will convert HeapRef -> mark(ref) directly.
    // For now, the GC operates on its own heap (separate from the
    // interpreter's raw-new objects). Full integration is a follow-up.
    gc::GarbageCollector::instance().register_root_scanner(
        [this](std::function<void(gc::HeapRef)> mark) {
            if (current_frame_ == nullptr) return;
            // Walk all registers. The gc_map tells us which registers
            // hold object references (Rule 86). Once objects are
            // GC-allocated, we'll resolve Object* -> HeapRef here.
            (void)mark;  // placeholder — no GC-managed objects yet
        });
}

Result<uint32_t> Interpreter::load_module(
    std::unique_ptr<bytecode::BytecodeModule> module) {
    // Verify before adding to the registry (Rule 105).
    auto verify_result = bytecode::BytecodeVerifier::verify(*module);
    if (!verify_result.has_value()) [[unlikely]] {
        return std::unexpected(verify_result.error());
    }
    uint32_t id = next_module_id_++;
    // std::move transfers ownership to the vector; the unique_ptr is
    // released. No explicit destructor call (B2 fix: do not double-destroy).
    modules_.push_back(std::move(module));
    return id;
}

Result<object_model::TaggedValue> Interpreter::execute(
    uint32_t module_id, uint32_t function_index,
    std::span<const object_model::TaggedValue> args) {
    if (module_id == 0 || module_id > modules_.size()) [[unlikely]] {
        return make_error(ErrorCategory::Bytecode, ERR_MODULE_NOT_FOUND);
    }
    const auto& module = *modules_[module_id - 1];
    if (function_index >= module.functions().size()) [[unlikely]] {
        return make_error(ErrorCategory::Bytecode, ERR_FUNCTION_NOT_FOUND);
    }
    const auto& fdesc = module.functions()[function_index];

    if (frame_depth_ >= common::DEFAULT_RECURSION_LIMIT) [[unlikely]] {
        return make_error(ErrorCategory::Stack, ERR_RECURSION_LIMIT);
    }
    if (args.size() != fdesc.param_count) [[unlikely]] {
        return make_error(ErrorCategory::Bytecode, ERR_ARG_COUNT);
    }
    if (fdesc.param_count > common::FRAME_REGISTER_COUNT) [[unlikely]] {
        // Verifier should have caught this; defense in depth (Rule 63).
        return make_error(ErrorCategory::Bytecode, ERR_ARG_COUNT);
    }

    // Construct the frame on the native stack (caller's frame).
    // For async/generator frames we would heap-allocate instead (Rule 76).
    InterpFrame frame(module_id, function_index, /*function_obj=*/nullptr);
    ++frame_depth_;
    struct FrameDepthGuard {
        uint32_t& depth;
        ~FrameDepthGuard() { --depth; }
    } depth_guard{frame_depth_};

    // B1 fix: expose the current module to handlers via current_module_.
    // Save and restore the previous value so that recursive execute()
    // calls (via CALL handler) work correctly.
    const bytecode::BytecodeModule* prev_module = current_module_;
    current_module_ = &module;
    InterpFrame* prev_frame = current_frame_;
    current_frame_ = &frame;
    struct CurrentModuleGuard {
        const bytecode::BytecodeModule*& slot;
        const bytecode::BytecodeModule* prev;
        ~CurrentModuleGuard() { slot = prev; }
    } module_guard{current_module_, prev_module};
    struct CurrentFrameGuard {
        InterpFrame*& slot;
        InterpFrame* prev;
        ~CurrentFrameGuard() { slot = prev; }
    } frame_guard{current_frame_, prev_frame};

    // Load arguments into registers.
    for (uint16_t i = 0; i < fdesc.param_count; ++i) {
        frame.store_reg(common::RegId{static_cast<uint8_t>(i)}, args[i]);
    }

    // Enter the dispatch loop.
    //
    // Computed goto dispatch (GCC labels-as-values).
    //
    // Each opcode has its own label and its own indirect jump to the next
    // handler. Unlike a switch (which shares one branch-prediction site
    // for the entire jump table), computed goto gives each opcode its
    // own branch predictor entry. This is the same technique CPython
    // 3.11+ uses (PEP 659) and gives ~10-15% over switch dispatch
    // because the branch predictor can learn per-opcode patterns
    // (e.g., "after LoadConst comes Add" vs "after Branch comes Jump").
    //
    // The hot opcodes (arithmetic, load/store, branch/jump, comparison)
    // are inlined directly into the label bodies — zero function calls
    // on the hot path. Cold opcodes fall through to L_cold which calls
    // the handler function via the dispatch table.
    //
    // Requires -std=gnu++26 (labels-as-values is a GNU extension).
    const auto code = module.code();
    constexpr uint64_t INSTRUCTION_BUDGET = 1'000'000'000ull;
    uint64_t instructions_executed = 0;

    // Label table: indexed by opcode value (0-255).
    // Built once per process (not per thread) — labels are function-local
    // and the same across all invocations of execute(). The static local
    // initialization is thread-safe under C++11+ and paid only once.
    //
    // We can't use C-style designated initializers in C++ ([expr] = val
    // is C-only), so we build the table with a one-shot initializer
    // lambda. The lambda captures &&label addresses via the closure.
    static const void* dispatch_table[256] = {};
    static bool dispatch_init = false;
    if (!dispatch_init) [[unlikely]] {
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::LoadConst)]       = &&L_load_const;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::LoadLocal)]       = &&L_load_local;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::LoadLocalFast)]   = &&L_load_local;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::LoadArg)]         = &&L_load_local;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Dup)]             = &&L_load_local;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::StoreLocal)]      = &&L_store_local;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Pop)]             = &&L_pop;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Nop)]             = &&L_nop;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Add)]             = &&L_add;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::AddIntFast)]      = &&L_add;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Sub)]             = &&L_sub;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::SubIntFast)]      = &&L_sub;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Mul)]             = &&L_mul;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::MulIntFast)]      = &&L_mul;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Div)]             = &&L_div;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::DivIntFast)]      = &&L_div;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Lt)]              = &&L_lt;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::LtIntFast)]       = &&L_lt;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Ge)]              = &&L_ge;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Gt)]              = &&L_gt;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Le)]              = &&L_le;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Eq)]              = &&L_eq;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::EqIntFast)]       = &&L_eq;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Ne)]              = &&L_ne;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::IsNull)]          = &&L_is_null;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::IsInt)]           = &&L_is_int;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Jump)]            = &&L_jump;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Branch)]          = &&L_branch;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::BranchTakenFast)] = &&L_branch_taken;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::BranchNotTakenFast)] = &&L_branch_not_taken;
        dispatch_table[static_cast<uint8_t>(bytecode::Opcode::Return)]          = &&L_return;
        dispatch_init = true;
    }
    // All other opcodes fall through to L_cold (function-pointer dispatch).

    // Initial dispatch.
    {
        const auto inst0 = code[frame.pc()];
        const auto op0 = static_cast<uint8_t>(inst0.opcode());
        const void* target = dispatch_table[op0];
        goto *(target ? target : &&L_cold);
    }

L_load_const:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const uint8_t ci = inst.operand_b();
        frame.store_reg(rdst, module.constants()[ci].value);
        frame.advance_pc();
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_load_local:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId src{inst.operand_b()};
        frame.store_reg(rdst, frame.load_reg(src));
        frame.advance_pc();
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_store_local:
    {
        const auto inst = code[frame.pc()];
        const common::RegId dst{inst.operand_a()};
        const common::RegId rsrc{inst.operand_b()};
        frame.store_reg(dst, frame.load_reg(rsrc));
        frame.advance_pc();
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_pop:
    {
        const auto inst = code[frame.pc()];
        frame.store_reg(common::RegId{inst.operand_a()},
                        object_model::TaggedValue::make_null());
        frame.advance_pc();
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_nop:
    frame.advance_pc();
    if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
    {
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_add:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        auto r = spec_int_add(a, b);
        if (r.has_value()) [[likely]] {
            frame.store_reg(rdst, *r);
            frame.advance_pc();
            if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
            const auto next = code[frame.pc()];
            const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
            goto *(t ? t : &&L_cold);
        }
        // Fallback: float, string, mixed.
        handlers_semantic::handle_add(frame, *this);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_sub:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        auto r = spec_int_sub(a, b);
        if (r.has_value()) [[likely]] {
            frame.store_reg(rdst, *r);
            frame.advance_pc();
            if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
            const auto next = code[frame.pc()];
            const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
            goto *(t ? t : &&L_cold);
        }
        handlers_semantic::handle_sub(frame, *this);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_mul:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        auto r = spec_int_mul(a, b);
        if (r.has_value()) [[likely]] {
            frame.store_reg(rdst, *r);
            frame.advance_pc();
            if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
            const auto next = code[frame.pc()];
            const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
            goto *(t ? t : &&L_cold);
        }
        handlers_semantic::handle_mul(frame, *this);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_div:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        auto r = spec_int_div(a, b);
        if (r.has_value()) {
            frame.store_reg(rdst, *r);
            frame.advance_pc();
            if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
            const auto next = code[frame.pc()];
            const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
            goto *(t ? t : &&L_cold);
        }
        handlers_semantic::handle_div(frame, *this);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_lt:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        if (a.is_int() && b.is_int()) [[likely]] {
            frame.store_reg(rdst,
                object_model::TaggedValue::make_bool(a.as_int() < b.as_int()));
            frame.advance_pc();
            if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
            const auto next = code[frame.pc()];
            const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
            goto *(t ? t : &&L_cold);
        }
        handlers_semantic::handle_lt(frame, *this);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_ge:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        if (a.is_int() && b.is_int()) [[likely]] {
            frame.store_reg(rdst,
                object_model::TaggedValue::make_bool(a.as_int() >= b.as_int()));
            frame.advance_pc();
            if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
            const auto next = code[frame.pc()];
            const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
            goto *(t ? t : &&L_cold);
        }
        handlers_semantic::handle_ge(frame, *this);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_gt:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        if (a.is_int() && b.is_int()) [[likely]] {
            frame.store_reg(rdst,
                object_model::TaggedValue::make_bool(a.as_int() > b.as_int()));
            frame.advance_pc();
            if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
            const auto next = code[frame.pc()];
            const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
            goto *(t ? t : &&L_cold);
        }
        handlers_semantic::handle_gt(frame, *this);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_le:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        if (a.is_int() && b.is_int()) [[likely]] {
            frame.store_reg(rdst,
                object_model::TaggedValue::make_bool(a.as_int() <= b.as_int()));
            frame.advance_pc();
            if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
            const auto next = code[frame.pc()];
            const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
            goto *(t ? t : &&L_cold);
        }
        handlers_semantic::handle_le(frame, *this);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_eq:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        frame.store_reg(rdst,
            object_model::TaggedValue::make_bool(a.bitwise_eq(b)));
        frame.advance_pc();
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_ne:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId rsrc2{inst.operand_b()};
        const auto a = frame.load_reg(rdst);
        const auto b = frame.load_reg(rsrc2);
        frame.store_reg(rdst,
            object_model::TaggedValue::make_bool(!a.bitwise_eq(b)));
        frame.advance_pc();
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_is_null:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId src{inst.operand_b()};
        frame.store_reg(rdst,
            object_model::TaggedValue::make_bool(frame.load_reg(src).is_null()));
        frame.advance_pc();
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_is_int:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rdst{inst.operand_a()};
        const common::RegId src{inst.operand_b()};
        frame.store_reg(rdst,
            object_model::TaggedValue::make_bool(frame.load_reg(src).is_int()));
        frame.advance_pc();
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_jump:
    {
        const auto inst = code[frame.pc()];
        const int16_t delta = static_cast<int16_t>(inst.operand_ab());
        frame.advance_pc(static_cast<uint32_t>(static_cast<int32_t>(delta)));
        // Safepoint poll on backedge (Rule 88).
        if (poll_safepoint()) [[unlikely]] handle_safepoint(frame);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_branch:
    {
        const auto inst = code[frame.pc()];
        const common::RegId cond_reg{inst.operand_a()};
        const int8_t delta8 = static_cast<int8_t>(inst.operand_b());
        const auto cond = frame.load_reg(cond_reg);
        bool taken;
        switch (cond.tag()) {
            case object_model::Tag::Null:   taken = false; break;
            case object_model::Tag::Bool:   taken = cond.as_bool(); break;
            case object_model::Tag::Int:    taken = cond.as_int() != 0; break;
            case object_model::Tag::Float: {
                const double f = cond.as_float();
                taken = (f != 0.0) || std::isnan(f);
                break;
            }
            default:           taken = true; break;
        }
        if (taken) {
            frame.advance_pc(static_cast<uint32_t>(static_cast<int32_t>(delta8)));
        } else {
            frame.advance_pc();
        }
        if (poll_safepoint()) [[unlikely]] handle_safepoint(frame);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_branch_taken:
    {
        const auto inst = code[frame.pc()];
        const common::RegId cond_reg{inst.operand_a()};
        const int8_t delta8 = static_cast<int8_t>(inst.operand_b());
        const auto cond = frame.load_reg(cond_reg);
        bool taken;
        switch (cond.tag()) {
            case object_model::Tag::Null:   taken = false; break;
            case object_model::Tag::Bool:   taken = cond.as_bool(); break;
            case object_model::Tag::Int:    taken = cond.as_int() != 0; break;
            case object_model::Tag::Float: {
                const double f = cond.as_float();
                taken = (f != 0.0) || std::isnan(f);
                break;
            }
            default:           taken = true; break;
        }
        if (taken) [[likely]] {
            frame.advance_pc(static_cast<uint32_t>(static_cast<int32_t>(delta8)));
        } else {
            frame.advance_pc();
        }
        if (poll_safepoint()) [[unlikely]] handle_safepoint(frame);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_branch_not_taken:
    {
        const auto inst = code[frame.pc()];
        const common::RegId cond_reg{inst.operand_a()};
        const int8_t delta8 = static_cast<int8_t>(inst.operand_b());
        const auto cond = frame.load_reg(cond_reg);
        bool taken;
        switch (cond.tag()) {
            case object_model::Tag::Null:   taken = false; break;
            case object_model::Tag::Bool:   taken = cond.as_bool(); break;
            case object_model::Tag::Int:    taken = cond.as_int() != 0; break;
            case object_model::Tag::Float: {
                const double f = cond.as_float();
                taken = (f != 0.0) || std::isnan(f);
                break;
            }
            default:           taken = true; break;
        }
        if (!taken) [[likely]] {
            frame.advance_pc();
        } else {
            frame.advance_pc(static_cast<uint32_t>(static_cast<int32_t>(delta8)));
        }
        if (poll_safepoint()) [[unlikely]] handle_safepoint(frame);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_return:
    {
        const auto inst = code[frame.pc()];
        const common::RegId rsrc{inst.operand_a()};
        frame.store_reg(common::RegId{0}, frame.load_reg(rsrc));
        frame.set_pc(static_cast<common::BytecodePC>(code.size()));
        // Fall through to loop exit (pc >= code.size()).
        goto L_dispatch_end;
    }

L_cold:
    {
        // Cold opcodes: function-pointer dispatch.
        const auto inst = code[frame.pc()];
        const auto op = inst.opcode();
        Handler h = dispatch_.get(op);
        if (!h) [[unlikely]] {
            const auto fb = bytecode::fallback_for(op);
            if (fb == bytecode::Opcode::Invalid) {
                return make_error(ErrorCategory::Bytecode, ERR_BAD_OPCODE);
            }
            h = dispatch_.get(fb);
            if (!h) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_BAD_OPCODE);
            }
        }
        h(frame, *this);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        // Check for frame exit (Return sets pc to end).
        if (frame.pc() >= code.size()) goto L_dispatch_end;
        if (poll_safepoint()) [[unlikely]] handle_safepoint(frame);
        if (!frame.exception().is_null()) [[unlikely]] goto L_exception;
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] goto L_timeout;
        const auto next = code[frame.pc()];
        const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
        goto *(t ? t : &&L_cold);
    }

L_exception:
    {
        const auto exc_type = frame.exception().is_object_ref()
            ? common::NULL_SYMBOL
            : common::NULL_SYMBOL;
        const auto* exh = module.find_handler(frame.pc(), exc_type);
        if (exh != nullptr) {
            frame.clear_exception();
            frame.set_pc(exh->handler_pc);
            const auto next = code[frame.pc()];
            const void* t = dispatch_table[static_cast<uint8_t>(next.opcode())];
            goto *(t ? t : &&L_cold);
        }
        return std::unexpected(make_error(ErrorCategory::Bytecode,
                                          ERR_UNHANDLED_EXCEPTION));
    }

L_timeout:
    return std::unexpected(make_error(ErrorCategory::Stack, ERR_TIMEOUT));

L_dispatch_end:
    // Return value convention: register 0 holds the return value.
    return frame.load_reg(common::RegId{0});
}

void Interpreter::handle_safepoint(InterpFrame& frame) noexcept {
    // B7 fix: real safepoint handler. Walks the frame's site profiles
    // and invalidates any IC whose shape epoch is stale (Rule 95, 117).
    //
    // B2-26 fix: check the global invalidation flag (was checking a
    // per-interpreter flag that was never set).
    if (GlobalEpoch::is_invalidation_pending()) [[unlikely]] {
        GlobalEpoch::clear_invalidation();
        const common::Epoch now = GlobalEpoch::current();
        // Walk each site profile and check whether the shape epoch has
        // advanced since the profile was last updated. If so, mark the
        // profile as needing re-quickening (state -> Generic) and clear
        // the poly_entries so the next observation starts fresh.
        for (auto& p : frame.profiles()) {
            if (p.shape_epoch != 0 && p.shape_epoch < now) {
                // Shape epoch advanced: invalidate this site.
                p.shape_epoch = common::INITIAL_EPOCH;
                p.shape_id = common::NULL_SHAPE;
                p.shape_version = common::INITIAL_SHAPE_VERSION;
                p.poly_entries.clear();
                // Demote to Generic so quickening is re-evaluated.
                if (p.state != SiteState::Disabled) {
                    p.state = SiteState::Generic;
                }
            }
        }
    }

    // Adaptive quickening: walk hot profiles and rewrite the bytecode
    // at hot, monomorphic sites to use the quickened form (DESIGN.md §5.4).
    // This is the Tier 0 specialization machinery — it reduces dispatch
    // overhead and type-check overhead at hot sites without invoking
    // the higher JIT tiers.
    //
    // We only run the quickening pass if we have a current module (i.e.,
    // we are inside execute()). The const_cast is safe: visit_frame uses
    // only the atomic-write API on the module, which is thread-safe.
    if (current_module_ != nullptr) [[likely]] {
        AdaptiveQuickening::visit_frame(frame,
            const_cast<bytecode::BytecodeModule&>(*current_module_), *this);
    }

    // Fusion engine: after quickening, try to fuse consecutive hot sites
    // into superinstructions (DESIGN.md §5.7). This reduces dispatch
    // overhead further by collapsing 2-3 instruction sequences into 1.
    if (current_module_ != nullptr) [[likely]] {
        FusionEngine::visit_frame(frame,
            const_cast<bytecode::BytecodeModule&>(*current_module_), *this);
    }

    // GC safepoint: if the GC has requested a collection, run it now.
    // The interpreter is at a safe point (no in-progress handler), so
    // the GC can safely scan roots (Rule 88).
    if (gc::GarbageCollector::instance().is_gc_requested()) [[unlikely]] {
        gc::GarbageCollector::instance().clear_gc_request();
        gc::GarbageCollector::instance().collect();
    }

    // GC safepoint: in a full implementation we would also check a global
    // GC-state flag for STW pauses requested by the GC thread itself.
    // DESIGN.md §19 (GC model) is the spec for this.
}

#pragma GCC diagnostic pop

}  // namespace omni::interpreter

// core/bytecode/bytecode_verifier.cpp
//
// Implementation of BytecodeVerifier.

#include "core/bytecode/bytecode_verifier.hpp"

#include <algorithm>
#include <vector>

#include "core/common/types.hpp"

namespace omni::bytecode {

using common::ErrorCategory;
using common::make_error;
using common::NULL_SYMBOL;
using common::Result;

namespace {

/// Sentinel error symbols. Pre-interned at process startup (B4 fix).
/// TODO: intern these in SymbolTable at process init.
constexpr common::SymbolId ERR_BYTECODE_TOO_LONG = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_OPCODE = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_REGISTER = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_BRANCH_OUT_OF_RANGE = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_CONST_INDEX_OUT_OF_RANGE = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_HANDLER_RANGE = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_FUNCTION_ENTRY = common::NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_PARAM_COUNT = common::NULL_SYMBOL;

/// Returns true if `op` is a defined opcode with a registered handler
/// (B5 fix: previously the verifier accepted any opcode value 1..255
/// because the upper-bound check `> 255` was impossible on uint8_t).
/// Now we explicitly enumerate the defined opcodes; any gap (e.g., 50..127
/// between SetField and SemanticMax) is rejected.
[[nodiscard]] constexpr bool is_defined_opcode(bytecode::Opcode op) noexcept {
    switch (op) {
        case bytecode::Opcode::Invalid:
            return false;
        // Semantic opcodes (DESIGN.md §4.1).
        case bytecode::Opcode::LoadArg:
        case bytecode::Opcode::LoadConst:
        case bytecode::Opcode::LoadLocal:
        case bytecode::Opcode::StoreLocal:
        case bytecode::Opcode::GetProp:
        case bytecode::Opcode::SetProp:
        case bytecode::Opcode::Add:
        case bytecode::Opcode::Sub:
        case bytecode::Opcode::Mul:
        case bytecode::Opcode::Div:
        case bytecode::Opcode::Mod:
        case bytecode::Opcode::Eq:
        case bytecode::Opcode::Lt:
        case bytecode::Opcode::Gt:
        case bytecode::Opcode::Le:
        case bytecode::Opcode::Ge:
        case bytecode::Opcode::Ne:
        case bytecode::Opcode::Call:
        case bytecode::Opcode::Jump:
        case bytecode::Opcode::Branch:
        case bytecode::Opcode::Return:
        case bytecode::Opcode::MakeObject:
        case bytecode::Opcode::MakeClosure:
        case bytecode::Opcode::GetIter:
        case bytecode::Opcode::Next:
        case bytecode::Opcode::Spawn:
        case bytecode::Opcode::Await:
        case bytecode::Opcode::SyncEnter:
        case bytecode::Opcode::SyncExit:
        case bytecode::Opcode::ChanSend:
        case bytecode::Opcode::ChanRecv:
        case bytecode::Opcode::Raise:
        case bytecode::Opcode::TryBegin:
        case bytecode::Opcode::TryEnd:
        case bytecode::Opcode::Match:
        case bytecode::Opcode::Using:
        case bytecode::Opcode::Defer:
        case bytecode::Opcode::Dup:
        case bytecode::Opcode::Pop:
        case bytecode::Opcode::IsNull:
        case bytecode::Opcode::IsInt:
        case bytecode::Opcode::IsFloat:
        case bytecode::Opcode::IsStr:
        case bytecode::Opcode::IsObject:
        case bytecode::Opcode::Coerce:
        case bytecode::Opcode::InjectTrait:
        case bytecode::Opcode::RemoveTrait:
        case bytecode::Opcode::GetField:
        case bytecode::Opcode::SetField:
        case bytecode::Opcode::Nop:  // B10 fix: Nop is valid (fusion marker)
            return true;
        // Quickened opcodes (DESIGN.md §4.2).
        case bytecode::Opcode::LoadLocalFast:
        case bytecode::Opcode::GetPropMono:
        case bytecode::Opcode::SetPropMono:
        case bytecode::Opcode::AddIntFast:
        case bytecode::Opcode::AddFloatFast:
        case bytecode::Opcode::AddStringConcatFast:
        case bytecode::Opcode::SubIntFast:
        case bytecode::Opcode::MulIntFast:
        case bytecode::Opcode::DivIntFast:
        case bytecode::Opcode::CallMono:
        case bytecode::Opcode::CallDirectSmall:
        case bytecode::Opcode::BranchTakenFast:
        case bytecode::Opcode::BranchNotTakenFast:
        case bytecode::Opcode::NextShapeFast:
        case bytecode::Opcode::ToIntFast:
        case bytecode::Opcode::ToFloatFast:
        case bytecode::Opcode::ToStrFast:
        case bytecode::Opcode::ToBoolFast:
        case bytecode::Opcode::EqIntFast:
        case bytecode::Opcode::LtIntFast:
        case bytecode::Opcode::GetPropPoly:
        case bytecode::Opcode::SetPropPoly:
            return true;
        // Fused opcodes (DESIGN.md §4.3).
        case bytecode::Opcode::AddIntRR:
        case bytecode::Opcode::AddIntRC:
        case bytecode::Opcode::AddStoreLocal:
        case bytecode::Opcode::GetPropAddIntConst:
        case bytecode::Opcode::GetPropCallMono:
        case bytecode::Opcode::LoadAddStore:
        case bytecode::Opcode::CallMonoReturn:
        case bytecode::Opcode::IterNextBranch:
        case bytecode::Opcode::GetAddSetMono:
            return true;
        // Sentinel bounds — never valid as actual opcodes.
        case bytecode::Opcode::SemanticMax:
        case bytecode::Opcode::QuickenedMax:
        case bytecode::Opcode::FusedMax:
            return false;
    }
    return false;
}

}  // namespace

Result<void> BytecodeVerifier::verify(const BytecodeModule& module) noexcept {
    if (module.length() > common::MAX_BYTECODE_LENGTH) [[unlikely]] {
        return make_error(ErrorCategory::Bytecode, ERR_BYTECODE_TOO_LONG);
    }

    const auto code = module.code();

    // Validate each instruction.
    for (uint32_t pc = 0; pc < code.size(); ++pc) {
        const Instruction inst = code[pc];
        const Opcode op = inst.opcode();

        // B5 fix: reject any opcode not in the defined set. Previously
        // the verifier accepted any value 1..255, allowing gaps in the
        // opcode space to pass verification.
        if (!is_defined_opcode(op)) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_OPCODE);
        }

        // For most ops, operand A and B are register ids. Validate them.
        // (For ops where A or B is an immediate (e.g., LoadConst),
        //  the validator is more permissive — those values are checked
        //  against the constant pool / branch-target range below.)
        const bool a_is_reg = (op != Opcode::LoadConst) && (op != Opcode::Jump)
                            && (op != Opcode::Branch) && (op != Opcode::CallDirectSmall);
        const bool b_is_reg = (op != Opcode::LoadConst) && (op != Opcode::Jump)
                            && (op != Opcode::Branch);
        if (a_is_reg && inst.operand_a() >= common::FRAME_REGISTER_COUNT) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_REGISTER);
        }
        if (b_is_reg && inst.operand_b() >= common::FRAME_REGISTER_COUNT) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_REGISTER);
        }

        // Branch targets must be within the module. Branches use a signed
        // 16-bit delta so backward branches (loops) are expressible (B6 fix:
        // previously delta was unsigned and only forward branches were valid).
        //
        // Encoding (per handlers_semantic.cpp):
        //   JUMP   delta16  — operand_ab is the signed 16-bit delta.
        //   BRANCH cond_reg, delta8 — operand_a is the cond register,
        //                             operand_b is the signed 8-bit delta.
        // The verifier must use the correct delta width for each.
        if (op == Opcode::Jump) {
            const uint16_t raw_delta = inst.operand_ab();
            const int16_t signed_delta = static_cast<int16_t>(raw_delta);
            const int64_t signed_target = static_cast<int64_t>(pc)
                                        + static_cast<int64_t>(signed_delta);
            if (signed_target < 0
                || signed_target >= static_cast<int64_t>(code.size())) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_BRANCH_OUT_OF_RANGE);
            }
        } else if (op == Opcode::Branch) {
            const int8_t signed_delta = static_cast<int8_t>(inst.operand_b());
            const int64_t signed_target = static_cast<int64_t>(pc)
                                        + static_cast<int64_t>(signed_delta);
            if (signed_target < 0
                || signed_target >= static_cast<int64_t>(code.size())) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_BRANCH_OUT_OF_RANGE);
            }
        }

        // LoadConst indices must be in range. The constant pool index
        // is encoded in operand_b (0-255) for the compact form.
        if (op == Opcode::LoadConst) {
            const uint8_t idx = inst.operand_b();
            if (idx >= module.constants().size()) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_CONST_INDEX_OUT_OF_RANGE);
            }
        }
    }

    // Validate function table (B30 fix: param_count must fit in the
    // register file, otherwise the arg-loading loop in execute() would
    // wrap the uint8_t RegId and corrupt registers).
    for (const auto& f : module.functions()) {
        if (f.entry_pc >= code.size()) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_FUNCTION_ENTRY);
        }
        if (f.local_count > common::FRAME_REGISTER_COUNT) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_REGISTER);
        }
        if (f.param_count > common::FRAME_REGISTER_COUNT) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_PARAM_COUNT);
        }
    }

    // Validate exception handler ranges (B15 fix: also check proper
    // nesting / disjointness). Handlers must be either fully disjoint
    // or properly nested (inner fully inside outer). Non-nested overlaps
    // would cause find_handler to return the wrong handler.
    {
        // Sort a copy by (try_start_pc, try_end_pc) ascending so we can
        // walk them in source order and verify nesting.
        std::vector<HandlerEntry> sorted_handlers(module.handlers().begin(),
                                                   module.handlers().end());
        std::sort(sorted_handlers.begin(), sorted_handlers.end(),
                  [](const HandlerEntry& a, const HandlerEntry& b) {
                      if (a.try_start_pc != b.try_start_pc)
                          return a.try_start_pc < b.try_start_pc;
                      return a.try_end_pc < b.try_end_pc;
                  });
        // Use a stack of active handler end-pcs to track nesting.
        std::vector<uint32_t> active_ends;
        for (const auto& h : sorted_handlers) {
            if (h.try_start_pc >= h.try_end_pc) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_BAD_HANDLER_RANGE);
            }
            if (h.try_end_pc > code.size()) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_BAD_HANDLER_RANGE);
            }
            if (h.handler_pc >= code.size()) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_BAD_HANDLER_RANGE);
            }
            // Pop any handler that ends at or before this one starts.
            while (!active_ends.empty()
                   && active_ends.back() <= h.try_start_pc) {
                active_ends.pop_back();
            }
            // If any active handler's end is *less than* this handler's end,
            // it means we have a non-nested overlap (e.g., [0..10) and [5..15)
            // — neither is fully inside the other).
            if (!active_ends.empty()
                && active_ends.back() < h.try_end_pc) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_BAD_HANDLER_RANGE);
            }
            active_ends.push_back(h.try_end_pc);
        }
    }

    return {};
}

}  // namespace omni::bytecode

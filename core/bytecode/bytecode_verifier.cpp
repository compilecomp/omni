// core/bytecode/bytecode_verifier.cpp
//
// Implementation of BytecodeVerifier.

#include "core/bytecode/bytecode_verifier.hpp"

#include "core/common/types.hpp"

namespace omni::bytecode {

using common::ErrorCategory;
using common::make_error;
using common::NULL_SYMBOL;
using common::Result;

namespace {

/// Sentinel error symbols. In a real implementation these would be
/// interned at process startup; for now we use NULL_SYMBOL and rely on
/// the category for diagnostics.
constexpr common::SymbolId ERR_BYTECODE_TOO_LONG = NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_OPCODE = NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_REGISTER = NULL_SYMBOL;
constexpr common::SymbolId ERR_BRANCH_OUT_OF_RANGE = NULL_SYMBOL;
constexpr common::SymbolId ERR_CONST_INDEX_OUT_OF_RANGE = NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_HANDLER_RANGE = NULL_SYMBOL;
constexpr common::SymbolId ERR_BAD_FUNCTION_ENTRY = NULL_SYMBOL;

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

        // Opcode value must be valid.
        const uint8_t opv = static_cast<uint8_t>(op);
        if (opv == 0 || (opv > static_cast<uint8_t>(Opcode::FusedMax))) [[unlikely]] {
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

        // Branch targets must be within the module.
        if (op == Opcode::Jump || op == Opcode::Branch) {
            const uint16_t delta = inst.operand_ab();
            const uint32_t target = pc + delta;
            if (target >= code.size()) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_BRANCH_OUT_OF_RANGE);
            }
        }

        // LoadConst indices must be in range.
        if (op == Opcode::LoadConst) {
            const uint16_t idx = inst.operand_ab();
            if (idx >= module.constants().size()) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_CONST_INDEX_OUT_OF_RANGE);
            }
        }
    }

    // Validate function table.
    for (const auto& f : module.functions()) {
        if (f.entry_pc >= code.size()) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_FUNCTION_ENTRY);
        }
        if (f.local_count > common::FRAME_REGISTER_COUNT) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_REGISTER);
        }
    }

    // Validate exception handler ranges.
    for (const auto& h : module.handlers()) {
        if (h.try_start_pc >= h.try_end_pc) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_HANDLER_RANGE);
        }
        if (h.try_end_pc > code.size()) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_HANDLER_RANGE);
        }
        if (h.handler_pc >= code.size()) [[unlikely]] {
            return make_error(ErrorCategory::Bytecode, ERR_BAD_HANDLER_RANGE);
        }
    }

    return {};
}

}  // namespace omni::bytecode

// core/bytecode/instruction.hpp
//
// Fixed-width instruction encoding for the Tier 0 interpreter.
//
// Purpose:
//   Implements LAWS.md Part I: "24-bit fixed-width instructions with 256
//   virtual registers per frame".
//
//   Encoding (24 bits, 3 bytes):
//     byte 0 (MSB): opcode (8 bits)
//     byte 1:       operand A (8 bits) — typically a destination or
//                   a register id (RegId, 0..255)
//     byte 2:       operand B (8 bits) — typically a source register,
//                   an immediate, or a small constant index
//
//   For instructions needing more than two operands (e.g., CALL with
//   function + arg count + args), we use a separate extended form
//   (InstructionExt) that consumes two consecutive 24-bit slots.
//   The opcode itself indicates the extended form (no separate flag bit).
//
// Invariants:
//   - Instruction is exactly 3 bytes (24 bits).
//   - Bit layout is portable across hosts (no endianness assumptions
//     in the field accessors; we explicitly pack/unpack bytes).
//   - The encoding is dense: small functions fit in a few hundred bytes.
//
// Edge cases:
//   - Branch targets are stored as a 16-bit pc delta (operand A:B combined).
//     Maximum branch distance is 65535 instructions; longer branches
//     use the extended form (Branch with a 32-bit target via two slots).
//   - LoadConst references a constant pool index (operand A:B combined,
//     16-bit, max 65535 constants per module).
//
// Cross-references:
//   - LAWS.md Part I (24-bit instructions, 256 registers per frame)
//   - DESIGN.md §4.1, §4.2, §4.3 (semantic, quickened, fused opcodes)
//   - LAWS.md Rule 31 (cache versioning: instruction format hash in profile manifest)

#pragma once

#include <cstdint>

#include "core/bytecode/opcode.hpp"
#include "core/common/types.hpp"

namespace omni::bytecode {

/// Compact 24-bit instruction, packed as 3 bytes plus 1 byte of padding
/// to make it 4-byte aligned. The padding is required for atomic
/// 32-bit access (B9 fix: the quickening overlay writes via atomic
/// 32-bit stores, which require 4-byte alignment).
class Instruction {
public:
    constexpr Instruction() : bytes_{0, 0, 0, 0} {}
    constexpr Instruction(Opcode op, uint8_t a, uint8_t b) noexcept
        : bytes_{static_cast<uint8_t>(op), a, b, 0} {}

    [[nodiscard]] constexpr Opcode opcode() const noexcept {
        return static_cast<Opcode>(bytes_[0]);
    }
    [[nodiscard]] constexpr uint8_t operand_a() const noexcept { return bytes_[1]; }
    [[nodiscard]] constexpr uint8_t operand_b() const noexcept { return bytes_[2]; }

    /// 16-bit immediate, formed by combining operands A and B (A is high
    /// byte, B is low byte). Used for branch targets and constant pool indices.
    [[nodiscard]] constexpr uint16_t operand_ab() const noexcept {
        return static_cast<uint16_t>(static_cast<uint16_t>(bytes_[1])
                                    << common::BYTE_SHIFT_1)
             | bytes_[2];
    }

    void set_opcode(Opcode op) noexcept { bytes_[0] = static_cast<uint8_t>(op); }
    void set_operand_a(uint8_t a) noexcept { bytes_[1] = a; }
    void set_operand_b(uint8_t b) noexcept { bytes_[2] = b; }

    /// Raw 32-bit value as a uint32_t (high byte 0). The low 24 bits
    /// are the instruction; the high 8 bits are padding.
    [[nodiscard]] constexpr uint32_t raw() const noexcept {
        return (uint32_t{bytes_[0]} << common::BYTE_SHIFT_2)
             | (uint32_t{bytes_[1]} << common::BYTE_SHIFT_1)
             | bytes_[2];
    }

    /// Pack into a single uint32_t for atomic store (B9 fix).
    [[nodiscard]] constexpr uint32_t packed() const noexcept {
        return (uint32_t{bytes_[0]} << common::BYTE_SHIFT_2)
             | (uint32_t{bytes_[1]} << common::BYTE_SHIFT_1)
             | (uint32_t{bytes_[2]} << common::BYTE_SHIFT_0)
             | (uint32_t{bytes_[3]} << common::BYTE_SHIFT_3);
    }

    /// Unpack from a uint32_t (B9 fix).
    static constexpr Instruction from_packed(uint32_t v) noexcept {
        Instruction i;
        i.bytes_[0] = static_cast<uint8_t>((v >> common::BYTE_SHIFT_2) & 0xFFu);
        i.bytes_[1] = static_cast<uint8_t>((v >> common::BYTE_SHIFT_1) & 0xFFu);
        i.bytes_[2] = static_cast<uint8_t>((v >> common::BYTE_SHIFT_0) & 0xFFu);
        i.bytes_[3] = static_cast<uint8_t>((v >> common::BYTE_SHIFT_3) & 0xFFu);
        return i;
    }

private:
    uint8_t bytes_[4];
};
static_assert(sizeof(Instruction) == 4, "Instruction must be 4 bytes (24 bits + padding for atomicity)");

/// Extended instruction (two consecutive 24-bit slots).
/// Used for ops needing more than 16 bits of immediate (large branches,
/// large constant indices, multi-arg calls).
class InstructionExt {
public:
    constexpr InstructionExt() = default;
    constexpr InstructionExt(Opcode op, uint8_t a, uint8_t b,
                              uint8_t c, uint8_t d, uint8_t e) noexcept
        : first_{op, a, b}, second_{static_cast<Opcode>(0), c, d}, extra_{e} {}

    [[nodiscard]] constexpr Opcode opcode() const noexcept { return first_.opcode(); }
    [[nodiscard]] constexpr uint8_t operand_a() const noexcept { return first_.operand_a(); }
    [[nodiscard]] constexpr uint8_t operand_b() const noexcept { return first_.operand_b(); }
    [[nodiscard]] constexpr uint8_t operand_c() const noexcept { return second_.operand_b(); }
    [[nodiscard]] constexpr uint8_t operand_d() const noexcept { return second_.operand_a(); }
    [[nodiscard]] constexpr uint8_t operand_e() const noexcept { return extra_; }

    /// 32-bit immediate formed from operands A:B:C:D (A is MSB).
    [[nodiscard]] constexpr uint32_t operand_32() const noexcept {
        return (uint32_t{first_.operand_a()} << common::BYTE_SHIFT_3)
             | (uint32_t{first_.operand_b()} << common::BYTE_SHIFT_2)
             | (uint32_t{second_.operand_a()} << common::BYTE_SHIFT_1)
             | second_.operand_b();
    }

private:
    Instruction first_;
    Instruction second_;
    uint8_t extra_{0};
};
static_assert(sizeof(InstructionExt) == 9, "InstructionExt must be 9 bytes (2*4 + 1)");

}  // namespace omni::bytecode

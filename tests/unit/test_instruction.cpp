// tests/unit/test_instruction.cpp
//
// Unit tests for core/bytecode/instruction.hpp.
//
// Covers:
//   - 24-bit Instruction size invariant.
//   - Opcode packing/unpacking.
//   - Operand accessors.
//   - 16-bit combined operand AB.
//   - Extended instruction layout.

#include "core/bytecode/instruction.hpp"
#include "core/bytecode/opcode.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace omni::bytecode;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void test_size_invariant() {
    CHECK(sizeof(Instruction) == 3);   // 24 bits packed as 3 bytes
    CHECK(sizeof(InstructionExt) == 7);
}

static void test_opcode_accessors() {
    Instruction i{Opcode::Add, 1, 2};
    CHECK(i.opcode() == Opcode::Add);
    CHECK(i.operand_a() == 1);
    CHECK(i.operand_b() == 2);
    CHECK(i.operand_ab() == 0x0102);
}

static void test_opcode_setters() {
    Instruction i{};
    i.set_opcode(Opcode::GetProp);
    i.set_operand_a(5);
    i.set_operand_b(10);
    CHECK(i.opcode() == Opcode::GetProp);
    CHECK(i.operand_a() == 5);
    CHECK(i.operand_b() == 10);
}

static void test_raw_value() {
    Instruction i{Opcode::Call, 0xAB, 0xCD};
    CHECK(i.raw() == ((uint32_t{static_cast<uint8_t>(Opcode::Call)} << 16)
                       | (uint32_t{0xAB} << 8) | 0xCD));
}

static void test_opcode_classifiers() {
    CHECK(is_semantic(Opcode::Add));
    CHECK(is_semantic(Opcode::Return));
    CHECK(!is_semantic(Opcode::AddIntFast));
    CHECK(is_quickened(Opcode::AddIntFast));
    CHECK(is_quickened(Opcode::GetPropMono));
    CHECK(!is_quickened(Opcode::Add));
    CHECK(is_fused(Opcode::AddIntRR));
    CHECK(is_fused(Opcode::IterNextBranch));
    CHECK(!is_fused(Opcode::Add));
}

static void test_fallback_mapping() {
    // Quickened -> semantic.
    CHECK(fallback_for(Opcode::AddIntFast) == Opcode::Add);
    CHECK(fallback_for(Opcode::GetPropMono) == Opcode::GetProp);
    CHECK(fallback_for(Opcode::CallMono) == Opcode::Call);
    // Fused -> semantic.
    CHECK(fallback_for(Opcode::AddIntRR) == Opcode::Add);
    CHECK(fallback_for(Opcode::IterNextBranch) == Opcode::Next);
    // Semantic -> itself.
    CHECK(fallback_for(Opcode::Add) == Opcode::Add);
    CHECK(fallback_for(Opcode::Return) == Opcode::Return);
}

int main() {
    test_size_invariant();
    test_opcode_accessors();
    test_opcode_setters();
    test_raw_value();
    test_opcode_classifiers();
    test_fallback_mapping();
    if (g_failures == 0) {
        std::printf("OK: instruction (%d checks passed)\n", 6);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d checks failed\n", g_failures);
    return 1;
}

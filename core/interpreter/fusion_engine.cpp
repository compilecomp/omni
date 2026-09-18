// core/interpreter/fusion_engine.cpp

#include "core/interpreter/fusion_engine.hpp"

#include "core/bytecode/instruction.hpp"
#include "core/bytecode/opcode.hpp"
#include "core/common/types.hpp"

namespace omni::interpreter {

using namespace bytecode;
using namespace common;

uint32_t FusionEngine::visit_frame(InterpFrame& /*frame*/,
                                      BytecodeModule& /*module*/,
                                      Interpreter& /*interp*/) noexcept {
    // B8 fix: still a stub; full implementation would walk profiles and
    // try each fusion pattern at hot consecutive-site groups.
    return 0;
}

bool FusionEngine::is_fusible(BytecodeModule& module, BytecodePC pc,
                                unsigned sequence_length) noexcept {
    auto code = module.code();
    if (pc + sequence_length > code.size()) return false;

    // DESIGN.md §5.7 fusion conditions (B27 fix: aligned with the
    // spec's "Do not initially fuse across" list).
    for (unsigned i = 1; i < sequence_length; ++i) {
        const Opcode op = code[pc + i].opcode();
        switch (op) {
            case Opcode::Call:               // arbitrary calls
            case Opcode::TryBegin:           // exception handlers
            case Opcode::TryEnd:
            case Opcode::Branch:             // simple branch behavior only
            case Opcode::Jump:
            case Opcode::Return:
            case Opcode::Raise:
            case Opcode::Spawn:              // task spawn
            case Opcode::Await:              // await points
            case Opcode::SyncExit:           // sync exits
            case Opcode::InjectTrait:        // trait injection
            case Opcode::RemoveTrait:        // trait injection
            case Opcode::SetProp:            // GC-sensitive publication
            case Opcode::Match:              // dispatches through traits
                return false;
            default:
                break;
        }
    }
    return true;
}

// Helper: atomically write a fused sequence at pc, marking subsequent
// slots as Opcode::Nop (B10 fix: Nop is verifier-accepted).
static void write_fused(BytecodeModule& module, BytecodePC pc,
                          Instruction fused, unsigned slot_count) noexcept {
    auto atomic_code = module.code_atomic_mut();
    atomic_code[pc].store(fused.packed(), std::memory_order_release);
    Instruction nop{};
    nop.set_opcode(Opcode::Nop);
    const uint32_t nop_packed = nop.packed();
    for (unsigned i = 1; i < slot_count; ++i) {
        atomic_code[pc + i].store(nop_packed, std::memory_order_release);
    }
}

bool FusionEngine::try_fuse_load_add_store(BytecodeModule& module,
                                              BytecodePC pc) noexcept {
    // LOAD_LOCAL rdst, src1
    // ADD       rdst, rdst, src2
    // STORE_LOCAL dst, rdst
    // -> LOAD_ADD_STORE src1, src2, dst
    auto code = module.code();
    if (pc + 3 > code.size()) return false;
    if (!is_fusible(module, pc, 3)) return false;
    if (code[pc].opcode() != Opcode::LoadLocal) return false;
    if (code[pc + 1].opcode() != Opcode::Add) return false;
    if (code[pc + 2].opcode() != Opcode::StoreLocal) return false;
    // B25 fix: validate register dataflow.
    const uint8_t load_dst = code[pc].operand_a();
    const uint8_t add_dst = code[pc + 1].operand_a();
    const uint8_t add_src1 = code[pc + 1].operand_a();
    const uint8_t store_src = code[pc + 2].operand_b();
    if (load_dst != add_dst) return false;
    if (load_dst != add_src1) return false;
    if (load_dst != store_src) return false;

    Instruction fused = code[pc];
    fused.set_opcode(Opcode::LoadAddStore);
    write_fused(module, pc, fused, 3);
    return true;
}

bool FusionEngine::try_fuse_get_prop_add_int_const(BytecodeModule& module,
                                                      BytecodePC pc) noexcept {
    auto code = module.code();
    if (pc + 2 > code.size()) return false;
    if (!is_fusible(module, pc, 2)) return false;
    if (code[pc].opcode() != Opcode::GetProp) return false;
    if (code[pc + 1].opcode() != Opcode::AddIntFast) return false;
    // B25 fix: validate that the destination of GetProp is the source
    // of AddIntFast.
    if (code[pc].operand_a() != code[pc + 1].operand_a()) return false;
    if (code[pc].operand_a() != code[pc + 1].operand_b()) return false;

    Instruction fused = code[pc];
    fused.set_opcode(Opcode::GetPropAddIntConst);
    write_fused(module, pc, fused, 2);
    return true;
}

bool FusionEngine::try_fuse_get_prop_call_mono(BytecodeModule& module,
                                                  BytecodePC pc) noexcept {
    auto code = module.code();
    if (pc + 2 > code.size()) return false;
    // B26 fix: CallMono is a call, but DESIGN.md §4.3 explicitly lists
    // GetPropCallMono as a valid fused op. Skip is_fusible (it would
    // forbid Call), but check the dataflow.
    if (code[pc].opcode() != Opcode::GetPropMono) return false;
    if (code[pc + 1].opcode() != Opcode::CallMono) return false;
    // B25 fix: validate the register dataflow.
    if (code[pc].operand_a() != code[pc + 1].operand_a()) return false;

    Instruction fused = code[pc];
    fused.set_opcode(Opcode::GetPropCallMono);
    write_fused(module, pc, fused, 2);
    return true;
}

bool FusionEngine::try_fuse_iter_next_branch(BytecodeModule& module,
                                                BytecodePC pc) noexcept {
    auto code = module.code();
    if (pc + 2 > code.size()) return false;
    if (code[pc].opcode() != Opcode::Next) return false;
    if (code[pc + 1].opcode() != Opcode::Branch) return false;
    // B26 fix: ITER_NEXT_BRANCH is explicitly listed in DESIGN.md §4.3,
    // so we don't call is_fusible. But check the preceding op to ensure
    // we're not crossing a forbidden boundary.
    if (pc > 0) {
        Opcode prev = code[pc - 1].opcode();
        switch (prev) {
            case Opcode::Call:
            case Opcode::TryBegin:
            case Opcode::TryEnd:
            case Opcode::Raise:
            case Opcode::Spawn:
            case Opcode::Await:
            case Opcode::SyncExit:
            case Opcode::InjectTrait:
            case Opcode::RemoveTrait:
                return false;
            default:
                break;
        }
    }

    Instruction fused = code[pc];
    fused.set_opcode(Opcode::IterNextBranch);
    write_fused(module, pc, fused, 2);
    return true;
}

}  // namespace omni::interpreter

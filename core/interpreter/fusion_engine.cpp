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
    // The full implementation would walk the frame's SiteProfiles,
    // find hot consecutive sites, and attempt fusion. For the prototype
    // we just return 0.
    return 0;
}

bool FusionEngine::is_fusible(BytecodeModule& module, BytecodePC pc,
                                unsigned sequence_length) noexcept {
    auto code = module.code();
    if (pc + sequence_length > code.size()) return false;

    // DESIGN.md §5.7 fusion conditions:
    // - sequence is hot (checked by caller via SiteProfile)
    // - side effects are closed
    // - no arbitrary call in the middle (CALL is forbidden mid-sequence)
    // - no exception edge in the middle (TryBegin/TryEnd forbidden)
    // - no GC-sensitive publication in the middle (we approximate by
    //   forbidding SetProp with a global target — but we don't have
    //   that info here, so we forbid SetProp entirely mid-sequence)
    // - branch behavior is simple (no Branch mid-sequence)
    // - all subops have compatible fallback PCs (true by construction)
    for (unsigned i = 1; i < sequence_length; ++i) {
        const Opcode op = code[pc + i].opcode();
        switch (op) {
            // Forbidden mid-sequence opcodes.
            case Opcode::Call:
            case Opcode::TryBegin:
            case Opcode::TryEnd:
            case Opcode::Branch:
            case Opcode::Jump:
            case Opcode::Return:
            case Opcode::Raise:
            case Opcode::Spawn:
            case Opcode::Await:
            case Opcode::SyncExit:
            case Opcode::InjectTrait:
            case Opcode::RemoveTrait:
            case Opcode::SetProp:
                return false;
            default:
                break;
        }
    }
    return true;
}

bool FusionEngine::try_fuse_load_add_store(BytecodeModule& module,
                                              BytecodePC pc) noexcept {
    // LOAD_LOCAL rdst, src1
    // ADD       rdst, rdst, src2
    // STORE_LOCAL dst, rdst
    // -> LOAD_ADD_STORE src1, src2, dst
    auto code = module.code_mut();
    if (pc + 3 > code.size()) return false;
    if (!is_fusible(module, pc, 3)) return false;
    if (code[pc].opcode() != Opcode::LoadLocal) return false;
    if (code[pc + 1].opcode() != Opcode::Add) return false;
    if (code[pc + 2].opcode() != Opcode::StoreLocal) return false;

    code[pc].set_opcode(Opcode::LoadAddStore);
    // The next two instructions become No-ops in the dispatch loop
    // (the fused handler advances pc by 3). For simplicity we set them
    // to the Invalid opcode; the dispatcher recognizes Invalid and
    // treats it as a 1-slot skip.
    code[pc + 1].set_opcode(Opcode::Invalid);
    code[pc + 2].set_opcode(Opcode::Invalid);
    return true;
}

bool FusionEngine::try_fuse_get_prop_add_int_const(BytecodeModule& module,
                                                      BytecodePC pc) noexcept {
    // GET_PROP rdst, obj, "x"
    // ADD_INT_FAST rdst, rdst, const
    // -> GET_PROP_ADD_INT_CONST obj, "x", const
    auto code = module.code_mut();
    if (pc + 2 > code.size()) return false;
    if (!is_fusible(module, pc, 2)) return false;
    if (code[pc].opcode() != Opcode::GetProp) return false;
    if (code[pc + 1].opcode() != Opcode::AddIntFast) return false;

    code[pc].set_opcode(Opcode::GetPropAddIntConst);
    code[pc + 1].set_opcode(Opcode::Invalid);
    return true;
}

bool FusionEngine::try_fuse_get_prop_call_mono(BytecodeModule& module,
                                                  BytecodePC pc) noexcept {
    auto code = module.code_mut();
    if (pc + 2 > code.size()) return false;
    // CallMono is a call — but we allow this specific fusion (it is
    // explicitly listed in DESIGN.md §4.3).
    if (code[pc].opcode() != Opcode::GetPropMono) return false;
    if (code[pc + 1].opcode() != Opcode::CallMono) return false;

    code[pc].set_opcode(Opcode::GetPropCallMono);
    code[pc + 1].set_opcode(Opcode::Invalid);
    return true;
}

bool FusionEngine::try_fuse_iter_next_branch(BytecodeModule& module,
                                                BytecodePC pc) noexcept {
    auto code = module.code_mut();
    if (pc + 2 > code.size()) return false;
    if (code[pc].opcode() != Opcode::Next) return false;
    if (code[pc + 1].opcode() != Opcode::Branch) return false;

    code[pc].set_opcode(Opcode::IterNextBranch);
    code[pc + 1].set_opcode(Opcode::Invalid);
    return true;
}

}  // namespace omni::interpreter

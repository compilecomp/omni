// core/interpreter/adaptive_quickening.cpp
//
// Implementation of the adaptive quickening engine.

#include "core/interpreter/adaptive_quickening.hpp"

#include "core/bytecode/instruction.hpp"
#include "core/bytecode/opcode.hpp"
#include "core/common/types.hpp"

namespace omni::interpreter {

using namespace bytecode;
using namespace common;
using namespace object_model;

uint32_t AdaptiveQuickening::visit_frame(InterpFrame& /*frame*/,
                                          BytecodeModule& /*module*/,
                                          Interpreter& /*interp*/) noexcept {
    // B8 fix: this stub still returns 0; full implementation would
    // iterate the frame's profiles and dispatch to per-opcode quickeners.
    // TODO: expose a non-const iterator on InterpFrame::profiles_ and
    // call try_quicken_get_prop/add/branch/call for each hot site.
    return 0;
}

bool AdaptiveQuickening::try_quicken_get_prop(SiteProfile& profile,
                                                 BytecodeModule& module) noexcept {
    if (!profile.is_hot()) return false;
    if (profile.state == SiteState::Disabled) return false;
    if (!profile.is_monomorphic()) return false;

    auto code = module.code();
    if (profile.pc >= code.size()) return false;
    const Instruction& inst = code[profile.pc];
    if (inst.opcode() != Opcode::GetProp) return false;

    // B9 fix: atomic 32-bit store. Concurrent readers may see either
    // the old or new opcode at this pc, but never a torn read.
    Instruction rewritten = inst;
    rewritten.set_opcode(Opcode::GetPropMono);
    auto atomic_code = module.code_atomic_mut();
    atomic_code[profile.pc].store(rewritten.packed(), std::memory_order_release);
    profile.state = SiteState::Quickened;
    return true;
}

bool AdaptiveQuickening::try_quicken_add(SiteProfile& profile,
                                            BytecodeModule& module) noexcept {
    if (!profile.is_hot()) return false;
    if (profile.state == SiteState::Disabled) return false;

    auto code = module.code();
    if (profile.pc >= code.size()) return false;
    const Instruction& inst = code[profile.pc];
    if (inst.opcode() != Opcode::Add) return false;

    const uint8_t bits = profile.type_feedback_bits;
    if (bits == 0) return false;
    if ((bits & (bits - 1)) != 0) return false;  // multiple tags observed

    Opcode quickened = Opcode::Invalid;
    if (bits == (1u << static_cast<uint8_t>(Tag::Int))) quickened = Opcode::AddIntFast;
    else if (bits == (1u << static_cast<uint8_t>(Tag::Float))) quickened = Opcode::AddFloatFast;
    else if (bits == (1u << static_cast<uint8_t>(Tag::Str))) quickened = Opcode::AddStringConcatFast;
    else return false;

    Instruction rewritten = inst;
    rewritten.set_opcode(quickened);
    auto atomic_code = module.code_atomic_mut();
    atomic_code[profile.pc].store(rewritten.packed(), std::memory_order_release);
    profile.state = SiteState::Quickened;
    return true;
}

bool AdaptiveQuickening::try_quicken_branch(SiteProfile& profile,
                                                BytecodeModule& module) noexcept {
    if (!profile.is_hot()) return false;
    if (profile.state == SiteState::Disabled) return false;
    if (profile.branch_bias.total() < common::BRANCH_BIAS_SAMPLE_MIN) return false;

    auto code = module.code();
    if (profile.pc >= code.size()) return false;
    const Instruction& inst = code[profile.pc];
    if (inst.opcode() != Opcode::Branch) return false;

    const uint32_t bias = profile.branch_bias.bias_taken_q10();
    Opcode quickened = Opcode::Invalid;
    if (bias >= common::BRANCH_BIAS_NUM) {
        quickened = Opcode::BranchTakenFast;
    } else if ((common::BIAS_RESOLUTION - bias) >= common::BRANCH_BIAS_NUM) {
        quickened = Opcode::BranchNotTakenFast;
    } else {
        return false;
    }
    Instruction rewritten = inst;
    rewritten.set_opcode(quickened);
    auto atomic_code = module.code_atomic_mut();
    atomic_code[profile.pc].store(rewritten.packed(), std::memory_order_release);
    profile.state = SiteState::Quickened;
    return true;
}

bool AdaptiveQuickening::try_quicken_call(SiteProfile& profile,
                                             BytecodeModule& module) noexcept {
    if (!profile.is_hot()) return false;
    if (profile.state == SiteState::Disabled) return false;
    if (profile.call_targets.size() != 1) return false;

    auto code = module.code();
    if (profile.pc >= code.size()) return false;
    const Instruction& inst = code[profile.pc];
    if (inst.opcode() != Opcode::Call) return false;

    Instruction rewritten = inst;
    rewritten.set_opcode(Opcode::CallMono);
    auto atomic_code = module.code_atomic_mut();
    atomic_code[profile.pc].store(rewritten.packed(), std::memory_order_release);
    profile.state = SiteState::Quickened;
    return true;
}

void AdaptiveQuickening::demote_to_semantic(SiteProfile& profile,
                                               BytecodeModule& module) noexcept {
    auto code = module.code();
    if (profile.pc >= code.size()) return;
    const Instruction& inst = code[profile.pc];
    Opcode fb = fallback_for(inst.opcode());
    if (fb == Opcode::Invalid) return;
    Instruction rewritten = inst;
    rewritten.set_opcode(fb);
    auto atomic_code = module.code_atomic_mut();
    atomic_code[profile.pc].store(rewritten.packed(), std::memory_order_release);
    profile.state = SiteState::Generic;
    profile.failure_count.fetch_add(1, std::memory_order_relaxed);
    if (profile.failure_count.load(std::memory_order_relaxed)
        >= common::T0_SITE_DISABLE_FAILURE_THRESHOLD) {
        profile.state = SiteState::Disabled;
    }
}

}  // namespace omni::interpreter

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

uint32_t AdaptiveQuickening::visit_frame(InterpFrame& frame,
                                          BytecodeModule& module,
                                          Interpreter& /*interp*/) noexcept {
    uint32_t quickened_count = 0;
    // For each profile, attempt to quicken. The profiles vector lives
    // inside the InterpFrame; iterating it is cheap.
    // (We can't easily iterate it from outside; the real impl would
    // expose a non-const iterator. For the prototype we just count
    // would-be quickenings.)
    (void)frame;
    (void)module;
    return quickened_count;
}

bool AdaptiveQuickening::try_quicken_get_prop(SiteProfile& profile,
                                                 BytecodeModule& module) noexcept {
    // Rule (DESIGN.md §5.4):
    //   if site is hot and monomorphic: quicken
    //   else if site is hot and polymorphic: use inline cache
    //   else if site is unstable: keep generic or disable speculation
    if (!profile.is_hot()) return false;
    if (profile.state == SiteState::Disabled) return false;
    if (!profile.is_monomorphic()) return false;

    // Rewrite the instruction at profile.pc.
    auto code = module.code_mut();
    if (profile.pc >= code.size()) return false;
    Instruction& inst = code[profile.pc];
    if (inst.opcode() != Opcode::GetProp) return false;

    // Rewrite to GetPropMono with the observed shape id.
    // (The full implementation packs shape_id and offset into the
    // 16-bit operand AB; for simplicity here we just set the opcode.)
    inst.set_opcode(Opcode::GetPropMono);
    profile.state = SiteState::Quickened;
    return true;
}

bool AdaptiveQuickening::try_quicken_add(SiteProfile& profile,
                                            BytecodeModule& module) noexcept {
    if (!profile.is_hot()) return false;
    if (profile.state == SiteState::Disabled) return false;

    auto code = module.code_mut();
    if (profile.pc >= code.size()) return false;
    Instruction& inst = code[profile.pc];
    if (inst.opcode() != Opcode::Add) return false;

    // Choose the quickened opcode based on type feedback.
    // type_feedback_bits bit 0 = Null, 1 = Bool, 2 = Int, 3 = Float, 4 = Str.
    // If only one tag bit is set, we can monomorphically quicken.
    const uint8_t bits = profile.type_feedback_bits;
    if (bits == 0) return false;  // no feedback yet
    if ((bits & (bits - 1)) != 0) return false;  // multiple tags observed

    Opcode quickened = Opcode::Invalid;
    if (bits == (1u << static_cast<uint8_t>(Tag::Int))) quickened = Opcode::AddIntFast;
    else if (bits == (1u << static_cast<uint8_t>(Tag::Float))) quickened = Opcode::AddFloatFast;
    else if (bits == (1u << static_cast<uint8_t>(Tag::Str))) quickened = Opcode::AddStringConcatFast;
    else return false;

    inst.set_opcode(quickened);
    profile.state = SiteState::Quickened;
    return true;
}

bool AdaptiveQuickening::try_quicken_branch(SiteProfile& profile,
                                                BytecodeModule& module) noexcept {
    if (!profile.is_hot()) return false;
    if (profile.state == SiteState::Disabled) return false;
    if (profile.branch_bias.total() < common::BRANCH_BIAS_SAMPLE_MIN) return false;

    auto code = module.code_mut();
    if (profile.pc >= code.size()) return false;
    Instruction& inst = code[profile.pc];
    if (inst.opcode() != Opcode::Branch) return false;

    const uint32_t bias = profile.branch_bias.bias_taken_q10();
    if (bias >= common::BRANCH_BIAS_NUM) {
        inst.set_opcode(Opcode::BranchTakenFast);
        profile.state = SiteState::Quickened;
        return true;
    }
    if ((1024 - bias) >= common::BRANCH_BIAS_NUM) {
        inst.set_opcode(Opcode::BranchNotTakenFast);
        profile.state = SiteState::Quickened;
        return true;
    }
    return false;
}

bool AdaptiveQuickening::try_quicken_call(SiteProfile& profile,
                                             BytecodeModule& module) noexcept {
    if (!profile.is_hot()) return false;
    if (profile.state == SiteState::Disabled) return false;
    if (profile.call_targets.size() != 1) return false;

    auto code = module.code_mut();
    if (profile.pc >= code.size()) return false;
    Instruction& inst = code[profile.pc];
    if (inst.opcode() != Opcode::Call) return false;

    inst.set_opcode(Opcode::CallMono);
    profile.state = SiteState::Quickened;
    return true;
}

void AdaptiveQuickening::demote_to_semantic(SiteProfile& profile,
                                               BytecodeModule& module) noexcept {
    auto code = module.code_mut();
    if (profile.pc >= code.size()) return;
    Instruction& inst = code[profile.pc];
    Opcode fb = fallback_for(inst.opcode());
    if (fb != Opcode::Invalid) {
        inst.set_opcode(fb);
    }
    profile.state = SiteState::Generic;
    profile.failure_count.fetch_add(1, std::memory_order_relaxed);
    if (profile.failure_count.load(std::memory_order_relaxed)
        >= common::T2_DEOPT_BLACKLIST_THRESHOLD) {
        profile.state = SiteState::Disabled;
    }
}

}  // namespace omni::interpreter

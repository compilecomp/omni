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
    // Walk every profile in the frame. For each hot profile, dispatch
    // to the per-opcode quickener based on the current opcode at
    // profile.pc. Sites already in Disabled or Quickened state are
    // skipped.
    //
    // Note: we iterate by index because try_quicken_* may mutate the
    // profile state in place, but they do not invalidate the vector.
    auto& profiles = frame.profiles();
    const size_t n = profiles.size();
    for (size_t i = 0; i < n; ++i) {
        SiteProfile& p = profiles[i];
        if (p.state == SiteState::Disabled) continue;
        if (p.state == SiteState::Quickened) continue;
        if (p.state == SiteState::Fused) continue;
        if (!p.is_hot()) continue;
        // Read the current opcode at this pc.
        auto code = module.code();
        if (p.pc >= code.size()) continue;
        const Opcode op = code[p.pc].opcode();
        bool ok = false;
        switch (op) {
            case Opcode::GetProp:
                ok = try_quicken_get_prop(p, module);
                break;
            case Opcode::Add:
                ok = try_quicken_add(p, module);
                break;
            case Opcode::Sub: {
                // Inline the Sub quickening: Sub -> SubIntFast if the
                // type feedback shows only Int.
                if (p.type_feedback_bits == (1u << static_cast<uint8_t>(Tag::Int))) {
                    Instruction rewritten = code[p.pc];
                    rewritten.set_opcode(Opcode::SubIntFast);
                    auto atomic_code = module.code_atomic_mut();
                    atomic_code[p.pc].store(rewritten.packed(), std::memory_order_release);
                    p.state = SiteState::Quickened;
                    ok = true;
                } else {
                    ok = false;
                }
                break;
            }
            case Opcode::Mul: {
                if (p.type_feedback_bits == (1u << static_cast<uint8_t>(Tag::Int))) {
                    Instruction rewritten = code[p.pc];
                    rewritten.set_opcode(Opcode::MulIntFast);
                    auto atomic_code = module.code_atomic_mut();
                    atomic_code[p.pc].store(rewritten.packed(), std::memory_order_release);
                    p.state = SiteState::Quickened;
                    ok = true;
                } else {
                    ok = false;
                }
                break;
            }
            case Opcode::Div: {
                if (p.type_feedback_bits == (1u << static_cast<uint8_t>(Tag::Int))) {
                    Instruction rewritten = code[p.pc];
                    rewritten.set_opcode(Opcode::DivIntFast);
                    auto atomic_code = module.code_atomic_mut();
                    atomic_code[p.pc].store(rewritten.packed(), std::memory_order_release);
                    p.state = SiteState::Quickened;
                    ok = true;
                } else {
                    ok = false;
                }
                break;
            }
            case Opcode::Eq: {
                if (p.type_feedback_bits == (1u << static_cast<uint8_t>(Tag::Int))) {
                    Instruction rewritten = code[p.pc];
                    rewritten.set_opcode(Opcode::EqIntFast);
                    auto atomic_code = module.code_atomic_mut();
                    atomic_code[p.pc].store(rewritten.packed(), std::memory_order_release);
                    p.state = SiteState::Quickened;
                    ok = true;
                } else {
                    ok = false;
                }
                break;
            }
            case Opcode::Lt: {
                if (p.type_feedback_bits == (1u << static_cast<uint8_t>(Tag::Int))) {
                    Instruction rewritten = code[p.pc];
                    rewritten.set_opcode(Opcode::LtIntFast);
                    auto atomic_code = module.code_atomic_mut();
                    atomic_code[p.pc].store(rewritten.packed(), std::memory_order_release);
                    p.state = SiteState::Quickened;
                    ok = true;
                } else {
                    ok = false;
                }
                break;
            }
            case Opcode::Branch:
                ok = try_quicken_branch(p, module);
                break;
            case Opcode::Call:
                ok = try_quicken_call(p, module);
                break;
            default:
                // No quickening rule for this opcode.
                ok = false;
                break;
        }
        if (ok) ++quickened_count;
    }
    return quickened_count;
}

bool AdaptiveQuickening::try_quicken_get_prop(SiteProfile& profile,
                                                 BytecodeModule& module) noexcept {
    if (!profile.is_hot()) return false;
    if (profile.state == SiteState::Disabled) return false;
    // B17 fix: only quicken if we have a cached (shape, slot) observation.
    // Without this, GET_PROP_MONO would have no IC data and would always
    // miss on the first execution.
    if (profile.poly_entries.empty()) return false;

    auto code = module.code();
    if (profile.pc >= code.size()) return false;
    const Instruction& inst = code[profile.pc];
    if (inst.opcode() != Opcode::GetProp) return false;

    // B9 fix: atomic 32-bit store.
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

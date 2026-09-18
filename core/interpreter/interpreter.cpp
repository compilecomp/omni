// core/interpreter/interpreter.cpp
//
// Interpreter implementation — module loading, dispatch loop, safepoints.

#include "core/interpreter/interpreter.hpp"

#include <memory>
#include <new>

#include "core/bytecode/bytecode_verifier.hpp"
#include "core/common/types.hpp"
#include "core/interpreter/handlers_semantic.hpp"

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

    // Load arguments into registers.
    for (uint16_t i = 0; i < fdesc.param_count; ++i) {
        frame.store_reg(common::RegId{static_cast<uint8_t>(i)}, args[i]);
    }

    // Enter the dispatch loop.
    const auto code = module.code();
    // Per-frame instruction-count budget. Prevents infinite loops from
    // hanging the runtime (Rule 24 fix: termination guard). Generous
    // default; the runtime can lower this via configuration.
    constexpr uint64_t INSTRUCTION_BUDGET = 1'000'000'000ull;
    uint64_t instructions_executed = 0;

    while (frame.pc() < code.size()) {
        // B23 fix: check for pending exception before dispatching.
        if (!frame.exception().is_null()) [[unlikely]] {
            // Find a handler covering the current pc.
            const auto exc_type = frame.exception().is_object_ref()
                ? common::NULL_SYMBOL  // would be exception class's symbol
                : common::NULL_SYMBOL;  // catch-all for non-object exceptions
            const auto* h = module.find_handler(frame.pc(), exc_type);
            if (h != nullptr) {
                frame.clear_exception();
                frame.set_pc(h->handler_pc);
                continue;
            }
            // No handler: propagate exception to caller.
            return std::unexpected(make_error(ErrorCategory::Bytecode,
                                              ERR_UNHANDLED_EXCEPTION));
        }

        // B24 fix: termination guard against infinite loops.
        if (++instructions_executed > INSTRUCTION_BUDGET) [[unlikely]] {
            return std::unexpected(make_error(ErrorCategory::Stack,
                                              ERR_TIMEOUT));
        }

        const auto inst = code[frame.pc()];
        const auto op = inst.opcode();

        Handler h = nullptr;
        if (bytecode::is_semantic(op)) [[likely]] {
            h = semantic_dispatch_.get(op);
        } else if (bytecode::is_quickened(op)) {
            h = quickened_dispatch_.get(op);
        } else if (bytecode::is_fused(op)) {
            h = fused_dispatch_.get(op);
        }
        if (!h) [[unlikely]] {
            // Fall back to the semantic opcode (Rule 96).
            const auto fb = bytecode::fallback_for(op);
            if (fb == bytecode::Opcode::Invalid) {
                return make_error(ErrorCategory::Bytecode, ERR_BAD_OPCODE);
            }
            h = semantic_dispatch_.get(fb);
            if (!h) [[unlikely]] {
                return make_error(ErrorCategory::Bytecode, ERR_BAD_OPCODE);
            }
        }

        h(frame, *this);

        // Safepoint polling (Rule 88).
        if (poll_safepoint()) [[unlikely]] {
            handle_safepoint(frame);
        }
    }

    // Return value convention: register 0 holds the return value.
    return frame.load_reg(common::RegId{0});
}

void Interpreter::handle_safepoint(InterpFrame& frame) noexcept {
    // Check for pending invalidations (Rule 95, Rule 117).
    if (invalidation_pending_.load(std::memory_order_acquire)) {
        invalidation_pending_.store(false, std::memory_order_release);
        // For each site profile, check if the shape epoch has bumped
        // since the profile was last updated; if so, mark the IC dirty.
        // In a real implementation this would walk the frame's profiles
        // and dispatch to per-kind invalidation handlers.
        (void)frame;
    }
    // GC safepoint: in a full implementation we would check a global
    // GC-state flag and yield if requested.
}

}  // namespace omni::interpreter

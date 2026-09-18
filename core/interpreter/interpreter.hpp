// core/interpreter/interpreter.hpp
//
// Tier 0 register interpreter — top-level class.
//
// Purpose:
//   Implements DESIGN.md §5: a register-based interpreter that is the
//   universal correctness fallback (Laws Rule 96). Every Omni function
//   must be runnable here.
//
//   The Interpreter owns:
//     - The dispatch tables (semantic, quickened, fused).
//     - The set of currently-loaded BytecodeModules.
//     - The thread's current frame stack (a stack of InterpFrame*).
//     - The recursion-limit counter (Rule 90).
//     - The safepoint poll counter (Rule 88).
//
// Invariants:
//   - One Interpreter per OS thread (no shared state across threads
//     except the global SymbolTable and the BytecodeModule registry,
//     both of which are thread-safe).
//   - The dispatch loop never allocates on the hot path (Rule 61).
//   - All GC references in registers are tracked via InterpFrame::gc_map.
//   - Safepoint polls occur every SAFEPONENT_POLL_INTERVAL instructions.
//
// Edge cases:
//   - Recursion limit (DEFAULT_RECURSION_LIMIT): when exceeded, the
//     interpreter raises RecursionError (Laws Rule 90).
//   - Stack overflow is also detected by the OS at the native stack
//     boundary; the runtime installs guard pages and translates the
//     resulting SIGSEGV into a RecursionError.
//   - When the interpreter observes a shape epoch bump (Rule 95), it
//     invalidates dependent ICs at the next safepoint (Rule 88).
//
// Cross-references:
//   - DESIGN.md §5 (Register interpreter)
//   - LAWS.md Rule 86 (GC references tracked)
//   - LAWS.md Rule 88 (safepoint polls)
//   - LAWS.md Rule 90 (recursion limits)
//   - LAWS.md Rule 96 (Tier 0 universal fallback)
//   - LAWS.md Rule 118 (no GIL on hot paths)

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/bytecode/bytecode_module.hpp"
#include "core/common/result.hpp"
#include "core/common/types.hpp"
#include "core/interpreter/dispatch.hpp"
#include "core/interpreter/interp_frame.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::interpreter {

class Interpreter {
public:
    Interpreter();

    /// Load a bytecode module. Returns the module id (assigned by the
    /// runtime). Verifies the module before adding it to the registry
    /// (Rule 105).
    [[nodiscard]] common::Result<uint32_t> load_module(
        std::unique_ptr<bytecode::BytecodeModule> module);

    /// Execute a function in the given module. Returns the return value
    /// or an error.
    [[nodiscard]] common::Result<object_model::TaggedValue> execute(
        uint32_t module_id, uint32_t function_index,
        std::span<const object_model::TaggedValue> args);

    /// Register a handler for an opcode in the semantic dispatch table.
    void register_handler(bytecode::Opcode op, Handler h) noexcept {
        semantic_dispatch_.set(op, h);
    }
    void register_quickened_handler(bytecode::Opcode op, Handler h) noexcept {
        quickened_dispatch_.set(op, h);
    }
    void register_fused_handler(bytecode::Opcode op, Handler h) noexcept {
        fused_dispatch_.set(op, h);
    }

    [[nodiscard]] const DispatchTable& semantic_dispatch() const noexcept {
        return semantic_dispatch_;
    }
    [[nodiscard]] const DispatchTable& quickened_dispatch() const noexcept {
        return quickened_dispatch_;
    }
    [[nodiscard]] const DispatchTable& fused_dispatch() const noexcept {
        return fused_dispatch_;
    }

    /// Current frame depth (for recursion limit checks).
    [[nodiscard]] uint32_t frame_depth() const noexcept { return frame_depth_; }

    /// Bump the safepoint poll counter and return true if a safepoint
    /// poll is due (Rule 88).
    [[nodiscard]] bool poll_safepoint() noexcept {
        if (++safepoint_counter_ < common::SAFEPONENT_POLL_INTERVAL) [[likely]] {
            return false;
        }
        safepoint_counter_ = 0;
        return true;
    }

    /// Called at a safepoint. Checks for pending GC, dependency
    /// invalidations, and task suspension requests.
    void handle_safepoint(InterpFrame& frame) noexcept;

private:
    DispatchTable semantic_dispatch_{};
    DispatchTable quickened_dispatch_{};
    DispatchTable fused_dispatch_{};

    std::vector<std::unique_ptr<bytecode::BytecodeModule>> modules_;
    uint32_t next_module_id_{1};

    uint32_t frame_depth_{0};
    uint32_t safepoint_counter_{0};

    /// Atomic flag: when set, the interpreter checks for pending
    /// invalidations at the next safepoint. Cleared after the check.
    std::atomic<bool> invalidation_pending_{false};
};

}  // namespace omni::interpreter

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

#include <cstring>

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
        dispatch_.set(op, h);
    }
    void register_quickened_handler(bytecode::Opcode op, Handler h) noexcept {
        dispatch_.set(op, h);
    }
    void register_fused_handler(bytecode::Opcode op, Handler h) noexcept {
        dispatch_.set(op, h);
    }

    [[nodiscard]] const DispatchTable& dispatch() const noexcept {
        return dispatch_;
    }
    [[nodiscard]] Handler handler_for(bytecode::Opcode op) const noexcept {
        return dispatch_.get(op);
    }

    /// Current frame depth (for recursion limit checks).
    [[nodiscard]] uint32_t frame_depth() const noexcept { return frame_depth_; }

    /// Thread id for this interpreter. Used by sync_enter/sync_exit to
    /// acquire/release thin locks (B2-5 fix: was hardcoded to 1).
    /// Each Interpreter is per-thread, so this is stable.
    [[nodiscard]] uint32_t thread_id() const noexcept { return thread_id_; }
    void set_thread_id(uint32_t id) noexcept { thread_id_ = id; }

    /// The module currently being executed by this interpreter (B1 fix:
    /// handlers need to read the instruction at frame.pc() from the
    /// module's code stream). Set by execute() before the dispatch loop.
    /// Returns nullptr if no execution is in progress.
    [[nodiscard]] const bytecode::BytecodeModule* current_module() const noexcept {
        return current_module_;
    }

    /// Convenience: read the instruction at the given pc from the current
    /// module. Returns Instruction{} if no module is loaded.
    /// This is INLINE so handlers don't pay a function-call penalty per
    /// instruction (the hot path). The dispatch loop already guarantees
    /// pc is in bounds, so the unchecked version below is preferred.
    [[nodiscard]] bytecode::Instruction current_instruction(
        common::BytecodePC pc) const noexcept {
        if (current_module_ == nullptr) [[unlikely]] return bytecode::Instruction{};
        const auto code = current_module_->code();
        if (pc >= code.size()) [[unlikely]] return bytecode::Instruction{};
        return code[pc];
    }

    /// Unchecked fast path: the dispatch loop guarantees pc is in bounds
    /// and current_module_ is non-null. Use this from handlers.
    [[nodiscard]] bytecode::Instruction current_inst_fast(
        common::BytecodePC pc) const noexcept {
        return current_module_->code()[pc];
    }

    /// Direct pointer to the current module's code data. Avoids
    /// re-constructing a span on every instruction.
    [[nodiscard]] const bytecode::Instruction* code_data() const noexcept {
        return current_module_ ? current_module_->code().data() : nullptr;
    }

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
    // Single unified dispatch table covering all 256 opcode values.
    // Semantic opcodes [0,127], quickened [128,223], fused [224,255]
    // share one table so the dispatch loop does a single array lookup
    // instead of a branch chain + lookup.
    DispatchTable dispatch_{};

    std::vector<std::unique_ptr<bytecode::BytecodeModule>> modules_;
    uint32_t next_module_id_{1};

    uint32_t frame_depth_{0};
    uint32_t safepoint_counter_{0};
    /// Thread id for thin-lock ownership (B2-5 fix). Defaults to 1
    /// for single-threaded use; set by the runtime when spawning tasks.
    uint32_t thread_id_{1};

    /// Pointer to the module currently being executed. Set in execute()
    /// before the dispatch loop begins; cleared on exit. Used by handlers
    /// to read the current instruction (B1 fix).
    const bytecode::BytecodeModule* current_module_{nullptr};

    /// Atomic flag: when set, the interpreter checks for pending
    /// invalidations at the next safepoint. Cleared after the check.
    std::atomic<bool> invalidation_pending_{false};
};

}  // namespace omni::interpreter

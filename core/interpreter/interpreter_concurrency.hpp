// core/interpreter/interpreter_concurrency.hpp
//
// Interpreter concurrency model — no-GIL rules.
//
// Purpose:
//   Implements DESIGN.md §5.8: the interpreter runs without a GIL.
//   Multiple OS threads may execute Omni bytecode in parallel on the
//   same BytecodeModule. Synchronization is fine-grained and occurs at:
//     - IC table updates (lock-free CAS on packed slots)
//     - SiteProfile updates (per-frame; no cross-thread sharing)
//     - Quickening overlay updates (atomic publish at safepoints)
//     - Shape invalidation (epoch-based; observed at next safepoint)
//     - sync blocks (fine-grained thin locks on individual objects)
//
// Invariants:
//   - The Interpreter is per-thread. The BytecodeModule registry is
//     shared but read-only after publication (Rule 110: function pointer
//     swaps safe and reversible).
//   - SiteProfiles are per-frame (per-thread); no cross-thread sharing.
//   - Shape epoch is a global atomic; bumps on transition.
//   - Quickened instruction stream updates use atomic 8-byte writes
//     (24-bit Instruction fits in a 32-bit slot which is atomic on all
//     supported platforms). Readers may see the old or new instruction
//     at any given moment, never a torn read.
//
// Cross-references:
//   - DESIGN.md §5.8 (no-GIL rules)
//   - LAWS.md Rule 109 (shared state race-free, TSAN-clean)
//   - LAWS.md Rule 110 (function pointer swaps safe and reversible)
//   - LAWS.md Rule 118 (no GIL on hot paths)
//   - LAWS.md Rule 119 (tier transitions observable)

#pragma once

#include <atomic>
#include <cstdint>

#include "core/common/types.hpp"

namespace omni::interpreter {

/// Global shape epoch counter. Bumps on every shape transition
/// (AddField, AddTrait, OverrideMethod, etc.). Interpreters and JIT
/// code observe the bump and invalidate dependent ICs at their next
/// safepoint.
class GlobalEpoch {
public:
    [[nodiscard]] static common::Epoch current() noexcept {
        return instance().epoch_.load(std::memory_order_acquire);
    }
    static common::Epoch bump() noexcept {
        // B2-26 fix: set the global invalidation-pending flag so that
        // all interpreters check for stale ICs at their next safepoint.
        instance().invalidation_pending_.store(true, std::memory_order_release);
        return instance().epoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    /// Check if any shape transition has occurred since the last check.
    /// Interpreters poll this at safepoints (Rule 88, Rule 95).
    [[nodiscard]] static bool is_invalidation_pending() noexcept {
        return instance().invalidation_pending_.load(std::memory_order_acquire);
    }
    /// Clear the invalidation flag. Called by an interpreter after it
    /// has walked its profiles and invalidated stale ICs.
    static void clear_invalidation() noexcept {
        instance().invalidation_pending_.store(false, std::memory_order_release);
    }

private:
    GlobalEpoch() = default;
    static GlobalEpoch& instance() {
        static GlobalEpoch e;
        return e;
    }
    std::atomic<common::Epoch> epoch_{common::INITIAL_EPOCH};
    std::atomic<bool> invalidation_pending_{false};
};

/// Safepoint state for one thread. The runtime sets `requested` to
/// ask the thread to enter a safepoint; the thread observes it at the
/// next safepoint poll (every SAFEPONENT_POLL_INTERVAL instructions)
/// and calls into the safepoint handler.
class ThreadSafepointState {
public:
    [[nodiscard]] bool is_requested() const noexcept {
        return requested_.load(std::memory_order_acquire);
    }
    void request() noexcept {
        requested_.store(true, std::memory_order_release);
    }
    void clear() noexcept {
        requested_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> requested_{false};
};

}  // namespace omni::interpreter

// core/interpreter/interp_frame.hpp
//
// Interpreter frame.
//
// Purpose:
//   Implements DESIGN.md §5.1: InterpFrame holds the function, closure_env,
//   pc, register file, IC tables, exception state, sync context, debug
//   scope, and GC map for one Tier 0 invocation.
//
// Invariants:
//   - An InterpFrame is allocated on the native stack (caller's frame)
//     or in a per-task frame arena (for async / generator frames that
//     need to survive across suspensions).
//   - The register file is a fixed-size array of FRAME_REGISTER_COUNT
//     TaggedValues. Stack-allocated for cache locality (Rule 61).
//   - The gc_map is a bitmask recording which registers currently hold
//     object references; updated on every register write (Laws Rule 86).
//   - sync_context tracks which sync blocks the frame currently holds;
//     released on frame exit and on exception (Laws Rule 118).
//   - FrameState is reconstructible on demand (Laws Rule 75) for
//     debugging, REPL recovery, and deopt.
//
// Edge cases:
//   - Exception state is a single TaggedValue (an ObjectRef to the
//     exception object). Null means "no exception".
//   - When a sync block exits via exception, the runtime releases the
//     sync lock before propagating.
//   - Generator/async frames persist across suspensions; their InterpFrame
//     must be heap-allocated and tracked by the GC (Laws Rule 76).
//
// Cross-references:
//   - DESIGN.md §5.1 (InterpFrame fields)
//   - LAWS.md Rule 75 (frames reconstructible on demand)
//   - LAWS.md Rule 86 (GC references tracked)
//   - LAWS.md Rule 89 (JIT frames walkable; Tier 0 included)
//   - LAWS.md Rule 90 (recursion limit checks)

#pragma once

#include <array>
#include <cstdint>

#include "core/common/flags.hpp"
#include "core/common/small_vector.hpp"
#include "core/common/types.hpp"
#include "core/interpreter/site_profile.hpp"
#include "core/object_model/object.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::interpreter {

/// Sync block entry on the sync context stack. Records which lock was
/// acquired so the runtime can release it on frame exit / exception.
struct SyncEntry {
    object_model::Object* lock_obj;
    uint32_t pc_at_entry;
};

class InterpFrame {
public:
    InterpFrame(uint32_t module_id, uint32_t function_index,
                object_model::Object* function_obj,
                object_model::Object* closure_env = nullptr)
        : module_id_(module_id),
          function_index_(function_index),
          function_obj_(function_obj),
          closure_env_(closure_env) {}

    // --- Identity / module linkage ---
    [[nodiscard]] uint32_t module_id() const noexcept { return module_id_; }
    [[nodiscard]] uint32_t function_index() const noexcept { return function_index_; }
    [[nodiscard]] object_model::Object* function_obj() const noexcept { return function_obj_; }
    [[nodiscard]] object_model::Object* closure_env() const noexcept { return closure_env_; }

    // --- Program counter ---
    [[nodiscard]] common::BytecodePC pc() const noexcept { return pc_; }
    void set_pc(common::BytecodePC p) noexcept { pc_ = p; }
    void advance_pc(uint32_t delta = 1) noexcept { pc_ += delta; }

    // --- Register file ---
    [[nodiscard]] object_model::TaggedValue load_reg(common::RegId r) const noexcept {
        [[assume(r != common::INVALID_REG)]];
        return regs_[r];
    }
    void store_reg(common::RegId r, object_model::TaggedValue v) noexcept {
        [[assume(r != common::INVALID_REG)]];
        regs_[r] = v;
        // Update GC map: if the new value is an object reference, set
        // the corresponding bit; otherwise clear it.
        if (v.is_object_ref() || v.is_closure_ref()) {
            gc_map_set(r);
        } else {
            gc_map_clear(r);
        }
    }

    // --- GC map (Rule 86) ---
    /// GC map is a bitmask over the register file. A bit is set when
    /// the corresponding register holds an Object reference (Object*).
    /// GC scans this map at safepoints to find live references.
    [[nodiscard]] uint64_t gc_map_chunk(unsigned i) const noexcept {
        [[assume(i < common::GC_MAP_CHUNKS)]];
        return gc_map_[i];
    }
    /// Returns true if register r currently holds a GC reference.
    [[nodiscard]] bool reg_holds_ref(common::RegId r) const noexcept {
        const unsigned chunk = r / common::GC_MAP_BITS_PER_CHUNK;
        const unsigned bit = r % common::GC_MAP_BITS_PER_CHUNK;
        return (gc_map_[chunk] >> bit) & common::ONE_BIT;
    }

    // --- Exception state ---
    [[nodiscard]] object_model::TaggedValue exception() const noexcept { return exception_; }
    void set_exception(object_model::TaggedValue e) noexcept { exception_ = e; }
    void clear_exception() noexcept { exception_ = object_model::TaggedValue{}; }

    // --- Sync context (Rule 118: fine-grained sync) ---
    void enter_sync(object_model::Object* lock_obj) {
        sync_stack_.push_back(SyncEntry{lock_obj, pc_});
    }
    void exit_sync() {
        if (!sync_stack_.empty()) sync_stack_.pop_back();
    }
    [[nodiscard]] const common::SmallVector<SyncEntry, 2>& sync_stack() const noexcept {
        return sync_stack_;
    }

    // --- Debug scope ---
    /// The debug scope is a SymbolId naming the current source location.
    /// Used by the debugger and Explainable Error Engine (Rule 47).
    [[nodiscard]] common::SymbolId debug_scope() const noexcept { return debug_scope_; }
    void set_debug_scope(common::SymbolId s) noexcept { debug_scope_ = s; }

    // --- Site profiles ---
    /// Lookup the SiteProfile for the current pc, creating one if missing.
    /// Hot path: O(N) linear scan is acceptable because the per-frame
    /// profile count is small (typically < 100). For larger frames we
    /// would switch to a robin_map (Rule 17).
    [[nodiscard]] SiteProfile* find_or_create_profile(common::BytecodePC pc) {
        for (auto& p : profiles_) {
            if (p.pc == pc) return &p;
        }
        profiles_.push_back(SiteProfile{});
        SiteProfile& p = profiles_.back();
        p.pc = pc;
        return &p;
    }

private:
    void gc_map_set(common::RegId r) noexcept {
        const unsigned chunk = r / common::GC_MAP_BITS_PER_CHUNK;
        const unsigned bit = r % common::GC_MAP_BITS_PER_CHUNK;
        gc_map_[chunk] |= (common::ONE_BIT << bit);
    }
    void gc_map_clear(common::RegId r) noexcept {
        const unsigned chunk = r / common::GC_MAP_BITS_PER_CHUNK;
        const unsigned bit = r % common::GC_MAP_BITS_PER_CHUNK;
        gc_map_[chunk] &= ~(common::ONE_BIT << bit);
    }

    uint32_t module_id_;
    uint32_t function_index_;
    object_model::Object* function_obj_;
    object_model::Object* closure_env_;
    common::BytecodePC pc_{0};

    /// Register file. 256 registers * 16 bytes = 4096 bytes per frame.
    /// Allocated inline for cache locality (Rule 61).
    std::array<object_model::TaggedValue, common::FRAME_REGISTER_COUNT> regs_{};

    /// GC reference map. GC_MAP_CHUNKS uint64_t chunks, one bit per register.
    /// Updated on every register store (Rule 86: all GC references tracked).
    uint64_t gc_map_[common::GC_MAP_CHUNKS]{};

    object_model::TaggedValue exception_{};
    common::SmallVector<SyncEntry, 2> sync_stack_{};
    common::SymbolId debug_scope_{common::NULL_SYMBOL};

    /// Site profiles. Lazily populated as the interpreter executes.
    /// For hot functions, this vector grows; for cold ones it stays empty.
    common::SmallVector<SiteProfile, 4> profiles_{};
};

}  // namespace omni::interpreter

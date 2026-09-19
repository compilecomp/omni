// core/object_model/compact_header.hpp
//
// 8-byte compact object header for GC-managed objects.
//
// Purpose:
//   Per the OmniGC spec: the object header should be 8 bytes, not 32.
//   Word 1 (32 bits): Shape ID — index into the global ShapeRegistry.
//   Word 2 (32 bits): Flags — GC mark bit, lock state, array length,
//                     cached hash, behavioral flags.
//
//   This is the header for GC-allocated objects. The legacy 32-byte
//   ObjectHeader (in object_header.hpp) is used by the current
//   interpreter's raw-new objects. Once MakeObject migrates to gc_alloc,
//   all objects will use this compact header.
//
//   Sync state (thin lock) is NOT in the header — it moves to a side
//   table (SyncTable) indexed by HeapRef offset. This keeps the header
//   at 8 bytes. Identity is the HeapRef offset itself (stable across GC
//   relocation because HeapRef is an offset, not a pointer).
//
// Invariants:
//   - CompactHeader is exactly 8 bytes.
//   - shape_id is a ShapeRegistry index (uint32). 0 = no shape (null).
//   - flags packs: GC color (2 bits), generation (2 bits), lock state
//     (2 bits: unlocked/locked/inflated), behavioral flags (remaining).
//
// Cross-references:
//   - OmniGC spec §3 (8-Byte Object Header)
//   - LAWS.md Rule 78 (object identity survives GC moves)
//   - LAWS.md Rule 118 (no GIL; thin locks)

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/common/types.hpp"
#include "core/object_model/capability_bits.hpp"

namespace omni::object_model {

/// Compact 8-byte object header for GC-managed objects.
struct CompactHeader {
    // Word 1: Shape ID (index into ShapeRegistry).
    uint32_t shape_id;

    // Word 2: Flags.
    // Bits 0-1: GC color (0=White, 1=Gray, 2=Black, 3=Remapped)
    // Bits 2-3: Generation (0=nursery, 1=old, 2=ancient)
    // Bits 4-5: Lock state (0=unlocked, 1=locked, 2=inflated)
    // Bit  6:   Frozen
    // Bit  7:   Sealed
    // Bit  8:   Shared
    // Bit  9:   Finalizable
    // Bit 10:   Disposable
    // Bit 11:   Weak (referenced by WeakRef)
    // Bit 12:   Native (wraps native resource)
    // Bit 13:   Virtualized (PEA-allocated)
    // Bits 14-31: Reserved (array length, cached hash, etc.)
    uint32_t flags;

    constexpr CompactHeader() noexcept : shape_id(0), flags(0) {}
    constexpr CompactHeader(uint32_t sid, uint32_t fl) noexcept
        : shape_id(sid), flags(fl) {}

    // GC color accessors.
    [[nodiscard]] constexpr uint32_t gc_color() const noexcept {
        return flags & 0x3;
    }
    constexpr void set_gc_color(uint32_t color) noexcept {
        flags = (flags & ~0x3u) | (color & 0x3);
    }

    // Generation accessors.
    [[nodiscard]] constexpr uint32_t generation() const noexcept {
        return (flags >> 2) & 0x3;
    }
    constexpr void set_generation(uint32_t gen) noexcept {
        flags = (flags & ~(0x3u << 2)) | ((gen & 0x3) << 2);
    }

    // Lock state accessors.
    [[nodiscard]] constexpr uint32_t lock_state() const noexcept {
        return (flags >> 4) & 0x3;
    }
    constexpr void set_lock_state(uint32_t state) noexcept {
        flags = (flags & ~(0x3u << 4)) | ((state & 0x3) << 4);
    }

    // Behavioral flag accessors.
    [[nodiscard]] constexpr bool is_frozen() const noexcept { return (flags >> 6) & 1; }
    [[nodiscard]] constexpr bool is_sealed() const noexcept { return (flags >> 7) & 1; }
    [[nodiscard]] constexpr bool is_shared() const noexcept { return (flags >> 8) & 1; }
    [[nodiscard]] constexpr bool is_finalizable() const noexcept { return (flags >> 9) & 1; }
    [[nodiscard]] constexpr bool is_disposable() const noexcept { return (flags >> 10) & 1; }
    [[nodiscard]] constexpr bool is_weak() const noexcept { return (flags >> 11) & 1; }
    [[nodiscard]] constexpr bool is_native() const noexcept { return (flags >> 12) & 1; }
    [[nodiscard]] constexpr bool is_virtualized() const noexcept { return (flags >> 13) & 1; }

    constexpr void set_frozen() noexcept { flags |= (1u << 6); }
    constexpr void set_sealed() noexcept { flags |= (1u << 7); }
    constexpr void set_shared() noexcept { flags |= (1u << 8); }
};
static_assert(sizeof(CompactHeader) == 8, "CompactHeader must be 8 bytes");

/// Lock states for CompactHeader.
enum class LockState : uint32_t {
    Unlocked = 0,
    Locked   = 1,
    Inflated = 2,  // monitor in SyncTable
};

/// Side table for inflated monitors (when a thin lock is contended).
/// Indexed by HeapRef offset. The side table is a flat array; lookup
/// is O(1). Most objects never need an entry (they're never contended).
class SyncTable {
public:
    static SyncTable& instance() {
        static SyncTable table;
        return table;
    }

    /// Inflate: create or look up a monitor for the given object offset.
    /// Returns a pointer to the monitor (a void* that the runtime casts
    /// to the monitor type).
    [[nodiscard]] void* inflate(uint32_t object_offset) {
        // For now, use a simple vector indexed by offset. A production
        // implementation would use a hash map to save memory.
        if (object_offset >= entries_.size()) {
            entries_.resize(object_offset + 1);
        }
        if (entries_[object_offset] == nullptr) {
            entries_[object_offset] = new uint64_t{0};  // placeholder monitor
        }
        return entries_[object_offset];
    }

    /// Get the monitor for an object, or nullptr if not inflated.
    [[nodiscard]] void* get(uint32_t object_offset) const noexcept {
        if (object_offset >= entries_.size()) return nullptr;
        return entries_[object_offset];
    }

private:
    SyncTable() = default;
    std::vector<void*> entries_;
};

}  // namespace omni::object_model

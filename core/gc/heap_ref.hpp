// core/gc/heap_ref.hpp
//
// 32-bit compressed object references (heap offsets).
//
// Purpose:
//   Implements the 32-bit addressing model from the OmniGC spec: instead
//   of storing raw 64-bit Object* pointers, we store 32-bit unsigned
//   offsets relative to a per-thread Heap Base. With 8-byte alignment,
//   a 32-bit offset addresses up to 32 GB of heap — sufficient for
//   99.9% of dynamic language workloads.
//
//   Resolution: Address = Heap_Base + (Offset * 8). This is a single
//   LEA on x86-64, costing zero extra cycles vs a raw pointer load.
//
//   The 32-bit size halves the memory footprint of object graphs and
//   the CCG (Computation Call Graph), doubling cache density. It also
//   frees register space (the JIT dedicates one register to Heap_Base
//   and uses 32-bit offsets everywhere else).
//
// Invariants:
//   - HeapRef is exactly 4 bytes (uint32_t).
//   - HeapRef(0) is the null reference (no object is ever at offset 0
//     because the heap starts at offset 1 * 8 = 8; offset 0 is reserved).
//   - HeapRef values are stable across GC relocation. When the GC moves
//     an object, the forwarding pointer is installed at the old location;
//     the HeapRef value itself does not change. The load barrier resolves
//     the forwarding chain.
//
// Cross-references:
//   - OmniGC spec (32-bit Compressed References + Side Metadata)
//   - DESIGN.md §3.1 (Object, ObjectHeader — will be updated to 8 bytes)
//   - LAWS.md Rule 78 (object identity survives GC moves)
//   - LAWS.md Rule 86 (GC references tracked)

#pragma once

#include <cstdint>
#include <cstring>

#include "core/common/types.hpp"

namespace omni::gc {

/// 32-bit compressed object reference. An offset into the managed heap,
/// measured in 8-byte units. HeapRef(0) is null.
class HeapRef {
public:
    constexpr HeapRef() noexcept : offset_(0) {}
    constexpr explicit HeapRef(uint32_t offset) noexcept : offset_(offset) {}

    [[nodiscard]] constexpr bool is_null() const noexcept { return offset_ == 0; }
    [[nodiscard]] constexpr uint32_t offset() const noexcept { return offset_; }

    /// Convert to a raw 64-bit address using the given heap base.
    /// Address = heap_base + offset * 8.
    [[nodiscard]] constexpr void* resolve(uintptr_t heap_base) const noexcept {
        return reinterpret_cast<void*>(heap_base + static_cast<uintptr_t>(offset_) * 8);
    }

    /// Create a HeapRef from a raw pointer + heap base. The pointer must
    /// be within the heap and 8-byte aligned.
    static constexpr HeapRef from_ptr(void* ptr, uintptr_t heap_base) noexcept {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        return HeapRef(static_cast<uint32_t>((addr - heap_base) / 8));
    }

    constexpr bool operator==(const HeapRef& other) const noexcept {
        return offset_ == other.offset_;
    }
    constexpr bool operator!=(const HeapRef& other) const noexcept {
        return offset_ != other.offset_;
    }

private:
    uint32_t offset_;
};

static_assert(sizeof(HeapRef) == 4, "HeapRef must be 4 bytes");

/// Null HeapRef constant.
constexpr HeapRef NULL_HEAP_REF{};

/// The Heap class manages a contiguous region of memory and provides
/// bump-pointer allocation. This is the non-moving fast path; the
/// concurrent moving collector (ZGC-style) will be built on top of this
/// in a later phase.
///
/// Per the spec: the heap is 8-byte aligned, supports up to 32 GB
/// (2^32 * 8 bytes), and uses side metadata bitmaps for GC colors.
class Heap {
public:
    /// Allocate a heap of `capacity_bytes` bytes. The capacity must be
    /// a multiple of 8 and must not exceed 32 GB (2^32 * 8).
    explicit Heap(size_t capacity_bytes);
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    /// The heap base address, used for HeapRef resolution.
    [[nodiscard]] uintptr_t base() const noexcept {
        return reinterpret_cast<uintptr_t>(base_);
    }

    /// Bump-pointer allocation. Returns a HeapRef to the allocated
    /// block, or NULL_HEAP_REF if the heap is full.
    /// The allocated block is zero-initialized.
    [[nodiscard]] HeapRef alloc(size_t size_bytes) noexcept;

    /// Resolve a HeapRef to a raw pointer. In the non-moving heap this
    /// is a simple address calculation. In the moving heap (future) this
    /// would check the side metadata for forwarding info.
    [[nodiscard]] void* resolve(HeapRef ref) const noexcept {
        if (ref.is_null()) return nullptr;
        return reinterpret_cast<void*>(base() + static_cast<uintptr_t>(ref.offset()) * 8);
    }

    /// Total capacity in bytes.
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

    /// Bytes currently allocated (excluding the reserved null slot).
    [[nodiscard]] size_t allocated() const noexcept {
        return static_cast<size_t>(bump_ptr_ - base_) - 8;
    }

private:
    uint8_t* base_;
    uint8_t* bump_ptr_;
    uint8_t* end_;
    size_t capacity_;
};

}  // namespace omni::gc

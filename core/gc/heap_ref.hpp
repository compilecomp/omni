// core/gc/heap_ref.hpp
//
// 32-bit compressed object references (heap offsets) + free-list heap.
//
// Purpose:
//   Implements the 32-bit addressing model from the OmniGC spec. HeapRef
//   is a 32-bit offset into the managed heap (in 8-byte units), supporting
//   up to 32 GB. Resolution is a single LEA on x86-64.
//
//   The Heap uses a hybrid allocator: a bump pointer for fresh allocations
//   and a free list for reclaimed blocks. Each allocation has an 8-byte
//   size header stored in the slot immediately before the HeapRef, so the
//   GC sweeper can walk the heap and know each object's size.
//
// Layout:
//   [slot 0: reserved (null sentinel)]
//   [slot 1: size header][slot 2..N: object data]  <- HeapRef = 2
//   [slot N+1: size header][slot N+2..M: object data]  <- HeapRef = N+2
//   ...
//   Free blocks: [slot: size header][slot: free node (next_free offset)]
//
// Invariants:
//   - HeapRef is exactly 4 bytes (uint32_t).
//   - HeapRef(0) is null (slot 0 is reserved).
//   - HeapRef(offset) points to user data; the size header is at slot
//     (offset - 1). The size header stores the total allocation size in
//     slots (1 header + N data slots).
//   - The free list is a singly linked list. Each free block's first data
//     slot holds the offset of the next free block (0 = end of list).
//
// Cross-references:
//   - OmniGC spec (32-bit Compressed References + Side Metadata)
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

    [[nodiscard]] constexpr void* resolve(uintptr_t heap_base) const noexcept {
        return reinterpret_cast<void*>(heap_base + static_cast<uintptr_t>(offset_) * 8);
    }

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

constexpr HeapRef NULL_HEAP_REF{};

/// The Heap manages a contiguous region of memory with a hybrid
/// bump-pointer + free-list allocator. Each allocation has an 8-byte
/// size header so the GC sweeper can walk the heap.
class Heap {
public:
    explicit Heap(size_t capacity_bytes);
    ~Heap();

    Heap(const Heap&) = delete;
    Heap& operator=(const Heap&) = delete;

    [[nodiscard]] uintptr_t base() const noexcept {
        return reinterpret_cast<uintptr_t>(base_);
    }

    /// Allocate `size_bytes` for user data. A size header (8 bytes) is
    /// prepended automatically. Returns a HeapRef to the user data,
    /// or NULL_HEAP_REF if the heap is full.
    /// The user data is zero-initialized.
    [[nodiscard]] HeapRef alloc(size_t size_bytes) noexcept;

    /// Free a previously-allocated block. The block is added to the
    /// free list. Called by the GC sweeper.
    void free(HeapRef ref) noexcept;

    /// Resolve a HeapRef to a raw pointer (user data start).
    [[nodiscard]] void* resolve(HeapRef ref) const noexcept {
        if (ref.is_null()) return nullptr;
        return reinterpret_cast<void*>(base() + static_cast<uintptr_t>(ref.offset()) * 8);
    }

    /// The size of the user data at `ref`, in bytes. Reads the size header.
    /// Returns 0 if ref is null or out of bounds.
    [[nodiscard]] size_t object_size(HeapRef ref) const noexcept;

    /// Total capacity in bytes.
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

    /// Bytes currently allocated (user data only, excluding size headers
    /// and the null slot). Updated by alloc() and free().
    [[nodiscard]] size_t allocated() const noexcept { return allocated_bytes_; }

    /// Walk every live (non-free) object in the heap in allocation order.
    /// The callback receives (ref, user_size_bytes) for each object.
    /// Free blocks are skipped (identified by the FREE flag in the size
    /// header). Used by the GC sweeper.
    template <typename Fn>
    void walk_objects(Fn&& callback) const noexcept {
        uint32_t slot = 1;  // skip null slot at 0
        const uint32_t end_slot = static_cast<uint32_t>((bump_ptr_ - base_) / 8);
        while (slot < end_slot) {
            const uint32_t total_slots = read_size_header(slot);
            if (total_slots == 0) {
                break;  // shouldn't happen
            }
            // High bit of the size header marks the block as free.
            const bool is_free = (total_slots & FREE_FLAG) != 0;
            const uint32_t actual_slots = total_slots & ~FREE_FLAG;
            if (!is_free) {
                const HeapRef ref{slot + 1};
                const size_t user_bytes = static_cast<size_t>(actual_slots - 1) * 8;
                callback(ref, user_bytes);
            }
            slot += actual_slots;
        }
    }

    /// Check if the block at `ref` is free (used by the GC to skip
    /// freed blocks during root scanning and object scanning).
    [[nodiscard]] bool is_free(HeapRef ref) const noexcept {
        if (ref.is_null()) return false;
        const uint32_t header_slot = ref.offset() - 1;
        const uint32_t total = read_size_header(header_slot);
        return (total & FREE_FLAG) != 0;
    }

private:
    // High bit of the size header marks the block as free.
    static constexpr uint32_t FREE_FLAG = 0x80000000u;

    void write_size_header(uint32_t header_slot, uint32_t total_slots) noexcept {
        uint32_t* p = reinterpret_cast<uint32_t*>(base_ + header_slot * 8);
        *p = total_slots;
    }
    [[nodiscard]] uint32_t read_size_header(uint32_t header_slot) const noexcept {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(base_ + header_slot * 8);
        return *p;
    }
    void mark_free(uint32_t header_slot) noexcept {
        uint32_t* p = reinterpret_cast<uint32_t*>(base_ + header_slot * 8);
        *p |= FREE_FLAG;
    }

    // Free list node: stored in the first data slot of a freed block.
    // Contains the offset of the next free block (0 = end of list).
    void write_free_next(uint32_t data_slot, uint32_t next_free) noexcept {
        uint32_t* p = reinterpret_cast<uint32_t*>(base_ + data_slot * 8);
        *p = next_free;
    }
    [[nodiscard]] uint32_t read_free_next(uint32_t data_slot) const noexcept {
        const uint32_t* p = reinterpret_cast<const uint32_t*>(base_ + data_slot * 8);
        return *p;
    }

    uint8_t* base_;
    uint8_t* bump_ptr_;
    uint8_t* end_;
    size_t capacity_;
    size_t allocated_bytes_{0};
    uint32_t free_list_head_{0};  // offset of first free block's header slot
};

}  // namespace omni::gc

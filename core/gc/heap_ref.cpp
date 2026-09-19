// core/gc/heap_ref.cpp
//
// Heap implementation — hybrid bump-pointer + free-list allocator with
// size headers for GC sweeping.

#include "core/gc/heap_ref.hpp"

#include <cstdlib>
#include <new>
#include <sys/mman.h>

namespace omni::gc {

namespace {

constexpr size_t round_up(size_t size, size_t align) noexcept {
    return (size + align - 1) & ~(align - 1);
}

}  // namespace

Heap::Heap(size_t capacity_bytes)
    : capacity_(round_up(capacity_bytes, 8)) {
    void* p = ::mmap(nullptr, capacity_,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        base_ = static_cast<uint8_t*>(std::malloc(capacity_));
        if (base_ == nullptr) {
            throw std::bad_alloc();
        }
    } else {
        base_ = static_cast<uint8_t*>(p);
    }
    // Slot 0 is reserved (null sentinel). Bump pointer starts at slot 1.
    bump_ptr_ = base_ + 8;
    end_ = base_ + capacity_;
}

Heap::~Heap() {
    if ((reinterpret_cast<uintptr_t>(base_) & 0xFFF) == 0) {
        ::munmap(base_, capacity_);
    } else {
        std::free(base_);
    }
}

HeapRef Heap::alloc(size_t size_bytes) noexcept {
    const size_t data_bytes = round_up(size_bytes, 8);
    const uint32_t data_slots = static_cast<uint32_t>(data_bytes / 8);
    const uint32_t total_slots = 1 + data_slots;  // 1 header + data

    // First-fit search on the free list.
    uint32_t prev_free = 0;
    uint32_t cur = free_list_head_;
    while (cur != 0) {
        const uint32_t cur_raw = read_size_header(cur);
        const uint32_t cur_total = cur_raw & SIZE_MASK;
        const uint32_t cur_data = cur_total - 1;
        if (cur_data >= data_slots) {
            // Found a fit. If the block is much larger, split it.
            if (cur_data >= data_slots + 2) {
                // Split: keep the first `total_slots` for this alloc,
                // create a new free block from the remainder.
                const uint32_t remaining = cur_total - total_slots;
                const uint32_t new_free_slot = cur + total_slots;
                write_size_header(new_free_slot, remaining | FREE_FLAG | (cur_raw & RAW_FLAG));
                const uint32_t next = read_free_next(cur + 1);
                write_free_next(new_free_slot + 1, next);
                if (prev_free == 0) {
                    free_list_head_ = new_free_slot;
                } else {
                    write_free_next(prev_free + 1, new_free_slot);
                }
                // Clear the free flag for the reused block (preserve RAW).
                write_size_header(cur, total_slots | (cur_raw & RAW_FLAG));
            } else {
                // Use the whole block (no split). Clear the free flag.
                const uint32_t next = read_free_next(cur + 1);
                if (prev_free == 0) {
                    free_list_head_ = next;
                } else {
                    write_free_next(prev_free + 1, next);
                }
                write_size_header(cur, cur_total | (cur_raw & RAW_FLAG));
            }
            // Zero the user data.
            uint8_t* data = base_ + (cur + 1) * 8;
            std::memset(data, 0, data_bytes);
            allocated_bytes_ += data_bytes;
            return HeapRef{cur + 1};
        }
        prev_free = cur;
        cur = read_free_next(cur + 1);
    }

    // No fit in free list; bump-allocate.
    if (bump_ptr_ + static_cast<size_t>(total_slots) * 8 > end_) [[unlikely]] {
        return NULL_HEAP_REF;
    }
    const uint32_t header_slot = static_cast<uint32_t>((bump_ptr_ - base_) / 8);
    write_size_header(header_slot, total_slots);
    bump_ptr_ += static_cast<size_t>(total_slots) * 8;
    uint8_t* data = base_ + (header_slot + 1) * 8;
    std::memset(data, 0, data_bytes);
    allocated_bytes_ += data_bytes;
    return HeapRef{header_slot + 1};
}

void Heap::free(HeapRef ref) noexcept {
    if (ref.is_null()) return;
    const uint32_t data_slot = ref.offset();
    const uint32_t header_slot = data_slot - 1;
    const uint32_t total_raw = read_size_header(header_slot);
    const uint32_t total_slots = total_raw & SIZE_MASK;
    const uint32_t data_slots = total_slots - 1;
    allocated_bytes_ -= static_cast<size_t>(data_slots) * 8;
    // Mark the block as free (preserve RAW flag) and add to free list.
    write_size_header(header_slot, total_slots | FREE_FLAG | (total_raw & RAW_FLAG));
    write_free_next(data_slot, free_list_head_);
    free_list_head_ = header_slot;
}

size_t Heap::object_size(HeapRef ref) const noexcept {
    if (ref.is_null()) return 0;
    const uint32_t header_slot = ref.offset() - 1;
    if (header_slot >= static_cast<uint32_t>((bump_ptr_ - base_) / 8)) return 0;
    const uint32_t total_slots = read_size_header(header_slot) & SIZE_MASK;
    return static_cast<size_t>(total_slots - 1) * 8;
}

}  // namespace omni::gc

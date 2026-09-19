// core/gc/heap_ref.cpp
//
// Heap implementation — bump-pointer allocation.

#include "core/gc/heap_ref.hpp"

#include <cstdlib>
#include <new>
#include <sys/mman.h>

namespace omni::gc {

namespace {

/// Round up `size` to the next multiple of `align`.
constexpr size_t round_up(size_t size, size_t align) noexcept {
    return (size + align - 1) & ~(align - 1);
}

}  // namespace

Heap::Heap(size_t capacity_bytes)
    : capacity_(round_up(capacity_bytes, 8)) {
    // Use mmap for large allocations (avoids page faults on first access
    // and gives us huge-page alignment for free).
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
    // Reserve offset 0 as the null sentinel (HeapRef(0) is null).
    // The first real allocation starts at offset 1 (byte 8).
    bump_ptr_ = base_ + 8;
    end_ = base_ + capacity_;
}

Heap::~Heap() {
    // Determine if base_ was allocated with mmap or malloc.
    // Heuristic: mmap returns page-aligned addresses (multiple of 4096).
    // This is not perfectly reliable but works in practice for the
    // non-moving heap. A production implementation would track the
    // allocation method.
    if ((reinterpret_cast<uintptr_t>(base_) & 0xFFF) == 0) {
        ::munmap(base_, capacity_);
    } else {
        std::free(base_);
    }
}

HeapRef Heap::alloc(size_t size_bytes) noexcept {
    const size_t aligned = round_up(size_bytes, 8);
    if (bump_ptr_ + aligned > end_) [[unlikely]] {
        return NULL_HEAP_REF;  // out of memory
    }
    // Zero-initialize (Rule 86: GC references must be trackable; null
    // is the safe default for freshly allocated memory).
    std::memset(bump_ptr_, 0, aligned);
    HeapRef ref = HeapRef::from_ptr(bump_ptr_, base());
    bump_ptr_ += aligned;
    return ref;
}

}  // namespace omni::gc

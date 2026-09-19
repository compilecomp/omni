// core/gc/generational.hpp
//
// Generational GC: nursery (copying) + old gen (mark-sweep).
//
// Purpose:
//   Implements a real two-generation garbage collector:
//   - Nursery: bump-allocated, collected by copying (minor GC).
//   - Old gen: mark-sweep with free list (major GC).
//
//   New allocations go to the nursery. When the nursery fills, a minor
//   GC copies live objects to old gen. Forwarding pointers are installed
//   at the old nursery address so references can be updated.
//
//   The write barrier marks cards dirty when an old-gen object stores
//   a reference to a nursery object. During minor GC, dirty cards are
//   scanned to find roots into the nursery.
//
// Invariants:
//   - Nursery objects are always at addresses in [nursery_base, nursery_end).
//   - Old-gen objects are always at addresses in [old_base, old_end).
//   - After a minor GC, all nursery objects are either freed (dead) or
//     copied to old gen (live). The nursery is then empty.
//   - Forwarding pointers: when an object is copied from nursery to old
//     gen, the first word of the old nursery location is overwritten
//     with the new Object* (tagged with a forwarding bit). The side
//     metadata marks the slot as Remapped.
//
// Cross-references:
//   - OmniGC spec (Concurrent Generational GC)
//   - LAWS.md Rule 87 (write barriers must be correct)
//   - LAWS.md Rule 78 (object identity survives GC moves)

#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

#include <sys/mman.h>

#include "core/gc/heap_ref.hpp"
#include "core/gc/remembered_set.hpp"
#include "core/gc/side_metadata.hpp"

namespace omni::gc {

/// Generation identifier.
enum class Generation : uint8_t {
    Nursery = 0,  // young gen (copying collector)
    Old     = 1,  // old gen (mark-sweep)
};

/// A nursery region: bump-allocated, cleared on minor GC.
/// Objects allocated here are copied to old gen on survival.
class Nursery {
public:
    explicit Nursery(size_t capacity_bytes)
        : base_(static_cast<uint8_t*>(
              ::mmap(nullptr, capacity_bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))),
          bump_(base_ + 8),  // skip slot 0 (null sentinel)
          end_(base_ + capacity_bytes),
          capacity_(capacity_bytes) {
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            bump_ = nullptr;
            end_ = nullptr;
        }
    }

    ~Nursery() {
        if (base_ != nullptr && base_ != MAP_FAILED) {
            ::munmap(base_, capacity_);
        }
    }

    Nursery(const Nursery&) = delete;
    Nursery& operator=(const Nursery&) = delete;

    /// Bump-allocate `size` bytes. Returns a raw pointer, or nullptr if full.
    [[nodiscard]] void* alloc(size_t size) noexcept {
        const size_t aligned = (size + 7) & ~size_t(7);
        if (bump_ + aligned > end_) [[unlikely]] return nullptr;
        void* ptr = bump_;
        bump_ += aligned;
        std::memset(ptr, 0, aligned);
        return ptr;
    }

    /// Check if `ptr` is within the nursery address range.
    [[nodiscard]] bool contains(void* ptr) const noexcept {
        const auto addr = reinterpret_cast<uintptr_t>(ptr);
        const auto base = reinterpret_cast<uintptr_t>(base_);
        return addr >= base && addr < reinterpret_cast<uintptr_t>(end_);
    }

    /// Reset the nursery (called after minor GC copies live objects).
    void reset() noexcept {
        bump_ = base_ + 8;  // skip null sentinel
    }

    /// Bytes used (excluding the null sentinel slot).
    [[nodiscard]] size_t used() const noexcept {
        return static_cast<size_t>(bump_ - base_) - 8;
    }

    /// Bytes free.
    [[nodiscard]] size_t free() const noexcept {
        return static_cast<size_t>(end_ - bump_);
    }

    [[nodiscard]] uintptr_t base() const noexcept {
        return reinterpret_cast<uintptr_t>(base_);
    }

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }

private:
    uint8_t* base_;
    uint8_t* bump_;
    uint8_t* end_;
    size_t capacity_;
};

/// Forwarding pointer mechanism for copying GC.
/// When an object is copied from nursery to old gen, the first word of
/// the old nursery location is overwritten with a tagged pointer to the
/// new location. The FORWARD_TAG bit distinguishes forwarded objects
/// from normal objects.
class ForwardingTable {
public:
    /// Install a forwarding pointer: old_ptr → new_ptr.
    /// Overwrites the first word of old_ptr with new_ptr | FORWARD_TAG.
    static void forward(void* old_ptr, void* new_ptr) noexcept {
        uintptr_t* word = reinterpret_cast<uintptr_t*>(old_ptr);
        *word = reinterpret_cast<uintptr_t>(new_ptr) | FORWARD_TAG;
    }

    /// Check if `ptr` has been forwarded.
    [[nodiscard]] static bool is_forwarded(void* ptr) noexcept {
        const uintptr_t* word = reinterpret_cast<const uintptr_t*>(ptr);
        return (*word & FORWARD_TAG) != 0;
    }

    /// Get the forwarding target (the new address). Caller must check
    /// is_forwarded() first.
    [[nodiscard]] static void* follow(void* ptr) noexcept {
        const uintptr_t* word = reinterpret_cast<const uintptr_t*>(ptr);
        return reinterpret_cast<void*>(*word & ~FORWARD_TAG);
    }

private:
    // Use the low bit as the forwarding tag. Object pointers are always
    // 8-byte aligned, so the low 3 bits are free.
    static constexpr uintptr_t FORWARD_TAG = 1;
};

}  // namespace omni::gc

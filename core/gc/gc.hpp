// core/gc/gc.hpp
//
// OmniGC — generational garbage collector.
//
// Architecture:
//   - Nursery (young gen): bump-allocated, collected by copying.
//     New objects go here. Minor GC copies live objects to old gen.
//   - Old gen: mark-sweep with free list. Major GC collects this.
//   - Write barrier: marks cards dirty when old→nursery refs are stored.
//   - Load barrier: follows forwarding pointers after minor GC.
//
// GC cycles:
//   - Minor GC: triggered when nursery is full. Copies live nursery
//     objects to old gen. Scans roots + dirty cards to find live
//     nursery objects. Installs forwarding pointers. Clears nursery.
//   - Major GC: triggered when old gen exceeds threshold. Mark-sweep
//     on old gen. Also does a minor GC first.
//
// Cross-references:
//   - OmniGC spec (Concurrent Generational GC, ZGC-style)
//   - DESIGN.md §19 (GC model)
//   - LAWS.md Rule 86 (GC references tracked)
//   - LAWS.md Rule 87 (read/write barriers)
//   - LAWS.md Rule 88 (safepoint polling)

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/gc/gc_handle.hpp"
#include "core/gc/generational.hpp"
#include "core/gc/heap_ref.hpp"
#include "core/gc/remembered_set.hpp"
#include "core/gc/side_metadata.hpp"

namespace omni::gc {

/// GC configuration.
struct GcConfig {
    double trigger_threshold = 0.8;        // major GC trigger
    double nursery_threshold = 0.9;        // minor GC trigger
    size_t initial_heap_size = 64 * 1024 * 1024;  // old gen
    size_t nursery_size = 4 * 1024 * 1024;         // nursery
};

/// Root scanner callback. The GC provides an evacuate function: given
/// a raw Object* pointer, evacuate returns the pointer to use going
/// forward (either the original, or the forwarded address if the object
/// was copied from nursery to old gen during minor GC). The root scanner
/// should call evacuate on every Object* it finds, and store the result
/// back if it changed.
///
/// For major GC, the GC also provides a mark_ref function: the root
/// scanner calls mark_ref(HeapRef) for old-gen objects so they get
/// marked as live.
using RootScanner = std::function<void(
    std::function<void*(void*)> evacuate,
    std::function<void(HeapRef)> mark_ref
)>;

/// Object scanner callback. Same interface as RootScanner but for
/// scanning an object's fields.
using ObjectScanner = std::function<void(
    void* obj,
    std::function<void*(void*)> evacuate,
    std::function<void(HeapRef)> mark_ref
)>;

class GarbageCollector {
public:
    static GarbageCollector& instance() {
        static GarbageCollector gc;
        return gc;
    }

    void init(const GcConfig& config = {});

    /// Allocate from the nursery. May trigger a minor GC if full.
    [[nodiscard]] void* alloc(size_t size_bytes);

    /// Allocate from old gen directly (for large objects or promoted).
    [[nodiscard]] HeapRef alloc_old(size_t size_bytes);

    /// Check if a pointer is in the nursery.
    [[nodiscard]] bool is_nursery(void* ptr) const noexcept {
        return nursery_ != nullptr && nursery_->contains(ptr);
    }

    /// Check if a pointer is in old gen.
    [[nodiscard]] bool is_old_gen(void* ptr) const noexcept {
        if (old_gen_ == nullptr) return false;
        const auto addr = reinterpret_cast<uintptr_t>(ptr);
        return addr >= old_gen_->base()
            && addr < old_gen_->base() + old_gen_->capacity();
    }

    /// Check if a pointer is GC-managed (nursery or old gen).
    [[nodiscard]] bool heap_contains(void* ptr) const noexcept {
        return is_nursery(ptr) || is_old_gen(ptr);
    }

    /// Convert a pointer to a HeapRef (old gen only).
    [[nodiscard]] HeapRef ptr_to_ref(void* ptr) const noexcept {
        return HeapRef::from_ptr(ptr, old_gen_->base());
    }

    /// Resolve a HeapRef (old gen) to a raw pointer.
    [[nodiscard]] void* resolve(HeapRef ref) const noexcept {
        return old_gen_->resolve(ref);
    }

    /// Write barrier. Called when storing a reference into an object field.
    /// If the parent is old-gen and the child is nursery, marks the card
    /// dirty so the minor GC scans it.
    void write_barrier(void* parent, void* child) noexcept {
        if (parent == nullptr || child == nullptr) return;
        if (!is_old_gen(parent) || !is_nursery(child)) return;
        // Old→nursery reference: mark the card dirty.
        const auto offset = reinterpret_cast<uintptr_t>(parent)
                          - old_gen_->base();
        remembered_set_->mark_dirty(static_cast<uint32_t>(offset));
    }

    /// Load barrier. Called on every object reference load.
    /// If the object has been forwarded (nursery object copied to old
    /// gen during minor GC), follows the forwarding pointer and returns
    /// the new address.
    [[nodiscard]] static void* load_barrier(void* ptr) noexcept {
        if (ptr == nullptr) return nullptr;
        if (ForwardingTable::is_forwarded(ptr)) {
            return ForwardingTable::follow(ptr);
        }
        return ptr;
    }

    void register_root_scanner(RootScanner scanner);
    void clear_root_scanners() { root_scanners_.clear(); }
    void set_object_scanner(ObjectScanner scanner) {
        object_scanner_ = std::move(scanner);
    }

    /// Trigger a minor GC (nursery collection).
    void minor_collect();

    /// Trigger a major GC (full collection: minor + old gen mark-sweep).
    void major_collect();

    /// Generic collect — picks minor or major based on heap state.
    void collect();

    [[nodiscard]] bool is_collecting() const noexcept {
        return collecting_.load(std::memory_order_acquire);
    }

    void request_gc() noexcept {
        gc_requested_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool is_gc_requested() const noexcept {
        return gc_requested_.load(std::memory_order_acquire);
    }
    void clear_gc_request() noexcept {
        gc_requested_.store(false, std::memory_order_release);
    }

    // Statistics.
    [[nodiscard]] size_t old_gen_capacity() const noexcept {
        return old_gen_ ? old_gen_->capacity() : 0;
    }
    [[nodiscard]] size_t old_gen_allocated() const noexcept {
        return old_gen_ ? old_gen_->allocated() : 0;
    }
    [[nodiscard]] size_t nursery_used() const noexcept {
        return nursery_ ? nursery_->used() : 0;
    }
    [[nodiscard]] uint64_t minor_collection_count() const noexcept {
        return minor_count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64_t major_collection_count() const noexcept {
        return major_count_.load(std::memory_order_acquire);
    }

    // Old-gen mark/sweep helpers (used by major GC).
    void mark_old_gen(HeapRef ref);
    void mark_nursery_ptr(void* ptr);

    // Side metadata access (for old gen).
    [[nodiscard]] SideMetadata& metadata() noexcept { return *metadata_; }
    [[nodiscard]] Heap& old_gen() noexcept { return *old_gen_; }

    // Mark an allocation as raw (not an Object — don't scan).
    void mark_raw(HeapRef ref) noexcept { old_gen_->mark_raw(ref); }
    [[nodiscard]] bool is_raw(HeapRef ref) const noexcept {
        return old_gen_->is_raw(ref);
    }

private:
    GarbageCollector() = default;

    void copy_nursery_to_old();
    void mark_phase_old_gen();
    void sweep_phase_old_gen();
    void reset_phase_old_gen();

    Nursery* nursery_{nullptr};
    Heap* old_gen_{nullptr};
    SideMetadata* metadata_{nullptr};
    RememberedSet* remembered_set_{nullptr};
    GcConfig config_{};

    std::vector<RootScanner> root_scanners_;
    ObjectScanner object_scanner_;

    std::atomic<bool> collecting_{false};
    std::atomic<bool> gc_requested_{false};
    std::atomic<uint64_t> minor_count_{0};
    std::atomic<uint64_t> major_count_{0};

    // Mark stack for old-gen mark phase.
    std::vector<HeapRef> mark_stack_;
};

/// Convenience: allocate from the GC heap (nursery).
[[nodiscard]] inline void* gc_alloc(size_t size_bytes) {
    return GarbageCollector::instance().alloc(size_bytes);
}

}  // namespace omni::gc

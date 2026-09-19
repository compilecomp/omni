// core/gc/gc.hpp
//
// OmniGC — mark-sweep garbage collector.
//
// Purpose:
//   Implements the first phase of the OmniGC: a mark-sweep collector
//   that runs concurrently with the interpreter. This is the foundation
//   that the ZGC-style moving collector will be built on top of.
//
//   The collector works in three phases:
//     1. Mark: Walk all GC roots (handle table + interpreter registers +
//        frame stacks). For each reachable object, set its color to Black.
//     2. Sweep: Walk the heap. Any object still White (unmarked) is
//        dead — its memory is reclaimed.
//     3. Reset: Clear all colors back to White for the next cycle.
//
//   The collector is triggered at safepoints when the heap's allocated
//   bytes exceed a threshold. The safepoint protocol ensures no mutator
//   is running during mark/sweep (STW). A future phase will make mark
//   concurrent (mutator runs during marking) and sweep concurrent.
//
// Invariants:
//   - The GC is triggered at safepoints (Rule 88). The interpreter polls
//     for GC requests at backedges and calls.
//   - Root scanning walks: (a) the handle table, (b) each interpreter's
//     register file + frame stack (via a registered root scanner callback).
//   - Object scanning walks: each live object's fields (via a registered
//     object scanner callback that knows the object's shape).
//   - The GC is thread-safe: multiple interpreters can register roots;
//     the GC runs in a single dedicated thread (for now).
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
#include "core/gc/heap_ref.hpp"
#include "core/gc/load_barrier.hpp"
#include "core/gc/side_metadata.hpp"

namespace omni::gc {

/// GC configuration. All thresholds are named constexpr (Rule 23).
struct GcConfig {
    /// Trigger a GC when allocated bytes exceed this fraction of capacity.
    /// Default: 0.8 (80% full).
    double trigger_threshold = 0.8;

    /// After GC, if allocated bytes still exceed this fraction, grow the heap.
    /// Default: 0.6 (60% full after GC).
    double grow_threshold = 0.6;

    /// Heap growth factor when growing.
    /// Default: 2.0 (double the heap).
    double grow_factor = 2.0;

    /// Initial heap size in bytes.
    /// Default: 64 MB.
    size_t initial_heap_size = 64 * 1024 * 1024;
};

/// Root scanner callback. The GC calls this to scan an interpreter's
/// roots. The callback should invoke `mark(ref)` for each live HeapRef
/// found in the interpreter's registers and frame stack.
using RootScanner = std::function<void(std::function<void(HeapRef)>)>;

/// Object scanner callback. The GC calls this to scan an object's fields.
/// The callback should invoke `mark(ref)` for each live HeapRef found
/// in the object's slots/fields.
using ObjectScanner = std::function<void(void* obj, std::function<void(HeapRef)>)>;

/// The garbage collector. One per process.
class GarbageCollector {
public:
    static GarbageCollector& instance() {
        static GarbageCollector gc;
        return gc;
    }

    /// Initialize the GC with the given config. Must be called before
    /// any allocation.
    void init(const GcConfig& config = {});

    /// Allocate `size_bytes` from the managed heap. Returns a HeapRef.
    /// May trigger a GC if the heap is above the trigger threshold.
    [[nodiscard]] HeapRef alloc(size_t size_bytes);

    /// Resolve a HeapRef to a raw pointer. This is the load barrier
    /// fast path: in the non-moving collector, it's a simple address
    /// calculation. In the moving collector (future), it checks the
    /// side metadata for forwarding info.
    [[nodiscard]] void* resolve(HeapRef ref) const noexcept {
        return heap_->resolve(ref);
    }

    /// Check if a raw pointer is within the GC heap's address range.
    /// Used by the root scanner to distinguish GC-managed objects from
    /// raw-new objects (which are not GC-collected).
    [[nodiscard]] bool heap_contains(void* ptr) const noexcept {
        if (heap_ == nullptr) return false;
        const auto addr = reinterpret_cast<uintptr_t>(ptr);
        const auto base = heap_->base();
        return addr >= base && addr < base + heap_->capacity();
    }

    /// Convert a raw pointer to a HeapRef. The pointer must be within
    /// the GC heap (call heap_contains first).
    [[nodiscard]] HeapRef ptr_to_ref(void* ptr) const noexcept {
        return HeapRef::from_ptr(ptr, heap_->base());
    }

    /// Mark an allocation as raw (not an Object — the GC should mark it
    /// but not call the object scanner on it). Used for slot arrays and
    /// other non-Object allocations.
    void mark_raw(HeapRef ref) noexcept {
        heap_->mark_raw(ref);
    }

    /// Check if an allocation is raw.
    [[nodiscard]] bool is_raw(HeapRef ref) const noexcept {
        return heap_->is_raw(ref);
    }

    /// Write barrier. Called on every object reference store into a
    /// GC-managed object's field. Records the parent→child edge in the
    /// remembered set for generational GC (Rule 87).
    /// Currently a no-op; the generational collector will implement it.
    void write_barrier(HeapRef parent, HeapRef child) noexcept {
        gc::write_barrier(parent, child);
    }

    /// Register a root scanner. The GC calls all registered scanners
    /// during the mark phase. Each interpreter should register its
    /// scanner on creation and unregister on destruction.
    void register_root_scanner(RootScanner scanner);
    void clear_root_scanners() { root_scanners_.clear(); }

    /// Set the object scanner. The GC calls this to scan an object's
    /// fields during the mark phase. There is one global object scanner
    /// that uses the object's shape to determine which fields are references.
    void set_object_scanner(ObjectScanner scanner) {
        object_scanner_ = std::move(scanner);
    }

    /// Manually trigger a GC cycle. Normally called automatically by
    /// alloc() when the threshold is exceeded.
    void collect();

    /// Is a GC currently in progress?
    [[nodiscard]] bool is_collecting() const noexcept {
        return collecting_.load(std::memory_order_acquire);
    }

    /// Request a GC at the next safepoint. Called by alloc() when the
    /// threshold is exceeded. The interpreter checks this at safepoints.
    void request_gc() noexcept {
        gc_requested_.store(true, std::memory_order_release);
    }
    [[nodiscard]] bool is_gc_requested() const noexcept {
        return gc_requested_.load(std::memory_order_acquire);
    }
    void clear_gc_request() noexcept {
        gc_requested_.store(false, std::memory_order_release);
    }

    /// Heap statistics.
    [[nodiscard]] size_t heap_capacity() const noexcept {
        return heap_ ? heap_->capacity() : 0;
    }
    [[nodiscard]] size_t heap_allocated() const noexcept {
        return heap_ ? heap_->allocated() : 0;
    }
    [[nodiscard]] uint64_t collection_count() const noexcept {
        return collection_count_.load(std::memory_order_acquire);
    }

private:
    GarbageCollector() = default;

    void mark_phase();
    void sweep_phase();
    void reset_phase();

    void mark(HeapRef ref);
    void scan_object(void* obj_ptr);

    Heap* heap_{nullptr};
    SideMetadata* metadata_{nullptr};
    GcConfig config_{};

    std::vector<RootScanner> root_scanners_;
    ObjectScanner object_scanner_;

    std::atomic<bool> collecting_{false};
    std::atomic<bool> gc_requested_{false};
    std::atomic<uint64_t> collection_count_{0};

    // Mark stack (gray objects queued for scanning).
    std::vector<HeapRef> mark_stack_;
};

/// Convenience: allocate from the GC heap. Equivalent to
/// GarbageCollector::instance().alloc(size).
[[nodiscard]] inline HeapRef gc_alloc(size_t size_bytes) {
    return GarbageCollector::instance().alloc(size_bytes);
}

/// Convenience: resolve a HeapRef through the load barrier.
[[nodiscard]] inline void* gc_resolve(HeapRef ref) noexcept {
    return GarbageCollector::instance().resolve(ref);
}

}  // namespace omni::gc

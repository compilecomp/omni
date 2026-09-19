// core/gc/gc.cpp
//
// Mark-sweep garbage collector implementation.

#include "core/gc/gc.hpp"

#include "core/gc/gc_handle.hpp"

#include <algorithm>
#include <cstring>

namespace omni::gc {

void GarbageCollector::init(const GcConfig& config) {
    config_ = config;
    if (heap_ == nullptr) {
        heap_ = new Heap(config_.initial_heap_size);
        metadata_ = new SideMetadata(heap_->capacity());
    }
}

HeapRef GarbageCollector::alloc(size_t size_bytes) {
    if (heap_ == nullptr) [[unlikely]] {
        init();
    }

    // Check if we should trigger a GC.
    const size_t allocated = heap_->allocated();
    const size_t capacity = heap_->capacity();
    if (static_cast<double>(allocated) / static_cast<double>(capacity)
        > config_.trigger_threshold) [[unlikely]] {
        request_gc();
    }

    // Allocate. If OOM, trigger a GC and retry.
    HeapRef ref = heap_->alloc(size_bytes);
    if (ref.is_null()) [[unlikely]] {
        collect();
        ref = heap_->alloc(size_bytes);
        if (ref.is_null()) {
            // Still OOM after GC. In a production implementation we would
            // grow the heap here. For now, return null.
            return NULL_HEAP_REF;
        }
    }

    // Initialize color to White (already zeroed by Heap::alloc, and
    // White = 0, so no explicit set needed).
    return ref;
}

void GarbageCollector::collect() {
    if (collecting_.exchange(true, std::memory_order_acq_rel)) {
        // Another thread is already collecting.
        return;
    }

    collection_count_.fetch_add(1, std::memory_order_relaxed);

    mark_phase();
    sweep_phase();
    reset_phase();

    collecting_.store(false, std::memory_order_release);
}

void GarbageCollector::mark_phase() {
    // Phase 1: Mark all roots.
    // Walk the handle table.
    HandleTable::instance().scan_roots([this](HeapRef ref) {
        mark(ref);
    });

    // Walk registered root scanners (interpreter registers + frames).
    for (auto& scanner : root_scanners_) {
        scanner([this](HeapRef ref) {
            mark(ref);
        });
    }

    // Phase 2: Process the mark stack (gray objects).
    while (!mark_stack_.empty()) {
        HeapRef ref = mark_stack_.back();
        mark_stack_.pop_back();
        void* obj_ptr = heap_->resolve(ref);
        scan_object(obj_ptr);
    }
}

void GarbageCollector::mark(HeapRef ref) {
    if (ref.is_null()) return;
    // Only mark White objects (avoid revisiting).
    GcColor color = metadata_->get_color(ref);
    if (color != GcColor::White) return;

    // Mark as Gray (queued for scanning).
    metadata_->set_color(ref, GcColor::Gray);
    mark_stack_.push_back(ref);
}

void GarbageCollector::scan_object(void* obj_ptr) {
    if (obj_ptr == nullptr) return;
    if (!object_scanner_) return;

    // Convert back to HeapRef for the color update.
    HeapRef ref = HeapRef::from_ptr(obj_ptr, heap_->base());

    // Mark this object as Black (fully scanned).
    metadata_->set_color(ref, GcColor::Black);

    // Scan the object's fields via the registered scanner.
    object_scanner_(obj_ptr, [this](HeapRef child) {
        mark(child);
    });
}

void GarbageCollector::sweep_phase() {
    // Walk every object in the heap. Free objects that are still White
    // (unmarked = dead). Live objects (Black) are kept; their color is
    // reset to White in reset_phase().
    heap_->walk_objects([this](HeapRef ref, size_t /*user_size*/) {
        GcColor color = metadata_->get_color(ref);
        if (color == GcColor::White) {
            // Dead object — reclaim it.
            heap_->free(ref);
        }
    });
}

void GarbageCollector::reset_phase() {
    // Reset all Black objects to White for the next GC cycle.
    // Walk the entire heap.
    const size_t capacity = heap_->capacity();
    const size_t slot_count = capacity / 8;
    for (uint32_t i = 1; i < slot_count; ++i) {  // skip slot 0 (null)
        HeapRef ref{i};
        GcColor color = metadata_->get_color(ref);
        if (color == GcColor::Black || color == GcColor::Gray) {
            metadata_->set_color(ref, GcColor::White);
        }
    }
    mark_stack_.clear();
}

void GarbageCollector::register_root_scanner(RootScanner scanner) {
    root_scanners_.push_back(std::move(scanner));
}

}  // namespace omni::gc

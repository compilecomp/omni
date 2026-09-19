// core/gc/gc.cpp
//
// Generational GC implementation.
//
// Minor GC (nursery collection):
//   1. Scan all roots (registers + frame stack via registered scanners).
//      For each Object* that points into the nursery, copy the object
//      to old gen, install a forwarding pointer, and UPDATE the pointer
//      in-place to point to the new old-gen address.
//   2. Scan dirty cards (old-gen objects that reference nursery objects).
//      Copy + update those references too.
//   3. Scan copied objects' fields for further nursery references (transitive
//      closure). Copy + update those too.
//   4. Reset the nursery (all live objects have been copied out).
//
// Major GC (old-gen mark-sweep):
//   1. Run a minor GC first (promotes all live nursery objects).
//   2. Mark phase: walk all roots, mark reachable old-gen objects Black.
//   3. Sweep phase: free White (dead) old-gen objects.
//   4. Reset phase: clear colors back to White.

#include "core/gc/gc.hpp"

#include "core/gc/gc_handle.hpp"

#include <algorithm>
#include <cstring>

namespace omni::gc {

void GarbageCollector::init(const GcConfig& config) {
    config_ = config;
    if (old_gen_ == nullptr) {
        old_gen_ = new Heap(config_.initial_heap_size);
        metadata_ = new SideMetadata(old_gen_->capacity());
        remembered_set_ = new RememberedSet(old_gen_->capacity());
        nursery_ = new Nursery(config_.nursery_size);
    }
}

void* GarbageCollector::alloc(size_t size_bytes) {
    if (nursery_ == nullptr) [[unlikely]] init();

    // Check nursery threshold: if the nursery is above the threshold,
    // trigger a minor GC before allocating.
    if (nursery_->used() > nursery_->capacity() * config_.nursery_threshold) {
        minor_collect();
    }

    // Try nursery first.
    void* ptr = nursery_->alloc(size_bytes);
    if (ptr != nullptr) return ptr;

    // Nursery full — trigger minor GC, then retry.
    minor_collect();
    ptr = nursery_->alloc(size_bytes);
    if (ptr != nullptr) return ptr;

    // Still full after GC — allocate directly from old gen.
    HeapRef ref = alloc_old(size_bytes);
    return ref.is_null() ? nullptr : old_gen_->resolve(ref);
}

HeapRef GarbageCollector::alloc_old(size_t size_bytes) {
    if (old_gen_ == nullptr) [[unlikely]] init();

    const size_t allocated = old_gen_->allocated();
    const size_t capacity = old_gen_->capacity();
    if (static_cast<double>(allocated) / static_cast<double>(capacity)
        > config_.trigger_threshold) [[unlikely]] {
        request_gc();
    }

    HeapRef ref = old_gen_->alloc(size_bytes);
    if (ref.is_null()) [[unlikely]] {
        major_collect();
        ref = old_gen_->alloc(size_bytes);
    }
    return ref;
}

void GarbageCollector::minor_collect() {
    if (collecting_.exchange(true, std::memory_order_acq_rel)) return;
    minor_count_.fetch_add(1, std::memory_order_relaxed);

    copy_nursery_to_old();

    // Reset nursery — all live objects have been copied to old gen.
    nursery_->reset();

    // Clear dirty cards (they've been scanned).
    remembered_set_->clear_all();

    collecting_.store(false, std::memory_order_release);
}

void GarbageCollector::copy_nursery_to_old() {
    // Cheney-style copying: we maintain a worklist of newly-copied
    // objects that need their fields scanned. The worklist is implemented
    // as a simple vector of Object* pointers to newly-copied old-gen objects.
    std::vector<void*> worklist;

    // Evacuate function: given a nursery Object*, copy it to old gen
    // (if not already forwarded), install forwarding pointer, and return
    // the new old-gen address. Adds the new object to the worklist.
    auto evacuate = [this, &worklist](void* ptr) -> void* {
        if (ptr == nullptr) return nullptr;
        if (!is_nursery(ptr)) return ptr;

        // Already forwarded?
        if (ForwardingTable::is_forwarded(ptr)) {
            return ForwardingTable::follow(ptr);
        }

        // Copy to old gen.
        constexpr size_t OBJECT_SIZE = 256;
        HeapRef new_ref = old_gen_->alloc(OBJECT_SIZE);
        if (new_ref.is_null()) return ptr;  // OOM — leave in nursery
        void* new_ptr = old_gen_->resolve(new_ref);
        std::memcpy(new_ptr, ptr, OBJECT_SIZE);
        ForwardingTable::forward(ptr, new_ptr);

        // Add to worklist for field scanning.
        worklist.push_back(new_ptr);
        return new_ptr;
    };

    auto mark = [this](HeapRef ref) { mark_old_gen(ref); };

    // Phase 1: Scan roots — evacuate nursery objects, update pointers.
    for (auto& scanner : root_scanners_) {
        scanner(evacuate, mark);
    }

    // Phase 2: Scan dirty cards — evacuate old→nursery refs.
    remembered_set_->scan_dirty([this, &evacuate, &mark](uint32_t card_base) {
        old_gen_->walk_objects([&](HeapRef ref, size_t /*user_bytes*/, bool is_raw) {
            const auto obj_offset = ref.offset() * 8;
            if (obj_offset < card_base || obj_offset >= card_base + CARD_SIZE) return;
            if (is_raw) return;
            void* obj_ptr = old_gen_->resolve(ref);
            if (object_scanner_) {
                object_scanner_(obj_ptr, evacuate, mark);
            }
        });
    });

    // Phase 3: Process the worklist — scan newly-copied objects' fields
    // for further nursery refs (transitive closure). This is O(copied)
    // not O(total old-gen), avoiding the O(n²) blowup.
    while (!worklist.empty()) {
        void* obj_ptr = worklist.back();
        worklist.pop_back();
        if (object_scanner_) {
            object_scanner_(obj_ptr, evacuate, mark);
        }
    }
}

void GarbageCollector::major_collect() {
    if (collecting_.exchange(true, std::memory_order_acq_rel)) return;

    // Minor GC first (promotes all live nursery objects to old gen).
    minor_collect();
    // minor_collect sets collecting_ to false, so re-set it.
    collecting_.store(true, std::memory_order_release);

    major_count_.fetch_add(1, std::memory_order_relaxed);

    mark_phase_old_gen();
    sweep_phase_old_gen();
    reset_phase_old_gen();

    collecting_.store(false, std::memory_order_release);
}

void GarbageCollector::collect() {
    // If nursery is mostly full, do a minor GC.
    // Otherwise, if old gen is mostly full, do a major GC.
    if (nursery_ && nursery_->used() > nursery_->capacity() * config_.nursery_threshold) {
        minor_collect();
    } else if (old_gen_->allocated() > old_gen_->capacity() * config_.trigger_threshold) {
        major_collect();
    } else {
        // Default to minor (cheaper).
        minor_collect();
    }
}

void GarbageCollector::mark_phase_old_gen() {
    // Mark all roots.
    HandleTable::instance().scan_roots([this](HeapRef ref) {
        mark_old_gen(ref);
    });

    // For major GC, evacuate is a no-op (all objects already in old gen
    // after the minor GC that runs first). mark_ref marks old-gen HeapRefs.
    auto no_evacuate = [](void* ptr) -> void* { return ptr; };
    auto mark = [this](HeapRef ref) { mark_old_gen(ref); };

    for (auto& scanner : root_scanners_) {
        scanner(no_evacuate, mark);
    }

    // Process mark stack.
    while (!mark_stack_.empty()) {
        HeapRef ref = mark_stack_.back();
        mark_stack_.pop_back();
        void* obj_ptr = old_gen_->resolve(ref);
        if (obj_ptr == nullptr) continue;

        // Mark this object Black.
        metadata_->set_color(ref, GcColor::Black);

        // Scan fields (evacuate is no-op during major GC).
        if (object_scanner_ && !old_gen_->is_raw(ref)) {
            object_scanner_(obj_ptr, no_evacuate, mark);
        }
    }
}

void GarbageCollector::mark_old_gen(HeapRef ref) {
    if (ref.is_null()) return;
    if (metadata_->get_color(ref) != GcColor::White) return;
    metadata_->set_color(ref, GcColor::Gray);
    mark_stack_.push_back(ref);
}

void GarbageCollector::mark_nursery_ptr(void* ptr) {
    // Nursery objects don't need marking — they're all copied during
    // minor GC. This is a no-op for major GC (which runs after minor).
    (void)ptr;
}

void GarbageCollector::sweep_phase_old_gen() {
    old_gen_->walk_objects([this](HeapRef ref, size_t, bool) {
        if (metadata_->get_color(ref) == GcColor::White) {
            old_gen_->free(ref);
        }
    });
}

void GarbageCollector::reset_phase_old_gen() {
    old_gen_->walk_objects([this](HeapRef ref, size_t, bool) {
        GcColor c = metadata_->get_color(ref);
        if (c == GcColor::Black || c == GcColor::Gray) {
            metadata_->set_color(ref, GcColor::White);
        }
    });
    mark_stack_.clear();
}

void GarbageCollector::register_root_scanner(RootScanner scanner) {
    root_scanners_.push_back(std::move(scanner));
}

}  // namespace omni::gc

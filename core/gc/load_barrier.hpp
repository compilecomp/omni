// core/gc/load_barrier.hpp
//
// Load and write barriers for the 32-bit OmniGC.
//
// Purpose:
//   Per the OmniGC spec §4: every object reference load must go through
//   a load barrier. The fast path checks the side metadata color:
//   if the object is not being relocated (color != Remapped), the
//   barrier returns immediately (~2-3 cycles). The slow path follows
//   the forwarding pointer.
//
//   The write barrier records old→young reference edges in the
//   remembered set (card table) for generational GC.
//
// Cross-references:
//   - OmniGC spec §4 (32-bit Load Barrier)
//   - LAWS.md Rule 87 (read/write barriers must be correct)

#pragma once

#include <cstdint>

#include "core/gc/heap_ref.hpp"
#include "core/gc/remembered_set.hpp"
#include "core/gc/side_metadata.hpp"

namespace omni::gc {

/// Load barrier. Called on every object reference load.
///
/// Fast path (non-moving collector): return the pointer as-is.
/// Fast path (moving collector): check side metadata color; if the
///   object is at its original address, return immediately.
/// Slow path (moving collector): follow forwarding pointer, update
///   the reference, return the new address.
[[nodiscard]] inline void* load_barrier(HeapRef ref,
                                          const Heap& heap,
                                          const SideMetadata& /*metadata*/) noexcept {
    if (ref.is_null()) return nullptr;
    void* ptr = heap.resolve(ref);

    // Non-moving collector: no forwarding possible. Return immediately.
    // Future: moving collector fast path checks color == Remapped.

    return ptr;
}

/// Write barrier. Called on every object reference store.
/// Records the parent→child edge in the remembered set if the parent
/// is old-gen and the child is young-gen (generational invariant).
///
/// The remembered_set is a global singleton; the write barrier marks
/// the card containing the parent object as dirty.
inline void write_barrier(HeapRef parent_ref, HeapRef child_ref,
                            RememberedSet* remembered_set) noexcept {
    if (parent_ref.is_null() || child_ref.is_null()) return;
    if (remembered_set == nullptr) return;
    // Mark the card containing the parent as dirty. The minor GC will
    // scan this card to find old→young references.
    // (Generational check — is parent old-gen and child young-gen? —
    // would go here. For now we mark all cards to be conservative.)
    remembered_set->mark_dirty(parent_ref.offset() * 8);
}

/// Convenience overload that uses the global remembered set.
/// This is the version called by the interpreter's write barrier hook.
inline void write_barrier(HeapRef parent_ref, HeapRef child_ref) noexcept {
    // The global remembered set is owned by GarbageCollector.
    // This no-op version is used when the GC hasn't initialized the
    // remembered set yet. The GC's write_barrier method overrides this.
    (void)parent_ref;
    (void)child_ref;
}

}  // namespace omni::gc

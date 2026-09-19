// core/gc/load_barrier.hpp
//
// Load barrier for the 32-bit OmniGC.
//
// Purpose:
//   Per the OmniGC spec §4: every object reference load must go through
//   a load barrier. The fast path checks the side metadata color:
//   if the object is not being relocated (color != Remapped, or color
//   matches the current GC phase), the barrier returns immediately
//   (~2-3 cycles). The slow path follows the forwarding pointer.
//
//   In the current non-moving collector, the barrier is essentially a
//   no-op — all objects are at their original addresses. The barrier
//   infrastructure is in place so the moving collector can be added
//   without changing call sites.
//
// Invariants:
//   - The barrier is called on every object reference load in JIT code
//     and in the interpreter's Object* → HeapRef conversion.
//   - The fast path is branchless on x86-64 (the color check is a
//     single compare + conditional move).
//   - The slow path is a function call to the runtime stub.
//
// Cross-references:
//   - OmniGC spec §4 (32-bit Load Barrier)
//   - LAWS.md Rule 87 (read barriers must be correct)

#pragma once

#include <cstdint>

#include "core/gc/heap_ref.hpp"
#include "core/gc/side_metadata.hpp"

namespace omni::gc {

/// Load barrier. Called on every object reference load.
///
/// Fast path (non-moving collector): return the pointer as-is.
/// Fast path (moving collector): check side metadata color; if the
///   object is at its original address, return immediately.
/// Slow path (moving collector): follow forwarding pointer, update
///   the reference, return the new address.
///
/// The current implementation is the non-moving fast path. The moving
/// collector will add the color check + forwarding logic.
[[nodiscard]] inline void* load_barrier(HeapRef ref,
                                          const Heap& heap,
                                          const SideMetadata& /*metadata*/) noexcept {
    if (ref.is_null()) return nullptr;
    void* ptr = heap.resolve(ref);

    // Non-moving collector: no forwarding possible. Return immediately.
    // This is the fast path — ~1 cycle (a pointer arithmetic + null check).

    // Future: moving collector fast path.
    // GcColor color = metadata.get_color(ref);
    // if (color != GcColor::Remapped) return ptr;  // not relocated
    // // Slow path: follow forwarding pointer.
    // ptr = follow_forwarding(ref, heap, metadata);

    return ptr;
}

/// Write barrier. Called on every object reference store.
///
/// In the generational collector, this records the write in the
/// remembered set so the GC can find cross-generational references
/// during minor collections.
///
/// The current implementation is a no-op (no generational GC yet).
inline void write_barrier(HeapRef /*parent_ref*/, HeapRef /*child_ref*/) noexcept {
    // Future: generational write barrier.
    // if (parent is old-gen && child is young-gen) {
    //     remembered_set.add(parent_ref);
    // }
}

}  // namespace omni::gc

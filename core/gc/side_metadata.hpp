// core/gc/side_metadata.hpp
//
// Side metadata for the 32-bit OmniGC.
//
// Purpose:
//   In 64-bit ZGC, GC "colors" (Marked0, Marked1, Remapped) are stored
//   in the unused high bits of the pointer. With 32-bit compressed
//   references, all bits are needed for the offset. The solution: store
//   GC colors in a parallel side metadata bitmap.
//
//   Density: 2 bits per 8 bytes of heap (one 2-bit color field per
//   aligned 8-byte slot).
//   Lookup: bitmap_index = heap_offset >> 3. Load the byte, extract the
//   2-bit field.
//   Concurrency: atomic bitwise operations. Mutator reads are lock-free.
//
// Color encoding (2 bits):
//   00 = White (unmarked, candidate for collection)
//   01 = Gray  (queued for scanning)
//   10 = Black (marked, live)
//   11 = Remapped (relocated to new address — forwarding info follows)
//
// Cross-references:
//   - OmniGC spec §2 (Side Metadata)
//   - OmniGC spec §4 (32-bit Load Barrier)
//   - LAWS.md Rule 87 (read/write barriers must be correct)

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/gc/heap_ref.hpp"

namespace omni::gc {

/// GC color states (2 bits per slot).
enum class GcColor : uint8_t {
    White    = 0,  // unmarked
    Gray     = 1,  // queued for scanning
    Black    = 2,  // marked live
    Remapped = 3,  // relocated (forwarding info in object header)
};

/// Side metadata bitmap. 2 bits per 8-byte heap slot.
/// Stored as a flat array of uint64_t (4 slots per uint64).
class SideMetadata {
public:
    /// Construct a side metadata table for a heap of `capacity_bytes`.
    explicit SideMetadata(size_t capacity_bytes)
        : words_((capacity_bytes / 8 + slots_per_word_ - 1) / slots_per_word_) {}

    /// Get the color of the object at the given heap offset.
    [[nodiscard]] GcColor get_color(HeapRef ref) const noexcept {
        const uint32_t slot = ref.offset();
        const uint32_t word_idx = slot / slots_per_word_;
        const uint32_t bit_offset = (slot % slots_per_word_) * bits_per_slot_;
        const uint64_t word = words_[word_idx].load(std::memory_order_acquire);
        return static_cast<GcColor>((word >> bit_offset) & slot_mask_);
    }

    /// Set the color of the object at the given heap offset.
    /// Uses atomic OR for single-bit transitions (White->Gray, Gray->Black).
    /// For multi-bit transitions, uses CAS.
    void set_color(HeapRef ref, GcColor color) noexcept {
        const uint32_t slot = ref.offset();
        const uint32_t word_idx = slot / slots_per_word_;
        const uint32_t bit_offset = (slot % slots_per_word_) * bits_per_slot_;
        const uint64_t color_bits = static_cast<uint64_t>(color) << bit_offset;
        const uint64_t clear_mask = ~(slot_mask_ << bit_offset);
        // CAS loop: clear old color, set new.
        uint64_t old = words_[word_idx].load(std::memory_order_relaxed);
        uint64_t new_val;
        do {
            new_val = (old & clear_mask) | color_bits;
        } while (!words_[word_idx].compare_exchange_weak(
            old, new_val, std::memory_order_release, std::memory_order_relaxed));
    }

    /// Check if the object at `ref` is live (Black or Remapped).
    [[nodiscard]] bool is_live(HeapRef ref) const noexcept {
        const GcColor c = get_color(ref);
        return c == GcColor::Black || c == GcColor::Remapped;
    }

    /// Check if the object at `ref` needs relocation (Remapped color
    /// means the forwarding pointer is installed).
    [[nodiscard]] bool needs_relocation(HeapRef ref) const noexcept {
        return get_color(ref) == GcColor::Remapped;
    }

private:
    static constexpr uint32_t bits_per_slot_ = 2;
    static constexpr uint32_t slots_per_word_ = 64 / bits_per_slot_;  // 32
    static constexpr uint64_t slot_mask_ = (1u << bits_per_slot_) - 1;

    std::vector<std::atomic<uint64_t>> words_;
};

}  // namespace omni::gc

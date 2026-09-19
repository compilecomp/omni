// core/gc/remembered_set.hpp
//
// Remembered set for generational GC.
//
// Purpose:
//   In a generational collector, minor GCs collect only the young
//   generation. To find live young objects, the GC must know which old
//   objects reference young objects (these are "card marks" or
//   "remembered set entries"). The write barrier records these edges.
//
//   This implementation uses a card table: the heap is divided into
//   fixed-size cards (e.g., 512 bytes). Each card has a 1-bit entry in
//   the card table. When a write barrier fires, the card containing the
//   write location is marked dirty. During minor GC, the GC scans only
//   dirty cards in the old generation.
//
// Invariants:
//   - Card size is a power of 2 (512 bytes = 64 slots).
//   - The card table is 1 bit per card. Dirty = 1.
//   - The card table is lock-free for marking (atomic OR).
//   - After a minor GC, all dirty cards in the young generation are
//     cleared (they've been scanned).
//
// Cross-references:
//   - OmniGC spec (Generational Awareness)
//   - LAWS.md Rule 87 (write barriers must be correct)

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/gc/heap_ref.hpp"

namespace omni::gc {

/// Card size in bytes. Must be a power of 2.
/// 512 bytes = 64 slots = 1 cache line on most architectures.
constexpr uint32_t CARD_SIZE = 512;
constexpr uint32_t CARD_SHIFT = 9;  // log2(512)
static_assert((1u << CARD_SHIFT) == CARD_SIZE);

/// Card table for generational GC. 1 bit per card.
/// The card table is indexed by heap offset >> CARD_SHIFT.
class RememberedSet {
public:
    explicit RememberedSet(size_t heap_capacity_bytes)
        : cards_((heap_capacity_bytes / CARD_SIZE + 7) / 8) {}

    /// Mark the card containing the given heap offset as dirty.
    /// Called by the write barrier when an old-gen object stores a
    /// reference to a young-gen object.
    void mark_dirty(uint32_t heap_offset) noexcept {
        const uint32_t card = heap_offset >> CARD_SHIFT;
        const uint32_t word = card / 32;
        const uint32_t bit = card % 32;
        if (word >= cards_.size()) [[unlikely]] return;
        cards_[word].fetch_or(1u << bit, std::memory_order_relaxed);
    }

    /// Check if the card containing the given heap offset is dirty.
    [[nodiscard]] bool is_dirty(uint32_t heap_offset) const noexcept {
        const uint32_t card = heap_offset >> CARD_SHIFT;
        const uint32_t word = card / 32;
        const uint32_t bit = card % 32;
        if (word >= cards_.size()) [[unlikely]] return false;
        return (cards_[word].load(std::memory_order_relaxed) >> bit) & 1;
    }

    /// Clear all dirty marks. Called after a full GC (not minor).
    void clear_all() noexcept {
        for (auto& w : cards_) {
            w.store(0, std::memory_order_relaxed);
        }
    }

    /// Iterate all dirty cards. The callback receives the card's base
    /// heap offset. Used by the minor GC to scan old→young references.
    template <typename Fn>
    void scan_dirty(Fn&& callback) const noexcept {
        const size_t num_words = cards_.size();
        for (size_t w = 0; w < num_words; ++w) {
            uint32_t bits = cards_[w].load(std::memory_order_relaxed);
            while (bits != 0) {
                const uint32_t bit = __builtin_ctz(bits);
                const uint32_t card = static_cast<uint32_t>(w) * 32 + bit;
                const uint32_t base_offset = card << CARD_SHIFT;
                callback(base_offset);
                bits &= bits - 1;  // clear lowest set bit
            }
        }
    }

    /// Number of dirty cards (for telemetry).
    [[nodiscard]] uint32_t dirty_count() const noexcept {
        uint32_t count = 0;
        for (const auto& w : cards_) {
            count += __builtin_popcount(w.load(std::memory_order_relaxed));
        }
        return count;
    }

private:
    std::vector<std::atomic<uint32_t>> cards_;
};

}  // namespace omni::gc

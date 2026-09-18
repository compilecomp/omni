// core/object_model/object_header.hpp
//
// Object header — every Omni object starts with one of these.
//
// Purpose:
//   Implements DESIGN.md §3.1: every object has {header, payload}.
//   The header carries GC bits, sync state, identity, shape reference,
//   and behavioral flags.
//
// Invariants:
//   - ObjectHeader is fixed-size (32 bytes) for cache-line friendliness.
//   - shape_ref is an atomic pointer to the current OmniShape. Multiple
//     threads may read concurrently; transitions write atomically.
//   - identity is a stable 64-bit id that survives shape transitions,
//     GC moving, and rehashing.
//   - sync_word is a thin-lock state (Laws Rule 118: no GIL).
//
// Edge cases:
//   - Forwarding pointers (during moving GC) live in the payload slot,
//     not in the header, so header layout stays stable across GC phases.
//   - Frozen / sealed / shared flags are sticky: once set, they may not
//     be cleared (DESIGN.md §3.1).
//
// Cross-references:
//   - DESIGN.md §3.1 (Object, ObjectHeader, payload kinds)
//   - LAWS.md Rule 78 (object identity must survive GC moves)
//   - LAWS.md Rule 118 (no GIL; thin locks)

#pragma once

#include <atomic>
#include <cstdint>

#include "core/common/flags.hpp"
#include "core/common/types.hpp"
#include "core/object_model/capability_bits.hpp"

namespace omni::object_model {

class OmniShape;

/// Object-level flags. Sticky bits (Frozen, Sealed, Shared) are write-once.
enum class ObjectFlag : uint32_t {
    None         = 0,
    Shared       = 1u << 0,  // visible to multiple tasks
    Frozen       = 1u << 1,  // immutable from now on
    Sealed       = 1u << 2,  // no new fields/traits may be added
    Monitored    = 1u << 3,  // has an associated monitor
    Finalizable  = 1u << 4,  // has a finalizer registered
    Disposable   = 1u << 5,  // implements `dispose`
    Weak         = 1u << 6,  // referenced by a WeakRef
    Native       = 1u << 7,  // wraps a native resource
    Virtualized  = 1u << 8,  // PEA-allocated virtual object
};

using ObjectFlags = common::Flags<ObjectFlag>;

/// GC mark/forwarding bits packed into one byte.
struct GcBits {
    uint8_t mark : 1;          // 0 = white, 1 = black (tri-color marking)
    uint8_t gray : 1;          // queued for scanning
    uint8_t generation : 2;   // 0 = nursery, 1 = old, 2 = ancient
    uint8_t forwarded : 1;    // set when forwarding pointer installed
    uint8_t pinned : 1;        // GC must not move this object
    uint8_t remembered : 1;    // in the remembered set (cross-gen write)
    uint8_t reserved : 1;
};
static_assert(sizeof(GcBits) == 1);

/// Thin-lock sync word. Lock-free fast path: an unlocked object holds
/// the owning thread id (or 0 for unlocked); a contended object promotes
/// to an inflated monitor pointer (DESIGN.md §3.1 sync_word).
class SyncWord {
public:
    constexpr SyncWord() noexcept : raw_(0) {}

    /// Try to acquire the lock for a given thread id. Returns true on success.
    bool try_acquire(uint32_t thread_id) noexcept {
        uint64_t expected = 0;
        return raw_.compare_exchange_strong(
            expected, thread_id, std::memory_order_acquire);
    }

    /// Release the lock. Caller must own it.
    void release(uint32_t thread_id) noexcept {
        [[assume(raw_.load(std::memory_order_relaxed) == thread_id)]];
        raw_.store(0, std::memory_order_release);
    }

    /// Inflate to a monitor pointer (slow path).
    void inflate(void* monitor) noexcept {
        // Tag the low bit so the runtime distinguishes monitor pointers
        // from thread ids (which are always small unsigned integers).
        raw_.store(reinterpret_cast<uint64_t>(monitor) | common::ONE_BIT,
                   std::memory_order_release);
    }

    [[nodiscard]] bool is_locked() const noexcept {
        return raw_.load(std::memory_order_relaxed) != 0;
    }
    [[nodiscard]] uint32_t owner_thread() const noexcept {
        uint64_t v = raw_.load(std::memory_order_relaxed);
        if (v & common::ONE_BIT) return 0;  // inflated to monitor
        return static_cast<uint32_t>(v);
    }

private:
    std::atomic<uint64_t> raw_;
};

struct ObjectHeader {
    /// Atomic pointer to current OmniShape. Reads are acquire;
    /// transitions are release-then-acquire (publish new shape, then
    /// invalidate dependents).
    std::atomic<OmniShape*> shape_ref{nullptr};

    /// Stable object identity. Survives shape transitions and GC moves.
    uint64_t identity{0};

    /// Sync word for fine-grained locking (no GIL).
    SyncWord sync_word{};

    /// Behavioral flags (Frozen, Sealed, Shared, ...).
    ObjectFlags flags{};

    /// GC state. Placed after the 4-byte flags so the whole header
    /// stays 32 bytes with no padding.
    GcBits gc_bits{};

    /// Reserved for future use. Keeps the header at 32 bytes.
    uint8_t reserved_[3] = {0, 0, 0};
};
static_assert(sizeof(ObjectHeader) == 32, "ObjectHeader must be 32 bytes");

}  // namespace omni::object_model

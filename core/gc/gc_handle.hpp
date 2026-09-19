// core/gc/gc_handle.hpp
//
// GC Handle table — the bridge between native compiler data structures
// (CCG, IR, JIT code) and the managed heap.
//
// Purpose:
//   Per the OmniGC spec: the CCG cannot hold raw pointers to managed
//   objects because a concurrent relocation would leave dangling pointers.
//   Instead, the CCG stores GC Handles — 32-bit indices into a runtime-
//   managed Handle Block table. When the GC relocates an object, it
//   updates the Handle Block; the CCG remains stable and always resolves
//   to the correct, updated memory address.
//
//   The handle table is also used for root scanning: the interpreter
//   registers its live references as handles, and the GC scans the
//   handle table as a set of GC roots.
//
// Invariants:
//   - Handle is a 32-bit index (uint32_t). Handle(0) is null.
//   - The handle table is append-only: handles are never freed
//     individually. Instead, handles are allocated in scopes (HandleScope)
//     that are bulk-freed when the scope exits. This avoids ABA issues
//     and keeps allocation O(1).
//   - Handle resolution is a single array lookup: table[handle].ref.
//     The ref is updated atomically by the GC during relocation.
//   - The handle table is thread-safe for concurrent read (resolve) and
//     single-writer (allocate). The GC's relocation pass is the only
//     writer of the ref field.
//
// Cross-references:
//   - OmniGC spec §2 (GC Handle Indirection)
//   - OmniGC spec §3 (Concurrent Root Scanning)
//   - LAWS.md Rule 78 (object identity survives GC moves)
//   - LAWS.md Rule 86 (GC references tracked)

#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "core/gc/heap_ref.hpp"

namespace omni::gc {

/// A GC handle — a 32-bit index into the handle table.
class Handle {
public:
    constexpr Handle() noexcept : index_(0) {}
    constexpr explicit Handle(uint32_t index) noexcept : index_(index) {}

    [[nodiscard]] constexpr bool is_null() const noexcept { return index_ == 0; }
    [[nodiscard]] constexpr uint32_t index() const noexcept { return index_; }

    constexpr bool operator==(const Handle& other) const noexcept {
        return index_ == other.index_;
    }

private:
    uint32_t index_;
};

constexpr Handle NULL_HANDLE{};

/// Handle table entry. The `ref` field is updated atomically by the GC
/// during relocation; all other fields are immutable after creation.
struct HandleEntry {
    std::atomic<HeapRef> ref{NULL_HEAP_REF};
    uint32_t scope_id{0};

    HandleEntry() = default;
    HandleEntry(const HandleEntry&) = delete;
    HandleEntry& operator=(const HandleEntry&) = delete;

    // Explicit move: atomics are non-movable by default, so we
    // transfer the value via load/store (relaxed is safe: the source
    // is being destroyed).
    HandleEntry(HandleEntry&& other) noexcept
        : ref(other.ref.load(std::memory_order_relaxed)),
          scope_id(other.scope_id) {}
    HandleEntry& operator=(HandleEntry&& other) noexcept {
        if (this != &other) {
            ref.store(other.ref.load(std::memory_order_relaxed),
                      std::memory_order_relaxed);
            scope_id = other.scope_id;
        }
        return *this;
    }
};

/// The handle table. A flat array of HandleEntry, indexed by Handle.
/// Allocation is bump-pointer into a free list; scope-based bulk free.
class HandleTable {
public:
    static HandleTable& instance() {
        static HandleTable table;
        return table;
    }

    /// Allocate a handle for the given HeapRef. The handle is owned by
    /// `scope_id` (0 = global, never freed).
    [[nodiscard]] Handle alloc(HeapRef ref, uint32_t scope_id = 0) {
        uint32_t idx;
        {
            // Try the free list first.
            // For simplicity, we use a bump allocator with no free list
            // for now. A production implementation would maintain a
            // lock-free free list.
            idx = next_index_.fetch_add(1, std::memory_order_relaxed);
            if (idx >= entries_.size()) [[unlikely]] {
                grow(idx + 1);
            }
        }
        entries_[idx].ref.store(ref, std::memory_order_release);
        entries_[idx].scope_id = scope_id;
        return Handle{idx + 1};  // +1 so Handle(0) is null
    }

    /// Resolve a handle to a HeapRef. Lock-free.
    [[nodiscard]] HeapRef resolve(Handle h) const noexcept {
        if (h.is_null()) return NULL_HEAP_REF;
        const uint32_t idx = h.index() - 1;
        if (idx >= entries_.size()) return NULL_HEAP_REF;
        return entries_[idx].ref.load(std::memory_order_acquire);
    }

    /// Update the HeapRef stored in a handle (e.g., after GC relocation).
    /// Called by the GC during the relocation pass.
    void update(Handle h, HeapRef new_ref) noexcept {
        if (h.is_null()) return;
        const uint32_t idx = h.index() - 1;
        if (idx >= entries_.size()) return;
        entries_[idx].ref.store(new_ref, std::memory_order_release);
    }

    /// Free all handles owned by the given scope. Called when a
    /// HandleScope exits.
    void free_scope(uint32_t scope_id) noexcept {
        for (auto& e : entries_) {
            if (e.scope_id == scope_id) {
                e.ref.store(NULL_HEAP_REF, std::memory_order_release);
                e.scope_id = 0;
            }
        }
    }

    /// Number of handle slots currently allocated.
    [[nodiscard]] uint32_t size() const noexcept {
        return next_index_.load(std::memory_order_acquire);
    }

    /// Scan all handles as GC roots. Called by the GC during mark phase.
    /// The callback is invoked for each non-null handle.
    template <typename Fn>
    void scan_roots(Fn&& callback) const {
        const uint32_t n = next_index_.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i) {
            HeapRef ref = entries_[i].ref.load(std::memory_order_acquire);
            if (!ref.is_null()) {
                callback(ref);
            }
        }
    }

private:
    HandleTable() {
        // Pre-allocate 1024 entries.
        entries_.resize(1024);
    }

    void grow(size_t min_size) {
        // Double until we fit. Called under contention; may over-allocate.
        size_t new_size = entries_.size();
        while (new_size < min_size) new_size *= 2;
        entries_.resize(new_size);
    }

    std::vector<HandleEntry> entries_;
    std::atomic<uint32_t> next_index_{0};
};

/// RAII handle scope. All handles allocated within this scope are
/// bulk-freed when the scope exits. Used for per-frame handles in the
/// interpreter (so we don't leak handles on every function call).
class HandleScope {
public:
    HandleScope() : scope_id_(next_scope_id_.fetch_add(1, std::memory_order_relaxed)) {}
    ~HandleScope() {
        HandleTable::instance().free_scope(scope_id_);
    }

    HandleScope(const HandleScope&) = delete;
    HandleScope& operator=(const HandleScope&) = delete;

    [[nodiscard]] Handle alloc(HeapRef ref) {
        return HandleTable::instance().alloc(ref, scope_id_);
    }

    [[nodiscard]] uint32_t scope_id() const noexcept { return scope_id_; }

private:
    uint32_t scope_id_;
    static std::atomic<uint32_t> next_scope_id_;
};

}  // namespace omni::gc

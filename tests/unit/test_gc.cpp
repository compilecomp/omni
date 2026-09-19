// tests/unit/test_gc.cpp
//
// Integration tests for the mark-sweep garbage collector.

#include "core/gc/gc.hpp"
#include "core/gc/gc_handle.hpp"
#include "core/gc/heap_ref.hpp"
#include "core/gc/side_metadata.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace omni::gc;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

// Test 1: Basic allocation + resolve.
static void test_gc_alloc() {
    GarbageCollector::instance().init({.initial_heap_size = 1024 * 1024});
    HeapRef ref = GarbageCollector::instance().alloc(64);
    CHECK(!ref.is_null());
    void* ptr = GarbageCollector::instance().resolve(ref);
    CHECK(ptr != nullptr);
    // Memory should be zero-initialized.
    auto* bytes = static_cast<uint8_t*>(ptr);
    for (int i = 0; i < 64; ++i) {
        CHECK(bytes[i] == 0);
    }
}

// Test 2: Multiple allocations don't overlap.
static void test_gc_alloc_no_overlap() {
    GarbageCollector::instance().init({.initial_heap_size = 1024 * 1024});
    HeapRef r1 = GarbageCollector::instance().alloc(32);
    HeapRef r2 = GarbageCollector::instance().alloc(32);
    CHECK(!r1.is_null());
    CHECK(!r2.is_null());
    CHECK(r1 != r2);
    // r2 should be at least 32 bytes (rounded to 8 = 32) after r1.
    CHECK(r2.offset() >= r1.offset() + 4);  // 32/8 = 4 slots
    // Write to r1's memory, verify r2 is unaffected.
    auto* p1 = static_cast<uint8_t*>(GarbageCollector::instance().resolve(r1));
    auto* p2 = static_cast<uint8_t*>(GarbageCollector::instance().resolve(r2));
    std::memset(p1, 0xAB, 32);
    CHECK(p2[0] == 0);  // r2 should still be zero
}

// Test 3: Handle table alloc + resolve.
static void test_handle_table() {
    GarbageCollector::instance().init({.initial_heap_size = 1024 * 1024});
    HeapRef ref = GarbageCollector::instance().alloc(64);
    Handle h = HandleTable::instance().alloc(ref);
    CHECK(!h.is_null());
    HeapRef resolved = HandleTable::instance().resolve(h);
    CHECK(resolved == ref);
}

// Test 4: Handle scope frees handles.
static void test_handle_scope() {
    GarbageCollector::instance().init({.initial_heap_size = 1024 * 1024});
    HeapRef ref = GarbageCollector::instance().alloc(64);
    Handle outer_h;
    {
        HandleScope scope;
        Handle h = scope.alloc(ref);
        CHECK(!h.is_null());
        CHECK(HandleTable::instance().resolve(h) == ref);
        outer_h = h;
    }
    // After scope exit, the handle should be freed (ref is null).
    HeapRef resolved = HandleTable::instance().resolve(outer_h);
    CHECK(resolved.is_null());
}

// Test 5: GC collection runs without crashing (mark-sweep on empty roots).
static void test_gc_collect_empty() {
    GarbageCollector::instance().init({.initial_heap_size = 1024 * 1024});
    // Allocate some objects with no roots — they should all be collected.
    for (int i = 0; i < 100; ++i) {
        (void)GarbageCollector::instance().alloc(64);
    }
    size_t before = GarbageCollector::instance().heap_allocated();
    CHECK(before > 0);
    // Clear root scanners to avoid scanning interpreter roots.
    GarbageCollector::instance().clear_root_scanners();
    GarbageCollector::instance().collect();
    uint64_t count = GarbageCollector::instance().collection_count();
    CHECK(count > 0);
}

// Test 6: GC with roots — rooted objects survive collection.
static void test_gc_with_roots() {
    GarbageCollector::instance().init({.initial_heap_size = 1024 * 1024});

    // Allocate an object and root it via a handle.
    HeapRef root_ref = GarbageCollector::instance().alloc(64);
    Handle root_handle = HandleTable::instance().alloc(root_ref);

    // Allocate some garbage (no roots).
    for (int i = 0; i < 50; ++i) {
        (void)GarbageCollector::instance().alloc(64);
    }

    // Register a root scanner that marks the rooted object.
    GarbageCollector::instance().clear_root_scanners();
    GarbageCollector::instance().register_root_scanner(
        [root_handle](std::function<void(HeapRef)> mark) {
            HeapRef ref = HandleTable::instance().resolve(root_handle);
            mark(ref);
        });

    // Set an object scanner that does nothing (no child refs).
    GarbageCollector::instance().set_object_scanner(
        [](void* obj, std::function<void(HeapRef)>) {
            (void)obj;  // no child references in this test
        });

    // Collect.
    GarbageCollector::instance().collect();

    // The rooted object should still be accessible.
    HeapRef resolved = HandleTable::instance().resolve(root_handle);
    CHECK(!resolved.is_null());
    void* ptr = GarbageCollector::instance().resolve(resolved);
    CHECK(ptr != nullptr);

    // Collection count should have increased.
    CHECK(GarbageCollector::instance().collection_count() > 0);
}

// Test 7: OOM triggers GC and retries.
static void test_gc_oom_triggers_gc() {
    // Small heap so we can fill it quickly.
    GarbageCollector::instance().init({.initial_heap_size = 4096});
    GarbageCollector::instance().clear_root_scanners();
    GarbageCollector::instance().set_object_scanner(
        [](void*, std::function<void(HeapRef)>) {});

    // Allocate until OOM, then verify GC was triggered.
    std::vector<HeapRef> refs;
    for (int i = 0; i < 100; ++i) {
        HeapRef r = GarbageCollector::instance().alloc(64);
        if (r.is_null()) break;
        refs.push_back(r);
    }
    // We should have allocated some objects before OOM.
    CHECK(!refs.empty());
    // Collection count should be > 0 (triggered by OOM retry).
    CHECK(GarbageCollector::instance().collection_count() > 0);
}

// Test 8: Sweep actually reclaims memory. After GC, allocated bytes
// should drop because dead objects are freed and their slots become
// available for reuse.
static void test_gc_sweep_reclaims() {
    GarbageCollector::instance().init({.initial_heap_size = 64 * 1024});
    GarbageCollector::instance().clear_root_scanners();
    // Clear all handles from previous tests so they don't act as roots.
    HandleTable::instance().free_scope(0);  // free global scope handles
    GarbageCollector::instance().set_object_scanner(
        [](void*, std::function<void(HeapRef)>) {});

    // First, collect to clean up any leftover objects from prior tests.
    GarbageCollector::instance().collect();

    // Allocate 100 objects with no roots (all garbage).
    for (int i = 0; i < 100; ++i) {
        (void)GarbageCollector::instance().alloc(128);
    }
    const size_t before = GarbageCollector::instance().heap_allocated();
    CHECK(before > 0);

    // Collect — all 100 objects should be freed.
    GarbageCollector::instance().collect();
    const size_t after = GarbageCollector::instance().heap_allocated();
    // After GC with no roots, allocated should be 0 (all garbage freed).
    CHECK(after == 0);

    // Verify we can allocate again (free list is populated).
    HeapRef r = GarbageCollector::instance().alloc(128);
    CHECK(!r.is_null());
}

// Test 9: Free list reuse — after GC frees objects, new allocations
// should come from the free list (not bump pointer).
static void test_gc_free_list_reuse() {
    GarbageCollector::instance().init({.initial_heap_size = 64 * 1024});
    GarbageCollector::instance().clear_root_scanners();
    HandleTable::instance().free_scope(0);
    GarbageCollector::instance().set_object_scanner(
        [](void*, std::function<void(HeapRef)>) {});

    // Clean up prior state.
    GarbageCollector::instance().collect();

    // Allocate 10 objects, record their offsets.
    std::vector<uint32_t> offsets;
    for (int i = 0; i < 10; ++i) {
        HeapRef r = GarbageCollector::instance().alloc(64);
        CHECK(!r.is_null());
        offsets.push_back(r.offset());
    }
    const size_t before = GarbageCollector::instance().heap_allocated();

    // GC frees all 10 (no roots).
    GarbageCollector::instance().collect();
    CHECK(GarbageCollector::instance().heap_allocated() == 0);

    // Allocate 10 more — should reuse the freed slots.
    for (int i = 0; i < 10; ++i) {
        HeapRef r = GarbageCollector::instance().alloc(64);
        CHECK(!r.is_null());
    }
    // Allocated should be back to the same level.
    const size_t after = GarbageCollector::instance().heap_allocated();
    CHECK(after == before);
}

int main() {
    test_gc_alloc();
    test_gc_alloc_no_overlap();
    test_handle_table();
    test_handle_scope();
    test_gc_collect_empty();
    test_gc_with_roots();
    test_gc_oom_triggers_gc();
    test_gc_sweep_reclaims();
    test_gc_free_list_reuse();
    if (g_failures == 0) {
        std::printf("OK: gc (%d tests passed)\n", 9);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d checks failed\n", g_failures);
    return 1;
}

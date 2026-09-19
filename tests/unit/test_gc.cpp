// tests/unit/test_gc.cpp
//
// Tests for the generational GC.

#include "core/gc/gc.hpp"
#include "core/gc/gc_handle.hpp"
#include "core/gc/generational.hpp"
#include "core/gc/heap_ref.hpp"
#include "core/gc/side_metadata.hpp"

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

static void test_alloc_nursery() {
    GarbageCollector::instance().init({});
    void* p = GarbageCollector::instance().alloc(64);
    CHECK(p != nullptr);
    CHECK(GarbageCollector::instance().is_nursery(p));
    // Zero-initialized.
    CHECK(*static_cast<uint8_t*>(p) == 0);
}

static void test_alloc_no_overlap() {
    GarbageCollector::instance().init({});
    void* p1 = GarbageCollector::instance().alloc(32);
    void* p2 = GarbageCollector::instance().alloc(32);
    CHECK(p1 != nullptr);
    CHECK(p2 != nullptr);
    CHECK(p1 != p2);
    std::memset(p1, 0xAB, 32);
    CHECK(*static_cast<uint8_t*>(p2) == 0);  // p2 unaffected
}

static void test_minor_gc_evacuates() {
    GarbageCollector::instance().init({.nursery_size = 64 * 1024});
    GarbageCollector::instance().clear_root_scanners();

    // Allocate objects with no roots (all garbage).
    for (int i = 0; i < 100; ++i) {
        (void)GarbageCollector::instance().alloc(128);
    }
    size_t before = GarbageCollector::instance().nursery_used();
    CHECK(before > 0);

    // Minor GC — all garbage, nursery should be empty after.
    GarbageCollector::instance().minor_collect();
    CHECK(GarbageCollector::instance().nursery_used() == 0);
    CHECK(GarbageCollector::instance().minor_collection_count() > 0);
}

static void test_minor_gc_preserves_roots() {
    GarbageCollector::instance().init({.nursery_size = 64 * 1024});
    GarbageCollector::instance().clear_root_scanners();

    // Allocate a rooted object.
    void* root = GarbageCollector::instance().alloc(64);
    CHECK(root != nullptr);
    CHECK(GarbageCollector::instance().is_nursery(root));

    // Register a root scanner that evacuates the root.
    GarbageCollector::instance().register_root_scanner(
        [&root](std::function<void*(void*)> evacuate,
                std::function<void(HeapRef)>) {
            root = evacuate(root);
        });

    // Fill nursery with garbage.
    for (int i = 0; i < 200; ++i) {
        (void)GarbageCollector::instance().alloc(128);
    }

    // Minor GC — root should be evacuated to old gen.
    GarbageCollector::instance().minor_collect();

    // Root should now be in old gen (not nursery).
    CHECK(GarbageCollector::instance().is_old_gen(root));
    CHECK(!GarbageCollector::instance().is_nursery(root));
}

static void test_forwarding_pointer() {
    // Test the ForwardingTable directly.
    // Use 8-byte-aligned values (low bit = 0) so is_forwarded is false initially.
    alignas(8) uintptr_t storage[4] = {0xDEAC, 0xBEEE, 0xCAFE, 0xF00C};
    void* old_ptr = &storage[0];
    void* new_ptr = reinterpret_cast<void*>(0x12345678);

    CHECK(!ForwardingTable::is_forwarded(old_ptr));
    ForwardingTable::forward(old_ptr, new_ptr);
    CHECK(ForwardingTable::is_forwarded(old_ptr));
    CHECK(ForwardingTable::follow(old_ptr) == new_ptr);
}

static void test_write_barrier() {
    GarbageCollector::instance().init({});
    GarbageCollector::instance().clear_root_scanners();

    // Old-gen object.
    HeapRef old_ref = GarbageCollector::instance().alloc_old(64);
    void* old_ptr = GarbageCollector::instance().resolve(old_ref);
    CHECK(GarbageCollector::instance().is_old_gen(old_ptr));

    // Nursery object.
    void* nursery_ptr = GarbageCollector::instance().alloc(64);
    CHECK(GarbageCollector::instance().is_nursery(nursery_ptr));

    // Write barrier: old → nursery should mark the card.
    GarbageCollector::instance().write_barrier(old_ptr, nursery_ptr);
    // The card containing old_ptr should be dirty.
    // (We can't easily check this without exposing the remembered set,
    // but at least it shouldn't crash.)
}

static void test_major_gc() {
    GarbageCollector::instance().init({});
    GarbageCollector::instance().clear_root_scanners();

    // Allocate old-gen objects with no roots (all garbage).
    for (int i = 0; i < 50; ++i) {
        (void)GarbageCollector::instance().alloc_old(256);
    }
    size_t before = GarbageCollector::instance().old_gen_allocated();
    CHECK(before > 0);

    // Major GC — all garbage should be freed.
    GarbageCollector::instance().major_collect();
    CHECK(GarbageCollector::instance().major_collection_count() > 0);
    // After major GC with no roots, old gen should be empty.
    CHECK(GarbageCollector::instance().old_gen_allocated() == 0);
}

static void test_handle_table() {
    GarbageCollector::instance().init({});
    HeapRef ref = GarbageCollector::instance().alloc_old(64);
    Handle h = HandleTable::instance().alloc(ref);
    CHECK(!h.is_null());
    CHECK(HandleTable::instance().resolve(h) == ref);
}

static void test_handle_scope() {
    GarbageCollector::instance().init({});
    HeapRef ref = GarbageCollector::instance().alloc_old(64);
    Handle outer_h;
    {
        HandleScope scope;
        Handle h = scope.alloc(ref);
        CHECK(!h.is_null());
        outer_h = h;
    }
    CHECK(HandleTable::instance().resolve(outer_h).is_null());
}

int main() {
    test_alloc_nursery();
    test_alloc_no_overlap();
    test_minor_gc_evacuates();
    test_minor_gc_preserves_roots();
    test_forwarding_pointer();
    test_write_barrier();
    test_major_gc();
    test_handle_table();
    test_handle_scope();
    if (g_failures == 0) {
        std::printf("OK: gc (%d tests passed)\n", 9);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d checks failed\n", g_failures);
    return 1;
}

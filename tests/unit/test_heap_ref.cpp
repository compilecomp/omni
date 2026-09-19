// tests/unit/test_heap_ref.cpp
//
// Unit tests for the 32-bit compressed reference + heap infrastructure.

#include "core/gc/heap_ref.hpp"
#include "core/gc/side_metadata.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace omni::gc;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void test_heap_ref_null() {
    HeapRef r;
    CHECK(r.is_null());
    CHECK(r.offset() == 0);
    CHECK(r == NULL_HEAP_REF);
}

static void test_heap_ref_resolve() {
    Heap heap(1024 * 1024);  // 1 MB
    HeapRef r = heap.alloc(64);
    CHECK(!r.is_null());
    CHECK(r.offset() != 0);
    void* ptr = heap.resolve(r);
    CHECK(ptr != nullptr);
    // The resolved pointer should be within the heap.
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t base = heap.base();
    CHECK(addr >= base);
    CHECK(addr < base + heap.capacity());
}

static void test_heap_ref_alignment() {
    Heap heap(1024 * 1024);
    HeapRef r1 = heap.alloc(7);  // request 7 bytes, get 8
    HeapRef r2 = heap.alloc(16);
    CHECK(!r1.is_null());
    CHECK(!r2.is_null());
    // r2 should be 8 bytes after r1 (rounded up).
    CHECK(r2.offset() == r1.offset() + 1);
    // Allocated bytes should be 8 + 16 = 24.
    CHECK(heap.allocated() == 24);
}

static void test_heap_ref_zero_init() {
    Heap heap(1024 * 1024);
    HeapRef r = heap.alloc(32);
    CHECK(!r.is_null());
    uint8_t* ptr = static_cast<uint8_t*>(heap.resolve(r));
    for (int i = 0; i < 32; ++i) {
        CHECK(ptr[i] == 0);
    }
}

static void test_heap_ref_oom() {
    Heap heap(64 + 8);  // tiny heap (64 usable + 8 reserved for null slot)
    HeapRef r = heap.alloc(64);  // exactly fills usable space
    CHECK(!r.is_null());
    HeapRef r2 = heap.alloc(8);  // should fail
    CHECK(r2.is_null());
}

static void test_side_metadata_colors() {
    SideMetadata meta(1024 * 1024);  // 1 MB heap
    Heap heap(1024 * 1024);
    HeapRef r = heap.alloc(16);
    CHECK(!r.is_null());

    // Default color is White (0).
    CHECK(meta.get_color(r) == GcColor::White);
    CHECK(!meta.is_live(r));

    // Mark it Gray.
    meta.set_color(r, GcColor::Gray);
    CHECK(meta.get_color(r) == GcColor::Gray);

    // Mark it Black.
    meta.set_color(r, GcColor::Black);
    CHECK(meta.get_color(r) == GcColor::Black);
    CHECK(meta.is_live(r));

    // Mark it Remapped (relocated).
    meta.set_color(r, GcColor::Remapped);
    CHECK(meta.get_color(r) == GcColor::Remapped);
    CHECK(meta.needs_relocation(r));
    CHECK(meta.is_live(r));
}

static void test_side_metadata_independence() {
    // Two adjacent slots should have independent colors.
    SideMetadata meta(1024 * 1024);
    Heap heap(1024 * 1024);
    HeapRef r1 = heap.alloc(8);
    HeapRef r2 = heap.alloc(8);
    CHECK(r1.offset() != r2.offset());

    meta.set_color(r1, GcColor::Black);
    meta.set_color(r2, GcColor::Gray);
    CHECK(meta.get_color(r1) == GcColor::Black);
    CHECK(meta.get_color(r2) == GcColor::Gray);
}

static void test_heap_ref_from_ptr() {
    Heap heap(1024 * 1024);
    HeapRef r = heap.alloc(32);
    void* ptr = heap.resolve(r);
    HeapRef r2 = HeapRef::from_ptr(ptr, heap.base());
    CHECK(r == r2);
}

int main() {
    test_heap_ref_null();
    test_heap_ref_resolve();
    test_heap_ref_alignment();
    test_heap_ref_zero_init();
    test_heap_ref_oom();
    test_side_metadata_colors();
    test_side_metadata_independence();
    test_heap_ref_from_ptr();
    if (g_failures == 0) {
        std::printf("OK: heap_ref (%d checks passed)\n", 8);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d checks failed\n", g_failures);
    return 1;
}

// tests/unit/test_compact_header.cpp
//
// Unit tests for the 8-byte compact object header.

#include "core/object_model/compact_header.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace omni::object_model;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

static void test_size() {
    CHECK(sizeof(CompactHeader) == 8);
}

static void test_default_init() {
    CompactHeader h;
    CHECK(h.shape_id == 0);
    CHECK(h.flags == 0);
    CHECK(h.gc_color() == 0);  // White
    CHECK(h.generation() == 0);  // nursery
    CHECK(h.lock_state() == 0);  // unlocked
    CHECK(!h.is_frozen());
    CHECK(!h.is_sealed());
    CHECK(!h.is_shared());
}

static void test_gc_color() {
    CompactHeader h;
    h.set_gc_color(2);  // Black
    CHECK(h.gc_color() == 2);
    CHECK((h.flags & 0x3) == 2);

    h.set_gc_color(3);  // Remapped
    CHECK(h.gc_color() == 3);

    h.set_gc_color(0);  // White
    CHECK(h.gc_color() == 0);
}

static void test_generation() {
    CompactHeader h;
    h.set_generation(1);  // old
    CHECK(h.generation() == 1);
    CHECK(!h.is_frozen());  // setting generation shouldn't affect other fields
}

static void test_lock_state() {
    CompactHeader h;
    h.set_lock_state(1);  // Locked
    CHECK(h.lock_state() == 1);

    h.set_lock_state(2);  // Inflated
    CHECK(h.lock_state() == 2);
}

static void test_behavioral_flags() {
    CompactHeader h;
    h.set_frozen();
    CHECK(h.is_frozen());

    h.set_sealed();
    CHECK(h.is_sealed());

    h.set_shared();
    CHECK(h.is_shared());

    // Verify flags don't interfere with each other.
    CHECK(h.is_frozen());
    CHECK(h.is_sealed());
    CHECK(h.is_shared());
}

static void test_combined() {
    CompactHeader h{42, 0};  // shape_id=42, flags=0
    h.set_gc_color(2);       // Black
    h.set_generation(1);     // old
    h.set_frozen();

    CHECK(h.shape_id == 42);
    CHECK(h.gc_color() == 2);
    CHECK(h.generation() == 1);
    CHECK(h.is_frozen());
    CHECK(!h.is_shared());
}

static void test_sync_table() {
    auto& st = SyncTable::instance();
    void* m1 = st.inflate(100);
    CHECK(m1 != nullptr);
    void* m2 = st.get(100);
    CHECK(m1 == m2);  // same monitor on second lookup
    void* m3 = st.get(200);
    CHECK(m3 == nullptr);  // not inflated yet
}

int main() {
    test_size();
    test_default_init();
    test_gc_color();
    test_generation();
    test_lock_state();
    test_behavioral_flags();
    test_combined();
    test_sync_table();
    if (g_failures == 0) {
        std::printf("OK: compact_header (%d tests passed)\n", 8);
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d checks failed\n", g_failures);
    return 1;
}

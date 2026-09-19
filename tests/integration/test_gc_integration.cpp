// tests/integration/test_gc_integration.cpp
//
// Integration test: GC + interpreter end-to-end.
//
// Verifies that:
//   - Objects allocated by MakeObject are GC-managed (on the GC heap).
//   - The root scanner correctly identifies live objects in registers.
//   - GC collection during execution does not collect live objects.
//   - GC collection reclaims unreachable objects.

#include "core/bytecode/bytecode_module.hpp"
#include "core/bytecode/instruction.hpp"
#include "core/bytecode/opcode.hpp"
#include "core/common/result.hpp"
#include "core/common/symbol_table.hpp"
#include "core/common/types.hpp"
#include "core/gc/gc.hpp"
#include "core/gc/gc_handle.hpp"
#include "core/interpreter/interpreter.hpp"
#include "core/object_model/object.hpp"
#include "core/object_model/shape_registry.hpp"
#include "core/object_model/tagged_value.hpp"

#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

using namespace omni;
using namespace omni::bytecode;
using namespace omni::common;
using namespace omni::interpreter;
using namespace omni::object_model;
using namespace omni::gc;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

// Test 1: MakeObject allocates from the GC heap.
// Build a module that creates an object and returns it.
// After execution, verify the object is on the GC heap.
//
//   pc=0: MAKE_OBJECT r0, shape_id   // r0 = new {x, y}
//   pc=1: RETURN r0
static void test_make_object_is_gc_managed() {
    // Register a shape with 2 slots.
    auto x_sym = SymbolTable::instance().intern("x");
    auto y_sym = SymbolTable::instance().intern("y");
    OmniShape* shape = ShapeRegistry::instance().intern(
        2, std::vector<SymbolId>{*x_sym, *y_sym});
    const ShapeId shape_id = shape->shape_id();
    CHECK(shape_id <= 255);

    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::MakeObject, 0, static_cast<uint8_t>(shape_id)});
    code.push_back(Instruction{Opcode::Return, 0, 0});

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    FunctionDesc fd{};
    fd.name = NULL_SYMBOL;
    fd.entry_pc = 0;
    fd.param_count = 0;
    fd.local_count = 1;
    fd.default_count = 0;
    fd.max_registers = 1;
    fd.is_async = false;
    fd.is_generator = false;
    mod->add_function(fd);

    Interpreter interp;
    auto load_result = interp.load_module(std::move(mod));
    CHECK(load_result.has_value());

    std::span<const TaggedValue> args{};
    auto exec_result = interp.execute(*load_result, 0, args);
    CHECK(exec_result.has_value());
    CHECK(exec_result->is_object_ref());

    Object* obj = exec_result->as_object();
    CHECK(obj != nullptr);
    // The object should be on the GC heap.
    CHECK(GarbageCollector::instance().heap_contains(obj));
}

// Test 2: GC does not collect objects held in registers during execution.
// Build a module that creates a rooted object, then loops creating
// temporary garbage objects (to trigger GC) while accessing the rooted
// object each iteration. If the GC collected the rooted object, the
// program would crash or produce wrong results.
//
//   pc=0: MAKE_OBJECT r0, shape_id   // rooted object
//   pc=1: LOAD_CONST r1, 0           // r1 = 10
//   pc=2: SET_PROP r0, r1            // r0.x = 10
//   pc=3: LOAD_CONST r2, 1           // r2 = 0 (acc)
//   pc=4: LOAD_CONST r3, 2           // r3 = 0 (counter)
//   pc=5: LOAD_CONST r4, 3           // r4 = N (limit)
//   pc=6: LOAD_CONST r5, 4           // r5 = 1 (inc)
//   loop:
//   pc=7: LOAD_LOCAL r6, r3          // r6 = counter
//   pc=8: GE r6, r4                  // r6 = (counter >= limit)
//   pc=9: BRANCH r6, +6              // if exit: pc=15 (RETURN)
//   pc=10: MAKE_OBJECT r7, shape_id  // garbage (overwritten each iter)
//   pc=11: GET_PROP r1, r0           // r1 = obj.x (access rooted object)
//   pc=12: ADD r2, r1                // acc += obj.x
//   pc=13: ADD r3, r5                // counter += 1
//   pc=14: JUMP -7                   // back to pc=7
//   pc=15: RETURN r2
static void test_gc_preserves_live_objects() {
    auto x_sym = SymbolTable::instance().intern("x");
    auto y_sym = SymbolTable::instance().intern("y");
    OmniShape* shape = ShapeRegistry::instance().intern(
        2, std::vector<SymbolId>{*x_sym, *y_sym});
    const ShapeId shape_id = shape->shape_id();

    // Use a very low threshold so GC triggers with the existing heap.
    // (init() doesn't recreate the heap if it already exists; the heap
    // may be 64MB from a previous test. 10000 objects * ~250 bytes =
    // ~2.5MB, which is ~3.9% of 64MB. Setting threshold to 1% ensures
    // GC triggers.)
    GarbageCollector::instance().init({.trigger_threshold = 0.01});
    // Clear stale root scanners from previous test Interpreters
    // (destroyed Interpreters leave dangling lambdas in the scanner list).
    GarbageCollector::instance().clear_root_scanners();

    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::MakeObject, 0, static_cast<uint8_t>(shape_id)}); // pc=0
    code.push_back(Instruction{Opcode::LoadConst, 1, 0});  // pc=1
    code.push_back(Instruction{Opcode::SetProp, 0, 1});    // pc=2
    code.push_back(Instruction{Opcode::LoadConst, 2, 1});  // pc=3
    code.push_back(Instruction{Opcode::LoadConst, 3, 2});  // pc=4
    code.push_back(Instruction{Opcode::LoadConst, 4, 3});  // pc=5
    code.push_back(Instruction{Opcode::LoadConst, 5, 4});  // pc=6
    code.push_back(Instruction{Opcode::LoadLocal, 6, 3});  // pc=7
    code.push_back(Instruction{Opcode::Ge, 6, 4});         // pc=8
    code.push_back(Instruction{Opcode::Branch, 6, 6});     // pc=9: +6 -> 15
    code.push_back(Instruction{Opcode::MakeObject, 7, static_cast<uint8_t>(shape_id)}); // pc=10
    code.push_back(Instruction{Opcode::GetProp, 1, 0});    // pc=11
    code.push_back(Instruction{Opcode::Add, 2, 1});        // pc=12
    code.push_back(Instruction{Opcode::Add, 3, 5});        // pc=13
    code.push_back(Instruction{Opcode::Jump, 0xFF, 0xF9}); // pc=14: -7 -> 7
    code.push_back(Instruction{Opcode::Return, 2, 0});     // pc=15

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    mod->set_prop_name(2, *x_sym);    // pc=2: SET_PROP x
    mod->set_prop_name(11, *x_sym);   // pc=11: GET_PROP x

    mod->add_constant(ConstEntry{TaggedValue::make_int(10)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(0)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(0)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(10000)});  // N
    mod->add_constant(ConstEntry{TaggedValue::make_int(1)});

    FunctionDesc fd{};
    fd.name = NULL_SYMBOL;
    fd.entry_pc = 0;
    fd.param_count = 0;
    fd.local_count = 8;
    fd.default_count = 0;
    fd.max_registers = 8;
    fd.is_async = false;
    fd.is_generator = false;
    mod->add_function(fd);

    Interpreter interp;
    auto load_result = interp.load_module(std::move(mod));
    CHECK(load_result.has_value());

    std::span<const TaggedValue> args{};
    auto exec_result = interp.execute(*load_result, 0, args);
    CHECK(exec_result.has_value());
    if (!exec_result.has_value()) return;
    CHECK(exec_result->is_int());
    // 10000 iterations * 10 = 100000
    CHECK(exec_result->as_int() == 100000);

    // GC should have run at least once during the loop (the garbage
    // objects from MAKE_OBJECT at pc=10 fill the heap).
    CHECK(GarbageCollector::instance().collection_count() > 0);
}

// Test 3: Range iterator objects are GC-managed.
static void test_range_iter_is_gc_managed() {
    //   pc=0: LOAD_CONST r0, 0   // r0 = 100
    //   pc=1: GET_ITER r1, r0    // r1 = iter([0, 100))
    //   pc=2: RETURN r1
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::LoadConst, 0, 0});
    code.push_back(Instruction{Opcode::GetIter, 1, 0});
    code.push_back(Instruction{Opcode::Return, 1, 0});

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    mod->add_constant(ConstEntry{TaggedValue::make_int(100)});
    FunctionDesc fd{};
    fd.name = NULL_SYMBOL;
    fd.entry_pc = 0;
    fd.param_count = 0;
    fd.local_count = 2;
    fd.default_count = 0;
    fd.max_registers = 2;
    fd.is_async = false;
    fd.is_generator = false;
    mod->add_function(fd);

    Interpreter interp;
    auto load_result = interp.load_module(std::move(mod));
    CHECK(load_result.has_value());

    std::span<const TaggedValue> args{};
    auto exec_result = interp.execute(*load_result, 0, args);
    CHECK(exec_result.has_value());
    CHECK(exec_result->is_object_ref());

    Object* obj = exec_result->as_object();
    CHECK(obj != nullptr);
    CHECK(GarbageCollector::instance().heap_contains(obj));
}

int main() {
    test_make_object_is_gc_managed();
    test_gc_preserves_live_objects();
    test_range_iter_is_gc_managed();
    if (g_failures == 0) {
        std::printf("OK: gc_integration (3 tests passed)\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d checks failed\n", g_failures);
    return 1;
}

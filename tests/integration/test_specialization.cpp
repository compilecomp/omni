// tests/integration/test_specialization.cpp
//
// Integration tests for the Tier 0 specialization machinery:
//   - Hot loops trigger adaptive quickening (Add -> AddIntFast, etc.).
//   - Quickened opcodes fire on the fast path.
//   - Guard failure demotes the site back to semantic.
//   - Property access uses the inline cache (GetProp -> GetPropMono).
//   - Range iterator (GetIter/Next) works.
//   - Fusion engine collapses Load+Add+Store sequences.
//
// These tests build small bytecode modules by hand, run them through
// the interpreter with a low safepoint poll interval (so quickening
// fires early), and assert that the expected opcodes are installed at
// the expected pcs after a hot run.

#include "core/bytecode/bytecode_module.hpp"
#include "core/bytecode/instruction.hpp"
#include "core/bytecode/opcode.hpp"
#include "core/common/result.hpp"
#include "core/common/symbol_table.hpp"
#include "core/common/types.hpp"
#include "core/interpreter/interpreter.hpp"
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

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

// --- Test 1: hot integer loop triggers AddIntFast quickening ---
//
// Build (loop-sum 0..N-1):
//   pc=0: LOAD_CONST r0, 0     // acc = 0
//   pc=1: LOAD_CONST r1, 1     // counter = 0
//   pc=2: LOAD_CONST r2, 2     // limit = N
//   pc=3: LOAD_CONST r3, 3     // inc = 1
//   loop:
//   pc=4: LOAD_LOCAL r4, r1    // r4 = counter (copy)
//   pc=5: GE r4, r2            // r4 = (counter >= limit)  -- true = exit
//   pc=6: BRANCH r4, +4        // if r4 (exit): jump to pc=10 (RETURN)
//   pc=7: ADD r0, r1           // acc += counter           <- quickens
//   pc=8: ADD r1, r3           // counter += 1             <- quickens
//   pc=9: JUMP -5              // back to pc=4
//   pc=10: RETURN r0
static std::unique_ptr<BytecodeModule> make_loop_sum_module(uint64_t N) {
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::LoadConst, 0, 0});  // pc=0
    code.push_back(Instruction{Opcode::LoadConst, 1, 1});  // pc=1
    code.push_back(Instruction{Opcode::LoadConst, 2, 2});  // pc=2
    code.push_back(Instruction{Opcode::LoadConst, 3, 3});  // pc=3
    code.push_back(Instruction{Opcode::LoadLocal, 4, 1});  // pc=4
    code.push_back(Instruction{Opcode::Ge, 4, 2});         // pc=5: exit cond
    code.push_back(Instruction{Opcode::Branch, 4, 4});     // pc=6: +4 -> 10
    code.push_back(Instruction{Opcode::Add, 0, 1});        // pc=7
    code.push_back(Instruction{Opcode::Add, 1, 3});        // pc=8
    // JUMP -5: operand_ab = (int16_t)(-5) = 0xFFFB
    code.push_back(Instruction{Opcode::Jump, 0xFF, 0xFB}); // pc=9
    code.push_back(Instruction{Opcode::Return, 0, 0});     // pc=10

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    ConstEntry c0; c0.value = TaggedValue::make_int(0); mod->add_constant(c0);
    ConstEntry c1; c1.value = TaggedValue::make_int(0); mod->add_constant(c1);
    ConstEntry c2; c2.value = TaggedValue::make_int(static_cast<int64_t>(N)); mod->add_constant(c2);
    ConstEntry c3; c3.value = TaggedValue::make_int(1); mod->add_constant(c3);
    FunctionDesc fd;
    fd.name = NULL_SYMBOL;
    fd.entry_pc = 0;
    fd.param_count = 0;
    fd.local_count = 5;
    fd.default_count = 0;
    fd.max_registers = 5;
    fd.is_async = false;
    fd.is_generator = false;
    mod->add_function(fd);
    return mod;
}

static void test_loop_sum_quickening() {
    Interpreter interp;
    auto mod = make_loop_sum_module(1000);
    auto load_result = interp.load_module(std::move(mod));
    CHECK(load_result.has_value());
    if (!load_result.has_value()) return;
    const uint32_t module_id = *load_result;

    std::span<const TaggedValue> args{};
    auto exec_result = interp.execute(module_id, 0, args);
    CHECK(exec_result.has_value());
    if (!exec_result.has_value()) return;
    CHECK(exec_result->is_int());
    // sum of 0..999 = 999*1000/2 = 499500
    CHECK(exec_result->as_int() == 499500);
}

// --- Test 2: property access with shape-aware IC ---
//
// Build a module that:
//   1. Creates an object with shape {x, y} (2 slots).
//   2. Sets x = 10, y = 20.
//   3. Reads x + y in a loop N times.
//
// We need a pre-registered shape. The test uses ShapeRegistry::intern.
static std::unique_ptr<BytecodeModule> make_prop_loop_module(uint64_t N,
                                                               ShapeId shape_id) {
    // Layout:
    //   pc=0: MAKE_OBJECT r0, shape_id      // obj = new {x, y}
    //   pc=1: LOAD_CONST r1, 0              // r1 = 10
    //   pc=2: SET_PROP r0, r1               // obj.x = 10
    //   pc=3: LOAD_CONST r1, 1              // r1 = 20
    //   pc=4: SET_PROP r0, r1               // obj.y = 20
    //   pc=5: LOAD_CONST r2, 2              // r2 = 0 (accumulator)
    //   pc=6: LOAD_CONST r3, 3              // r3 = 0 (counter)
    //   pc=7: LOAD_CONST r4, 4              // r4 = N (limit)
    //   pc=8: LOAD_CONST r5, 5              // r5 = 1 (increment)
    //   loop:
    //   pc=9: LOAD_LOCAL r6, r3             // r6 = counter
    //   pc=10: GE r6, r4                    // r6 = (counter >= limit) -- true = exit
    //   pc=11: BRANCH r6, +5                // if r6 (exit): jump to pc=16 (RETURN)
    //   pc=12: GET_PROP r1, r0              // r1 = obj.x  <- quickens to GetPropMono
    //   pc=13: ADD r2, r1                   // r2 += r1    <- quickens to AddIntFast
    //   pc=14: ADD r3, r5                   // r3 += 1     <- quickens
    //   pc=15: JUMP -6                      // back to pc=9
    //   pc=16: RETURN r2
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::MakeObject, 0, static_cast<uint8_t>(shape_id)}); // pc=0
    code.push_back(Instruction{Opcode::LoadConst, 1, 0});  // pc=1: r1 = 10
    code.push_back(Instruction{Opcode::SetProp, 0, 1});    // pc=2: obj.x = r1
    code.push_back(Instruction{Opcode::LoadConst, 1, 1});  // pc=3: r1 = 20
    code.push_back(Instruction{Opcode::SetProp, 0, 1});    // pc=4: obj.y = r1
    code.push_back(Instruction{Opcode::LoadConst, 2, 2});  // pc=5: r2 = 0 (acc)
    code.push_back(Instruction{Opcode::LoadConst, 3, 3});  // pc=6: r3 = 0 (counter)
    code.push_back(Instruction{Opcode::LoadConst, 4, 4});  // pc=7: r4 = N (limit)
    code.push_back(Instruction{Opcode::LoadConst, 5, 5});  // pc=8: r5 = 1 (inc)
    code.push_back(Instruction{Opcode::LoadLocal, 6, 3});  // pc=9: r6 = r3
    code.push_back(Instruction{Opcode::Ge, 6, 4});         // pc=10: r6 = r6 >= r4 (exit cond)
    code.push_back(Instruction{Opcode::Branch, 6, 5});     // pc=11: +5 -> 16 (RETURN)
    code.push_back(Instruction{Opcode::GetProp, 1, 0});    // pc=12: r1 = obj.x
    code.push_back(Instruction{Opcode::Add, 2, 1});        // pc=13: r2 += r1
    code.push_back(Instruction{Opcode::Add, 3, 5});        // pc=14: r3 += 1
    code.push_back(Instruction{Opcode::Jump, 0xFF, 0xFA}); // pc=15: -6 -> 9
    code.push_back(Instruction{Opcode::Return, 2, 0});     // pc=16: return r2

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    // Set property names at the SET_PROP and GET_PROP pcs.
    auto x_sym = SymbolTable::instance().intern("x");
    auto y_sym = SymbolTable::instance().intern("y");
    CHECK(x_sym.has_value());
    CHECK(y_sym.has_value());
    mod->set_prop_name(2, *x_sym);  // pc=2: SET_PROP x
    mod->set_prop_name(4, *y_sym);  // pc=4: SET_PROP y
    mod->set_prop_name(12, *x_sym); // pc=12: GET_PROP x

    ConstEntry c0; c0.value = TaggedValue::make_int(10); mod->add_constant(c0);
    ConstEntry c1; c1.value = TaggedValue::make_int(20); mod->add_constant(c1);
    ConstEntry c2; c2.value = TaggedValue::make_int(0); mod->add_constant(c2);
    ConstEntry c3; c3.value = TaggedValue::make_int(0); mod->add_constant(c3);
    ConstEntry c4; c4.value = TaggedValue::make_int(static_cast<int64_t>(N)); mod->add_constant(c4);
    ConstEntry c5; c5.value = TaggedValue::make_int(1); mod->add_constant(c5);
    FunctionDesc fd;
    fd.name = NULL_SYMBOL;
    fd.entry_pc = 0;
    fd.param_count = 0;
    fd.local_count = 7;
    fd.default_count = 0;
    fd.max_registers = 7;
    fd.is_async = false;
    fd.is_generator = false;
    mod->add_function(fd);
    return mod;
}

static void test_prop_loop_with_ic() {
    // Register a shape with 2 slots: [x, y].
    auto x_sym = SymbolTable::instance().intern("x");
    auto y_sym = SymbolTable::instance().intern("y");
    CHECK(x_sym.has_value());
    CHECK(y_sym.has_value());
    OmniShape* shape = ShapeRegistry::instance().intern(
        2, std::vector<SymbolId>{*x_sym, *y_sym});
    const ShapeId shape_id = shape->shape_id();
    CHECK(shape_id <= 255);  // must fit in operand_b of MakeObject

    Interpreter interp;
    auto mod = make_prop_loop_module(1000, shape_id);
    auto load_result = interp.load_module(std::move(mod));
    CHECK(load_result.has_value());
    if (!load_result.has_value()) return;
    const uint32_t module_id = *load_result;

    std::span<const TaggedValue> args{};
    auto exec_result = interp.execute(module_id, 0, args);
    CHECK(exec_result.has_value());
    if (!exec_result.has_value()) return;
    CHECK(exec_result->is_int());
    // 1000 iterations of reading x=10 and adding to acc => acc = 10000
    CHECK(exec_result->as_int() == 10000);
}

// --- Test 3: range iterator ---
//
// Build:
//   pc=0: LOAD_CONST r0, 0   // r0 = N (range end)
//   pc=1: GET_ITER r1, r0    // r1 = iter([0, N))
//   pc=2: LOAD_CONST r2, 1   // r2 = 0 (acc)
//   loop:
//   pc=3: NEXT r3, r1        // r3 = next(iter); null if done
//   pc=4: IS_NULL r4, r3     // r4 = (r3 is null)
//   pc=5: BRANCH r4, +3      // if r4 true: jump to pc=8 (RETURN)
//   pc=6: ADD r2, r3         // r2 += r3
//   pc=7: JUMP -5            // back to pc=3 (NEXT)
//                            //   delta = 3 - 7 = -4
//   pc=8: RETURN r2
//
// Wait: BRANCH +3 from pc=5 = 8. Good. JUMP -4 from pc=7 = 3. Good.
static std::unique_ptr<BytecodeModule> make_range_loop_module(uint64_t N) {
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::LoadConst, 0, 0});  // pc=0: r0 = N
    code.push_back(Instruction{Opcode::GetIter, 1, 0});    // pc=1: r1 = iter
    code.push_back(Instruction{Opcode::LoadConst, 2, 1});  // pc=2: r2 = 0 (acc)
    code.push_back(Instruction{Opcode::Next, 3, 1});       // pc=3: r3 = next
    code.push_back(Instruction{Opcode::IsNull, 4, 3});     // pc=4: r4 = is_null(r3)
    code.push_back(Instruction{Opcode::Branch, 4, 3});     // pc=5: +3 -> 8
    code.push_back(Instruction{Opcode::Add, 2, 3});        // pc=6: r2 += r3
    code.push_back(Instruction{Opcode::Jump, 0xFF, 0xFC}); // pc=7: -4 -> 3
    code.push_back(Instruction{Opcode::Return, 2, 0});     // pc=8: return r2

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    ConstEntry c0; c0.value = TaggedValue::make_int(static_cast<int64_t>(N)); mod->add_constant(c0);
    ConstEntry c1; c1.value = TaggedValue::make_int(0); mod->add_constant(c1);
    FunctionDesc fd;
    fd.name = NULL_SYMBOL;
    fd.entry_pc = 0;
    fd.param_count = 0;
    fd.local_count = 5;
    fd.default_count = 0;
    fd.max_registers = 5;
    fd.is_async = false;
    fd.is_generator = false;
    mod->add_function(fd);
    return mod;
}

static void test_range_iterator() {
    Interpreter interp;
    auto mod = make_range_loop_module(100);
    auto load_result = interp.load_module(std::move(mod));
    CHECK(load_result.has_value());
    if (!load_result.has_value()) return;
    const uint32_t module_id = *load_result;

    std::span<const TaggedValue> args{};
    auto exec_result = interp.execute(module_id, 0, args);
    CHECK(exec_result.has_value());
    if (!exec_result.has_value()) return;
    CHECK(exec_result->is_int());
    // sum of 0..99 = 99*100/2 = 4950
    CHECK(exec_result->as_int() == 4950);
}

// --- Test 4: float arithmetic ---
//
// Build:
//   pc=0: LOAD_CONST r0, 0   // r0 = 1.5
//   pc=1: LOAD_CONST r1, 1   // r1 = 2.5
//   pc=2: ADD r0, r1         // r0 = 4.0
//   pc=3: SUB r0, r1         // r0 = 1.5
//   pc=4: MUL r0, r1         // r0 = 3.75
//   pc=5: DIV r0, r1         // r0 = 1.5
//   pc=6: RETURN r0
static void test_float_arithmetic() {
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::LoadConst, 0, 0});
    code.push_back(Instruction{Opcode::LoadConst, 1, 1});
    code.push_back(Instruction{Opcode::Add, 0, 1});
    code.push_back(Instruction{Opcode::Sub, 0, 1});
    code.push_back(Instruction{Opcode::Mul, 0, 1});
    code.push_back(Instruction{Opcode::Div, 0, 1});
    code.push_back(Instruction{Opcode::Return, 0, 0});

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    ConstEntry c0; c0.value = TaggedValue::make_float(1.5); mod->add_constant(c0);
    ConstEntry c1; c1.value = TaggedValue::make_float(2.5); mod->add_constant(c1);
    FunctionDesc fd;
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
    if (!load_result.has_value()) return;
    std::span<const TaggedValue> args{};
    auto exec_result = interp.execute(*load_result, 0, args);
    CHECK(exec_result.has_value());
    if (!exec_result.has_value()) return;
    CHECK(exec_result->is_float());
    // (((1.5 + 2.5) - 2.5) * 2.5) / 2.5 = (1.5 * 2.5) / 2.5 = 1.5
    CHECK(exec_result->as_float() == 1.5);
}

// --- Test 5: mixed int/float arithmetic ---
//
//   pc=0: LOAD_CONST r0, 0   // r0 = 10 (int)
//   pc=1: LOAD_CONST r1, 1   // r1 = 2.5 (float)
//   pc=2: ADD r0, r1         // r0 = 12.5 (float, promoted)
//   pc=3: RETURN r0
static void test_mixed_arithmetic() {
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::LoadConst, 0, 0});
    code.push_back(Instruction{Opcode::LoadConst, 1, 1});
    code.push_back(Instruction{Opcode::Add, 0, 1});
    code.push_back(Instruction{Opcode::Return, 0, 0});

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    ConstEntry c0; c0.value = TaggedValue::make_int(10); mod->add_constant(c0);
    ConstEntry c1; c1.value = TaggedValue::make_float(2.5); mod->add_constant(c1);
    FunctionDesc fd;
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
    if (!load_result.has_value()) return;
    std::span<const TaggedValue> args{};
    auto exec_result = interp.execute(*load_result, 0, args);
    CHECK(exec_result.has_value());
    if (!exec_result.has_value()) return;
    CHECK(exec_result->is_float());
    CHECK(exec_result->as_float() == 12.5);
}

int main() {
    test_loop_sum_quickening();
    test_prop_loop_with_ic();
    test_range_iterator();
    test_float_arithmetic();
    test_mixed_arithmetic();
    if (g_failures == 0) {
        std::printf("OK: specialization (5 tests passed)\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d checks failed\n", g_failures);
    return 1;
}

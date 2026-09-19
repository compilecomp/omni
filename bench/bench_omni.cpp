// bench/bench_omni.cpp
//
// Omni Tier 0 interpreter benchmarks, structured for direct comparison
// with bench/python_bench.py. Each workload builds a bytecode module by
// hand, executes it through the Interpreter, and reports wall-clock time.
//
// The bytecode encodings mirror the Python workloads exactly so that the
// comparison is apples-to-apples (same N, same operations, same result).
//
// Workloads:
//   1. loop_sum(N)         — sum 0..N-1 in a tight integer while-loop.
//   2. sum_of_squares(N)   — sum i*i for i in 0..N-1.
//   3. prop_access_loop(N) — create object, read .x in a loop N times.
//   4. range_loop_sum(N)   — sum 0..N-1 using range iterator.
//
// Encoding conventions (from handlers_semantic.cpp):
//   LOAD_CONST   rdst, const_idx8    — rdst = constants[const_idx8]
//   LOAD_LOCAL   rdst, src           — rdst = regs[src]
//   STORE_LOCAL  dst, rsrc           — regs[dst] = regs[rsrc]
//   ADD/SUB/MUL  rdst, rsrc2         — rdst = rdst OP rsrc2  (2-operand)
//   LT/GE        rdst, rsrc2         — rdst = bool(rdst OP rsrc2)
//   BRANCH       cond_reg, delta8    — if regs[cond] truthy: pc += signed delta8
//   JUMP         delta16             — pc += signed delta16 (operand_ab)
//   RETURN       rsrc                — regs[0] = regs[rsrc]; exit
//   MAKE_OBJECT  rdst, shape_id8     — rdst = new Object(shape)
//   SET_PROP     obj_reg, rsrc       — obj.prop_name = regs[rsrc]  (name in side table)
//   GET_PROP     rdst, obj_reg       — rdst = obj.prop_name        (name in side table)
//   GET_ITER     rdst, src           — rdst = range_iterator(0, regs[src])
//   NEXT         rdst, iter_reg      — rdst = next(iter); null if done
//   IS_NULL      rdst, src           — rdst = (regs[src] is null)

#include "core/bytecode/bytecode_module.hpp"
#include "core/bytecode/instruction.hpp"
#include "core/bytecode/opcode.hpp"
#include "core/common/result.hpp"
#include "core/common/symbol_table.hpp"
#include "core/common/types.hpp"
#include "core/interpreter/interpreter.hpp"
#include "core/object_model/object.hpp"
#include "core/object_model/shape_registry.hpp"
#include "core/object_model/tagged_value.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace omni;
using namespace omni::bytecode;
using namespace omni::common;
using namespace omni::interpreter;
using namespace omni::object_model;

// -------------------------------------------------------------------------
// Module builders
// -------------------------------------------------------------------------

// Each builder constructs a bytecode module that, when executed via
// Interpreter::execute(module_id, 0, {}), runs the named workload once
// and returns the result in register 0.
//
// Layout pattern for a while-loop with exit condition:
//   pc=0..k:   load constants into registers
//   loop:      (pc=k+1)
//     copy counter to temp reg
//     compare temp >= limit  (true = exit)
//     branch-if-true to RETURN
//     ... loop body ...
//     jump back to loop
//   RETURN

// Build a "loop_sum" module: sum 0..N-1.
//
//   pc=0: LOAD_CONST r0, 0     // acc = 0
//   pc=1: LOAD_CONST r1, 1     // counter = 0
//   pc=2: LOAD_CONST r2, 2     // limit = N
//   pc=3: LOAD_CONST r3, 3     // inc = 1
//   loop:
//   pc=4: LOAD_LOCAL r4, r1    // r4 = counter (copy)
//   pc=5: GE r4, r2            // r4 = (counter >= limit) -- exit cond
//   pc=6: BRANCH r4, +4        // if exit: jump to pc=10 (RETURN)
//   pc=7: ADD r0, r1           // acc += counter
//   pc=8: ADD r1, r3           // counter += 1
//   pc=9: JUMP -5              // back to pc=4
//   pc=10: RETURN r0
static std::unique_ptr<BytecodeModule> make_loop_sum_module(int64_t N) {
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::LoadConst, 0, 0});
    code.push_back(Instruction{Opcode::LoadConst, 1, 1});
    code.push_back(Instruction{Opcode::LoadConst, 2, 2});
    code.push_back(Instruction{Opcode::LoadConst, 3, 3});
    code.push_back(Instruction{Opcode::LoadLocal, 4, 1});
    code.push_back(Instruction{Opcode::Ge, 4, 2});
    code.push_back(Instruction{Opcode::Branch, 4, 4});
    code.push_back(Instruction{Opcode::Add, 0, 1});
    code.push_back(Instruction{Opcode::Add, 1, 3});
    code.push_back(Instruction{Opcode::Jump, 0xFF, 0xFB});  // -5
    code.push_back(Instruction{Opcode::Return, 0, 0});

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    mod->add_constant(ConstEntry{TaggedValue::make_int(0)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(0)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(N)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(1)});
    FunctionDesc fd{};
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

// Build a "sum_of_squares" module: sum i*i for i=0..N-1.
//
//   pc=0: LOAD_CONST r0, 0     // acc = 0
//   pc=1: LOAD_CONST r1, 1     // counter = 0
//   pc=2: LOAD_CONST r2, 2     // limit = N
//   pc=3: LOAD_CONST r3, 3     // inc = 1
//   loop:
//   pc=4: LOAD_LOCAL r4, r1    // r4 = counter
//   pc=5: GE r4, r2            // r4 = (counter >= limit)
//   pc=6: BRANCH r4, +5        // if exit: jump to pc=11 (RETURN)
//   pc=7: LOAD_LOCAL r5, r1    // r5 = counter
//   pc=8: MUL r5, r1           // r5 = counter * counter
//   pc=9: ADD r0, r5           // acc += r5
//   pc=10: ADD r1, r3          // counter += 1
//   pc=11: JUMP -8             // back to pc=4
//   pc=12: RETURN r0
//
// Wait, BRANCH +5 from pc=6 = 11, but 11 is JUMP. Fix: RETURN at pc=12, so
// BRANCH delta = 12 - 6 = +6. JUMP delta = 4 - 11 = -7.
static std::unique_ptr<BytecodeModule> make_sum_of_squares_module(int64_t N) {
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::LoadConst, 0, 0});  // pc=0
    code.push_back(Instruction{Opcode::LoadConst, 1, 1});  // pc=1
    code.push_back(Instruction{Opcode::LoadConst, 2, 2});  // pc=2
    code.push_back(Instruction{Opcode::LoadConst, 3, 3});  // pc=3
    code.push_back(Instruction{Opcode::LoadLocal, 4, 1});  // pc=4
    code.push_back(Instruction{Opcode::Ge, 4, 2});         // pc=5
    code.push_back(Instruction{Opcode::Branch, 4, 6});     // pc=6: +6 -> 12
    code.push_back(Instruction{Opcode::LoadLocal, 5, 1});  // pc=7
    code.push_back(Instruction{Opcode::Mul, 5, 1});        // pc=8
    code.push_back(Instruction{Opcode::Add, 0, 5});        // pc=9
    code.push_back(Instruction{Opcode::Add, 1, 3});        // pc=10
    code.push_back(Instruction{Opcode::Jump, 0xFF, 0xF9}); // pc=11: -7 -> 4
    code.push_back(Instruction{Opcode::Return, 0, 0});     // pc=12

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    mod->add_constant(ConstEntry{TaggedValue::make_int(0)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(0)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(N)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(1)});
    FunctionDesc fd{};
    fd.name = NULL_SYMBOL;
    fd.entry_pc = 0;
    fd.param_count = 0;
    fd.local_count = 6;
    fd.default_count = 0;
    fd.max_registers = 6;
    fd.is_async = false;
    fd.is_generator = false;
    mod->add_function(fd);
    return mod;
}

// Build a "prop_access_loop" module: create a {x, y} object, set x=10,
// then read x in a loop N times, accumulating into acc.
//
// Requires a pre-registered shape with 2 slots [x, y].
static std::unique_ptr<BytecodeModule> make_prop_access_loop_module(
    int64_t N, ShapeId shape_id) {
    //   pc=0: MAKE_OBJECT r0, shape_id  // obj = new {x, y}
    //   pc=1: LOAD_CONST r1, 0          // r1 = 10
    //   pc=2: SET_PROP r0, r1           // obj.x = 10
    //   pc=3: LOAD_CONST r1, 1          // r1 = 20
    //   pc=4: SET_PROP r0, r1           // obj.y = 20
    //   pc=5: LOAD_CONST r2, 2          // r2 = 0 (acc)
    //   pc=6: LOAD_CONST r3, 3          // r3 = 0 (counter)
    //   pc=7: LOAD_CONST r4, 4          // r4 = N (limit)
    //   pc=8: LOAD_CONST r5, 5          // r5 = 1 (inc)
    //   loop:
    //   pc=9: LOAD_LOCAL r6, r3         // r6 = counter
    //   pc=10: GE r6, r4                // r6 = (counter >= limit)
    //   pc=11: BRANCH r6, +5            // if exit: jump to pc=16 (RETURN)
    //   pc=12: GET_PROP r1, r0          // r1 = obj.x
    //   pc=13: ADD r2, r1               // r2 += r1
    //   pc=14: ADD r3, r5               // r3 += 1
    //   pc=15: JUMP -6                  // back to pc=9
    //   pc=16: RETURN r2
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::MakeObject, 0, static_cast<uint8_t>(shape_id)});
    code.push_back(Instruction{Opcode::LoadConst, 1, 0});
    code.push_back(Instruction{Opcode::SetProp, 0, 1});
    code.push_back(Instruction{Opcode::LoadConst, 1, 1});
    code.push_back(Instruction{Opcode::SetProp, 0, 1});
    code.push_back(Instruction{Opcode::LoadConst, 2, 2});
    code.push_back(Instruction{Opcode::LoadConst, 3, 3});
    code.push_back(Instruction{Opcode::LoadConst, 4, 4});
    code.push_back(Instruction{Opcode::LoadConst, 5, 5});
    code.push_back(Instruction{Opcode::LoadLocal, 6, 3});
    code.push_back(Instruction{Opcode::Ge, 6, 4});
    code.push_back(Instruction{Opcode::Branch, 6, 5});
    code.push_back(Instruction{Opcode::GetProp, 1, 0});
    code.push_back(Instruction{Opcode::Add, 2, 1});
    code.push_back(Instruction{Opcode::Add, 3, 5});
    code.push_back(Instruction{Opcode::Jump, 0xFF, 0xFA});  // -6
    code.push_back(Instruction{Opcode::Return, 2, 0});

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    // Property name side table.
    auto x_sym = SymbolTable::instance().intern("x");
    auto y_sym = SymbolTable::instance().intern("y");
    mod->set_prop_name(2, *x_sym);   // pc=2: SET_PROP x
    mod->set_prop_name(4, *y_sym);   // pc=4: SET_PROP y
    mod->set_prop_name(12, *x_sym);  // pc=12: GET_PROP x
    // Constants.
    mod->add_constant(ConstEntry{TaggedValue::make_int(10)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(20)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(0)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(0)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(N)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(1)});
    FunctionDesc fd{};
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

// Build a "range_loop_sum" module: sum 0..N-1 using range iterator.
//
//   pc=0: LOAD_CONST r0, 0    // r0 = N (range end)
//   pc=1: GET_ITER r1, r0     // r1 = iter([0, N))
//   pc=2: LOAD_CONST r2, 1    // r2 = 0 (acc)
//   loop:
//   pc=3: NEXT r3, r1         // r3 = next(iter); null if done
//   pc=4: IS_NULL r4, r3      // r4 = (r3 is null)
//   pc=5: BRANCH r4, +3       // if r4 (done): jump to pc=8 (RETURN)
//   pc=6: ADD r2, r3          // r2 += r3
//   pc=7: JUMP -4             // back to pc=3
//   pc=8: RETURN r2
static std::unique_ptr<BytecodeModule> make_range_loop_sum_module(int64_t N) {
    std::vector<Instruction> code;
    code.push_back(Instruction{Opcode::LoadConst, 0, 0});
    code.push_back(Instruction{Opcode::GetIter, 1, 0});
    code.push_back(Instruction{Opcode::LoadConst, 2, 1});
    code.push_back(Instruction{Opcode::Next, 3, 1});
    code.push_back(Instruction{Opcode::IsNull, 4, 3});
    code.push_back(Instruction{Opcode::Branch, 4, 3});  // +3 -> 8
    code.push_back(Instruction{Opcode::Add, 2, 3});
    code.push_back(Instruction{Opcode::Jump, 0xFF, 0xFC});  // -4 -> 3
    code.push_back(Instruction{Opcode::Return, 2, 0});

    auto mod = std::make_unique<BytecodeModule>(1, std::move(code));
    mod->add_constant(ConstEntry{TaggedValue::make_int(N)});
    mod->add_constant(ConstEntry{TaggedValue::make_int(0)});
    FunctionDesc fd{};
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

// -------------------------------------------------------------------------
// Benchmark runner
// -------------------------------------------------------------------------

struct BenchResult {
    double seconds;
    TaggedValue value;
};

// Run a benchmark with a fresh module each repeat (so quickening starts
// from scratch each time). The builder is called `repeat` times.
template <typename Builder>
static BenchResult run_bench_fresh(const char* name, Builder builder, int repeat = 3) {
    double best = 1e30;
    TaggedValue result{};
    for (int i = 0; i < repeat; ++i) {
        Interpreter interp;
        auto load_result = interp.load_module(builder());
        if (!load_result.has_value()) {
            std::fprintf(stderr, "%s: load_module failed\n", name);
            std::exit(1);
        }
        const uint32_t module_id = *load_result;
        std::span<const TaggedValue> args{};
        auto t0 = std::chrono::steady_clock::now();
        auto exec_result = interp.execute(module_id, 0, args);
        auto t1 = std::chrono::steady_clock::now();
        if (!exec_result.has_value()) {
            std::fprintf(stderr, "%s: execute failed: category=%d\n",
                         name, static_cast<int>(exec_result.error().category));
            std::exit(1);
        }
        result = *exec_result;
        double dt = std::chrono::duration<double>(t1 - t0).count();
        if (dt < best) best = dt;
    }
    return {best, result};
}

static std::string format_result(const TaggedValue& v) {
    if (v.is_int()) return std::to_string(v.as_int());
    if (v.is_float()) return std::to_string(v.as_float());
    if (v.is_bool()) return v.as_bool() ? "true" : "false";
    if (v.is_null()) return "null";
    return "<object>";
}

int main() {
    // Pre-register the {x, y} shape for prop_access_loop.
    auto x_sym = SymbolTable::instance().intern("x");
    auto y_sym = SymbolTable::instance().intern("y");
    OmniShape* shape = ShapeRegistry::instance().intern(
        2, std::vector<SymbolId>{*x_sym, *y_sym});
    const ShapeId shape_id = shape->shape_id();

    // N values must match bench/python_bench.py exactly for a fair comparison.
    constexpr int64_t N_LOOP = 10'000'000;
    constexpr int64_t N_SQUARES = 2'000'000;
    constexpr int64_t N_PROP = 5'000'000;
    constexpr int64_t N_RANGE = 10'000'000;

    std::printf("%-22s %12s %20s %10s\n", "workload", "N", "result", "time_s");
    std::printf("%s\n", std::string(70, '-').c_str());

    {
        auto r = run_bench_fresh("loop_sum",
            [=]() { return make_loop_sum_module(N_LOOP); });
        std::printf("%-22s %12lld %20s %10.6f\n",
                    "loop_sum", (long long)N_LOOP,
                    format_result(r.value).c_str(), r.seconds);
    }
    {
        auto r = run_bench_fresh("sum_of_squares",
            [=]() { return make_sum_of_squares_module(N_SQUARES); });
        std::printf("%-22s %12lld %20s %10.6f\n",
                    "sum_of_squares", (long long)N_SQUARES,
                    format_result(r.value).c_str(), r.seconds);
    }
    {
        auto r = run_bench_fresh("prop_access_loop",
            [=]() { return make_prop_access_loop_module(N_PROP, shape_id); });
        std::printf("%-22s %12lld %20s %10.6f\n",
                    "prop_access_loop", (long long)N_PROP,
                    format_result(r.value).c_str(), r.seconds);
    }
    {
        auto r = run_bench_fresh("range_loop_sum",
            [=]() { return make_range_loop_sum_module(N_RANGE); });
        std::printf("%-22s %12lld %20s %10.6f\n",
                    "range_loop_sum", (long long)N_RANGE,
                    format_result(r.value).c_str(), r.seconds);
    }
    return 0;
}

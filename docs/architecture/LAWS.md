# Omni Compiler Laws & Architecture Specification

**Status:** Stable  
**Owner:** Omni Systems Dev Team  
**Last Updated:** 2026-09-19  
**Target:** High-performance Omni engine via Omni Bytecode and the Omni Semantic Profile  
**Related Sections:** `ir_spec.md`, `effect_system.md`, `abi.md`, `bytecode_spec.md`, `omni_semantic_profile.md`, `runtime_contracts.md`  

This document is the authoritative, uncompressed transcription of the laws that govern the Omni compiler and runtime. Every commit to the `compiler/`, `runtime/`, `tools/`, and `tests/` trees must comply. CI verifies them. There are no exceptions.

---

## Part 0: Omni Semantic Contract

Omni is a **high-performance dynamic language engine** designed to execute Omni-compliant code through a tiered compiler and runtime infrastructure built on a Sea of Nodes intermediate representation. 

Omni is not a generic virtual machine. It is purpose-built for Omni's philosophy of Pythonic readability, extreme runtime flexibility, shape-shifting objects, and free-threaded concurrency. However, the compiler and runtime must never hardcode Omni semantic assumptions directly into core optimization passes, IR invariants, backend lowering, or runtime fast paths unless those semantics are explicitly exposed through the Omni Semantic Profile.

All Omni code must flow through the engine via the following contract:

1. **Omni Bytecode**  
   A typed, register-based, effect-carrying bytecode format that can be validated, profiled, interpreted, baseline-compiled via Copy-and-Patch stencils, and optimized through the full Sea of Nodes pipeline.

2. **Omni Semantic Profile**  
   A versioned description of the Omni semantics, object model, memory model, exception model, dynamic features (shape-shifting, trait injection, universal callability), native interop rules, and observability requirements that Omni must preserve.

3. **Omni Semantic Oracle**  
   The Omni Core Specification and the OmniTest suite against which the engine can be compared for semantic fidelity.

4. **Runtime Capability Declarations**  
   Explicit declarations of which dynamic, reflective, numeric, concurrency, debugging, and memory-management features the Omni environment supports.

The Omni core compiler must remain semantically disciplined. All Omni-specific semantics must be expressed through:
- The Omni Semantic Profile
- capability flags
- effect classes
- dependency records
- lowering hooks
- runtime helpers
- verifier constraints
- test matrices

If an Omni feature cannot be represented safely in Omni IR, the frontend must lower that feature conservatively or mark it opaque. The runtime must then execute it through a lower tier or through approved runtime helpers.

### Omni Semantic Profile Requirements

The Omni Semantic Profile must define, at minimum:
- Omni Core Specification version and dialect/version hash
- bytecode format version
- object header layout and object shape/trait version model
- prototype and trait chain versioning rules
- module and import semantics
- global, module, and builtin namespace semantics
- exception model and Explainable Error Engine contracts
- stack trace model and frame introspection capabilities
- numeric semantics including NaN, negative zero, BigInt, Number, and implicit morphing rules
- operator overload or dispatch-hook semantics including `toPrimitive`, `valueOf`, `toString`, and universal callability/iterability hooks
- dynamic member/property access and shape-shifting semantics
- reflection and introspection capabilities
- debugging, profiling, tracing, and Interactive REPL Recovery hooks
- weak-reference model including WeakRef and FinalizationRegistry
- finalization model and memory management model (Concurrent Generational GC)
- identity semantics and native/FFI model
- concurrency and threading model (No GIL, M:N Threading, `sync` blocks)
- suspension model including generators, async functions, and async generators

### Omni Capability Flags

The Omni Semantic Profile must declare capability flags such as:
- `HasDynamicCodeEvaluation`
- `HasDynamicCompilation`
- `HasDynamicImports`
- `HasDynamicMemberMutation`
- `HasShapeShifting`
- `HasTraitInjection`
- `HasUniversalCallability`
- `HasUniversalIterability`
- `HasImplicitMorphing`
- `HasMonkeyPatching`
- `HasFrameIntrospection`
- `HasWeakReferences`
- `HasFinalizers`
- `HasIdentityAddressObservation`
- `HasReferenceCountIntrospection`
- `HasMovingGC`
- `HasResumableFunctions`
- `HasGenerators`
- `HasCoroutines`
- `HasAsyncFunctions`
- `HasFibers`
- `HasContinuations`
- `HasOperatorOverloading`
- `HasDynamicDispatch`
- `HasModuleReloading`
- `HasFFI`
- `HasNoGIL`
- `HasFreeThreading`
- `HasSyncBlocks`
- `HasTracingHooks`
- `HasProfilerHooks`
- `HasDebuggerHooks`
- `HasInteractiveREPLRecovery`
- `HasExplainableErrors`
- `HasProxyObjects`
- `HasSymbolSpecies`
- `HasSymbolToPrimitive`
- `HasSymbolIterator`
- `HasSymbolHasInstance`
- `HasPrivateFields`
- `HasPrivateMethods`
- `HasTemporalDeadZone`

If a capability is absent, the compiler must not assume the feature exists. If a capability is present, the compiler must preserve its observable semantics.

---

## Part I: Architectural Overview

The Omni execution model consists of three distinct, seamlessly integrated execution tiers. Transitions between tiers are governed by profile data and static guarantees, never by arbitrary timeouts. *(Note: Tier 3 / AOT is intentionally omitted per architectural mandate; Omni relies entirely on adaptive JIT compilation).*

1. **Tier 0: Direct-Threaded Register Interpreter**  
   The baseline execution engine. Uses computed gotos and a register-based bytecode representation, not a stack-based representation, to minimize dispatch overhead. Uses 24-bit fixed-width instructions with 256 virtual registers per frame. This tier provides fast startup, minimal memory footprint, full Omni semantic fidelity, and the ultimate fallback for all Omni code. Collects initial Inline Cache shape feedback, type histograms, branch probabilities, and call target profiles.

2. **Tier 1: Baseline JIT via Copy-and-Patch Stencils**  
   Triggered by low-level heat, such as loop backedges or function invocation counts. Compiles in linear time using pre-compiled machine code templates called Super Stencils. Performs basic type specialization, copy propagation, simple inlining, and lightweight guard insertion. No intermediate representation is constructed. Compilation time is strictly bounded to prevent mutator stalls.

3. **Tier 2: Optimizing JIT via Sea of Nodes**  
   Triggered by sustained heat and rich PGO data. Constructs the full Omni Sea of Nodes IR. Employs the full Omni optimization pipeline including all passes. Performs aggressive speculative optimizations such as partial escape analysis, scalar replacement, guard-based devirtualization, SLP vectorization, guard hoisting, guard elimination, range analysis, and speculative effect reordering where legal. Requires full FrameState and deoptimization infrastructure.

---

## Part II: The Unified Pipeline & Speculation Laws

**Rule 1 — One Pipeline, Multiple Inputs**  
The IR, pass interfaces, verifier constraints, and correctness constraints are unified across Tier 1 and Tier 2. Tiers differ only by budgets, enabled speculation policies, available proofs, Omni capabilities, and telemetry requirements.  
- Tier 1 Input: Static IR + Minimal Heuristics + Omni Semantic Profile.  
- Tier 2 Input: Static IR + PGO Data + Omni Semantic Profile.  
There is no "JIT-only" pass list. If a pass exists, it must handle all modes via a unified interface, but the pipeline may apply tier-specific filters.

**Rule 2 — PGO is a Force Multiplier, Not New Logic**  
PGO data does not change what the compiler does; it changes how aggressively it does it. Example: a speculative effect-reordering pass may be conceptually available in all tiers. In Tier 2, it accepts high-confidence PGO and inserts a guard. In Tier 1, it is skipped due to budget. PGO must never introduce new semantic behavior. It only enables or disables optimizations that are already semantically legal under guard.

**Rule 3 — Every PGO-Driven Decision Requires a Guard**  
If a pass makes a decision based on PGO, it must emit a validated guard mechanism: runtime check, shape/version guard, patchpoint, trap, dependency invalidation, hardware check, or another approved validation mechanism. Examples include: pointers never alias, object property lookup is monomorphic, call site has one dominant receiver type, integer value remains in range, object shape is stable, branch is almost always taken. Guard success executes the optimized path. Guard failure triggers deoptimization or fallback.

**Rule 4 — Deoptimization Must Reconstruct Tier 0 State**  
When a JIT guard fails, the runtime must deoptimize to the exact same state the Tier 0 Direct-Threaded Register Interpreter would have been in at that instruction position. This includes: restoring register values, re-materializing Omni frames, restoring local variables, restoring closure cells, restoring exception state, restoring tracing/monitoring state, restoring Omni-visible memory effects, restoring reference-management state where observable, and rolling back or compensating speculatively reordered effects only where legal. Speculative execution must not perform irreversible Omni-visible side effects (like trait injection) before the last guard protecting that speculation.

**Rule 5 — FrameState is Mandatory for All Guards**  
Every node that introduces a speculative assumption must have a FrameState attachment. This snapshot allows the deoptimizer to rebuild the Omni execution world if the speculation fails. FrameState must include enough information to reconstruct the Omni-visible state required by the Omni Semantic Profile, including but not limited to: bytecode offset, source position, local variables, closure cells, free variables, global/module namespace version, builtin/runtime library version, exception state, tracing/profiling state, suspension state, object shape/trait versions, and reference-management state.

---

## Part III: Compilation Pipeline & Memory Laws

**Rule 6 — NO EXCEPTIONS ON THE HOT PATH**  
Native C++ exceptions are forbidden on compiler/runtime hot paths. The JIT compiler and Runtime Deoptimization engine MUST be compiled with `-fno-exceptions`. Zero throw statements are allowed in any code path executed during compilation or runtime specialization. All fallible operations MUST use `std::expected<T, Diagnostic>` or `Result<T, Error>`. Omni exceptions remain first-class runtime values and must be modeled explicitly in the Sea of Nodes IR. If a JIT compilation fails, it returns an Error variant, causing the system to silently fall back to Tier 0 or Tier 1. No stack unwinding. No catch blocks. No overhead.

**Rule 7 — Zero-Allocation Hot Path**  
JIT compilers must use `std::pmr::monotonic_buffer_resource` for IR allocation. Bulk-free after compilation. No malloc or free in the compiler hot path. Hot path means: compiler pass execution, guard execution, inline-cache fast paths, allocation fast paths, and deopt entry trampolines. Deopt materialization may allocate only through a controlled runtime path with explicit budgets.

**Rule 8 — No RTTI**  
Both pipelines are compiled with `-fno-rtti`. Use `enum class NodeKind` for type switching. RTTI is forbidden in the IR and backend to ensure maximum devirtualization and cache locality.

**Rule 9 — No `std::shared_ptr` or `std::function` in Hot IR Code**  
They allocate and incur atomic overhead. Use raw pointers plus stable NodeIds inside passes.

**Rule 10 — Every Pass Must Be Idempotent and Monotonic**  
Running the same pass twice must produce the identical IR. A pass either reduces node count or moves the IR closer to a normal form. If a pass can grow the IR, such as Loop Unrolling or SLP Vectorization, it must run inside a guarded fixpoint with a strict budget.

**Rule 11 — Mutator Threads Never Block on JIT**  
If a function becomes hot and triggers a JIT compilation, the mutator thread continues executing the current tier. The JIT runs asynchronously on a background compiler thread. Once ready, a safe-point patch swaps the function pointer. Function pointer publication must be atomic and safe against concurrent execution. Old code must remain valid until quiescence.

**Rule 12 — Thread-Local Allocation for Mutators**  
Mutator threads use thread-local bump pointers, lexical regions, or equivalent thread-local allocation mechanisms for their own runtime allocations where the Omni memory model permits. Global synchronization happens only at explicit yield points, safepoints, or memory-model boundaries.

**Rule 13 — Compiler Threads Never Block on Mutator State**  
The compiler works on a frozen snapshot of the IR and PGO data. Mutator updates after the snapshot are picked up by the next compilation.

**Rule 14 — Epoch-Based Reclamation**  
Old JIT code and IR nodes are reclaimed using epoch-based garbage collection. When the optimizer replaces a Node, the old node is tagged with an epoch. Once all threads advance past that epoch, the memory is bulk-freed. This avoids both locks and use-after-free. Generated code must not be reclaimed until no thread can be executing it or depend on its deopt metadata.

---

## Part IV: The Numbered Rules

### Data Structures & IR Design

**Rule 15 — Index-Based Graph**  
Never use raw pointers (Node*) for edges in the Sea of Nodes. All node references must use a 32-bit integer index: `using NodeId = uint32_t;`. This cuts memory footprint in half, doubles L1/L2 cache capacity, and makes the IR trivially serializable and immune to pointer invalidation during arena reallocation.

**Rule 16 — Interned Symbols**  
Never pass, compare, or store `std::string` or `std::string_view` in the IR or passes. All identifiers, variable names, function names, type names, field names, module names, and Omni property names must be interned into a global SymbolTable at the frontend. The IR must only use a SymbolId which is a uint32_t.

**Rule 17 — Cache-Friendly Hash Maps**  
`std::unordered_map` and `std::map` are forbidden in the compiler hot path. For Global Value Numbering, Hash-Consing, and any pass requiring a hash table, you must use a cache-friendly, open-addressing hash map such as `tsl::robin_map` or `std::flat_hash_map`.

**Rule 18 — Sparse Sets and BitVectors for Pass Data**  
Ban `std::set`, `std::unordered_set`, and `std::vector<bool>` for dataflow analysis. Passes tracking sets of NodeIds, such as liveness, dominators, and visited sets, must use Sparse Sets for small dense sets or BitVectors for large sparse sets.

**Rule 19 — Small Buffer Optimization for Variable-Length Data**  
Ban `std::vector` for data that usually has 1 to 4 elements. For use-def chains, instruction operands, and basic block predecessors and successors, use a `SmallVector<T, N>`, where N is typically 2, 3, or 4.

**Rule 20 — Structure of Arrays for Bulk Pass Processing**  
When a pass needs to process a specific field of millions of nodes, do not iterate over the Node structs. Extract that attribute into contiguous `std::pmr::vector` storage in Structure-of-Arrays layout to allow perfect CPU prefetching and SIMD vectorization on the compiler's own passes.

### Performance & Hardware

**Rule 21 — Exploit C++26 Compiler Hints**  
Use `[[likely]]` and `[[unlikely]]` on all PGO-driven branches and deoptimization traps. Use C++26 `[[assume(condition)]]` to tell the compiler about invariants, for example: `[[assume(node_id < graph.size())]];`. This eliminates bounds checks in internal compiler data structures where safe.

**Rule 22 — Zero-Cost Error Propagation**  
Do not use verbose `if (err)` chains that ruin branch prediction. Use `std::expected<T, Error>` and monadic operations (and_then, transform) or a custom TRY() macro that compiles down to a single branch, keeping the hot path instruction cache pristine.

**Rule 23 — No Hard-Coded Constants in Optimization Logic**  
Magic numbers are forbidden. Every threshold, budget, limit, and heuristic constant used in any optimization pass MUST be defined as a named, documented constexpr constant or configuration parameter. CI static analysis fails if numeric literals greater than 2 appear in pass logic without a named constant reference.

**Rule 24 — No Target-Specific Hacks in Generic Passes**  
Mid-level and research passes, such as GVN, LICM, SLP, and alias analysis, MUST NOT contain target-specific conditionals such as `#ifdef X86`. All target knowledge must be abstracted behind the Target interface and queried via cost models or capability flags.

**Rule 25 — No Heuristics Without Empirical Validation**  
Every heuristic MUST be backed by benchmark data showing measurable improvement, a mechanism to override or tune it, and documentation explaining why the value was chosen.

**Rule 26 — No Silent Fallbacks Without Telemetry**  
When the JIT falls back to a lower tier, when a speculative guard fails, or when regalloc spills excessively, the event MUST be recorded in telemetry and profile data. Silent fallbacks hide performance problems.

**Rule 27 — No Assumption of Stable Hardware**  
No pass may assume fixed cache line sizes, SIMD widths, or memory latency ratios. All hardware parameters MUST be queried at runtime for JIT via the Target interface.

**Rule 28 — No Optimization Without Measurable Win**  
Every optimization pass added to the pipeline MUST demonstrate a measurable geometric mean improvement across the benchmark suite, OR enable a correctness or safety property that cannot be achieved otherwise. Underperforming passes are removed.

### Correctness & Omni Semantics

**Rule 29 — No FFI Optimization Without ABI Proof**  
FFI and native interop optimizations must prove: calling convention correctness, stack alignment, register clobbering, exception propagation, memory ownership transfer, Omni runtime state preservation, and reference-management transfer where applicable.

**Rule 30 — No Vectorization Without Dependence Proof**  
Vectorization, whether SLP or loop-based, must prove no aliasing, or use versioned checks, bounds safety, alignment, and correct scalar fallback.

**Rule 31 — No Persistent State Without Versioning**  
Profile caches and code caches must be versioned. A change in the IR format, Omni bytecode version, Omni Semantic Profile version, pass order, target ABI, or runtime configuration invalidates the cache.

**Rule 32 — All Orthogonal Boolean State Must Be Bitmasked**  
Any set of independent boolean properties on a hot-path data structure, such as NodeFlags or EffectTags, must be represented as a bitmask with type-safe `Flags<E>` wrappers. Raw integers are forbidden for flag-like state.

**Rule 33 — No Implicit Conversions or Coercions in IR**  
The Omni IR MUST NOT perform implicit type conversions, integer promotions, or pointer coercions. All conversions must be explicit nodes. The frontend lowering pass inserts these explicitly. Examples: IntToPtr, PtrToInt, SExt, ZExt, Trunc, BitCast, Box, Unbox, TaggedToFloat64, Float64ToTagged, TaggedToSmi, SmiToTagged, MorphToCallable, MorphToIterable.

### Testing & Verification

**Rule 34 — Five Regression Tests Per Bug Fix**  
Every bug fix must include at least 5 regression tests: 1. Minimal reproducer. 2. Variant trigger (different code pattern, same root cause). 3. Boundary or negative test (ensuring fix does not over-correct). 4. Integration or contextual test (bug in realistic surrounding code). 5. Deopt and State Reconstruction test (verifying deopt to Tier 0 produces exact same state). Enforcement: CI fails if a PR labeled bugfix has fewer than 5 new test cases.

**Rule 35 — Golden Tests for Every Pass**  
Every optimization pass must have at least 10 golden IR tests. Checked-in input and expected IR file pairs must be maintained. Tests must run in both Static Mode and Profile Mode.

**Rule 36 — Differential Testing is Mandatory in CI**  
Tier 0 Interpreter versus Tier 1 JIT versus Tier 2 JIT comparisons run on every PR. Tier outputs must be observationally equivalent according to the Omni Semantic Oracle (OmniTest). Any permitted differences must be explicitly listed, versioned, and tested. Divergence blocks merge.

**Rule 37 — Deopt Paths Must Be Fuzzed Weekly**  
Scheduled CI job. Results triaged within 24 hours. Untriaged deopt fuzz failures block releases.

**Rule 38 — Replay Logs Retained for All CI Failures**  
Failed test runs automatically save full compile replay artifacts: IR snapshot, PGO profile, compiler options, RNG seed, target description, Omni Semantic Profile version, failure context. Debugging starts from replay, not reproduction.

**Rule 39 — Performance Regressions Require Explicit Waiver**  
If a benchmark regresses beyond the approved threshold, the PR must include root cause analysis, justification, a tracking issue, and approval. No silent performance degradation.

**Rule 40 — Graph Verifier Runs in Debug Builds After Every Pass**  
The verifier checks: no dangling NodeIds, effect chain continuity, control dominance, use-def consistency, FrameState attached to every PGO-driven guard, dependency metadata completeness, Omni effect legality.

**Rule 41 — Test Names Encode the Bug or Feature They Cover**  
Bad: `test_pea_3`. Good: `pea_non_escaping_region_object_with_deopt_materializes_correctly`. Searchable, self-documenting.

### Speculation & Guarding

**Rule 42 — No Assumption Without Invalidation**  
Every PGO-driven assumption must have: a registry entry (Watchdog), an invalidation path (Trip), and a fallback to static proof or lower-tier execution.

**Rule 43 — No Specialization Without Fallback**  
Every specialized clone (e.g., bounds-check-eliminated array loop) must have: a generic fallback, a deopt path, and a budget limit.

**Rule 44 — No Profile Data Without Confidence**  
Profile data must include: sample count, stability, age, decay, variance, deopt correlation. Low-confidence data must not trigger aggressive speculation.

**Rule 45 — No Aggressive Pass Without a Cost Model**  
Inlining, cloning, unrolling, SLP vectorization, and PEA materialization must all use a strict cost model based on target hardware latencies and Omni object overhead.

---

## Part V: Code Quality & Developer Velocity Laws

**Rule 46 — Local Pre-Commit Checks Must Complete in Under 2 Seconds**  
Strictness must not impede velocity. The local pre-commit hook, including formatting, basic linting, and copyright headers, must execute in under 2 seconds. Heavy checks are deferred to asynchronous CI.

**Rule 47 — Actionable Compiler Diagnostics**  
The compiler must never output opaque errors. All Diagnostic objects must include: the exact source location, a clear human-readable message, the expected vs actual state, and a suggested fix (e.g., "Add missing trait", "Wrap in `sync` block"). This powers the Explainable Error Engine and saves developers hours of debugging.

**Rule 48 — `[[nodiscard]]` on All Result Types**  
All functions returning `std::expected`, Result, or Error must be marked `[[nodiscard]]`. Ignoring an error is a compilation failure. This forces developers to handle edge cases explicitly without requiring verbose, performance-killing if chains.

**Rule 49 — No `#define` Macros for Logic**  
C-style macros for control flow or logic are forbidden. Use constexpr functions, inline functions, or templates. Macros are exempt only for header guards and trivial token pasting. This ensures the debugger can step through the code and the compiler can inline and optimize it properly.

**Rule 50 — Fast Incremental Builds via Modular CMake**  
The build system must be structured to allow sub-second incremental builds for single-file changes. Heavy dependencies must be isolated. Developers must not wait minutes to test a single IR pass modification.

**Rule 51 — Automated Refactoring Tools Over Manual Edits**  
When a structural change is required, a scripted refactoring tool must be provided and run as part of the PR. Manual, error-prone find-and-replace across many files is forbidden.

**Rule 52 — Self-Contained, Reproducible Test Cases**  
Every test must be fully self-contained. It must not rely on external network calls, specific local directory structures, or non-deterministic system state. Tests must run identically on any developer machine or CI runner.

---

## Part VI: Anti-Slop & Robustness Laws

**Rule 53 — No "Small Bug" or "Minor Edge Case" Rationalization**  
The phrases "small bug," "minor edge case," "rarely happens," "only affects cold paths," and "good enough for now" are banned. In a systems compiler, small bugs cause silent data corruption or catastrophic performance cliffs. All bugs must be triaged with explicit severity.

**Rule 54 — No Workarounds for Compiler or Runtime Bugs**  
Adding code to work around a bug in the compiler, runtime, or standard library is forbidden. The underlying defect MUST be fixed. Temporary mitigations require a tracking issue, a removal deadline of two weeks or less, and explicit approval from the tech lead.

**Rule 55 — No Implicit Knowledge Transfer**  
All design decisions, trade-offs, historical context, and operational knowledge MUST be captured in persistent, searchable documentation. Oral tradition and chat messages are not valid knowledge stores.

**Rule 56 — No Premature Simplification**  
Do not simplify, abstract, or generalize code until the full problem space is understood and at least two concrete use cases exist. Premature simplification creates leaky abstractions that fail under real-world Omni conditions.

**Rule 57 — No Copy-Paste Code or Structural Duplication**  
If two code blocks share structure, extract a helper, template, or data-driven approach. ABI definitions, register lists, and pass boilerplate must use generators, constexpr helpers, or declarative tables.

**Rule 58 — No Silent Fallbacks or Default Returns**  
Switch statements on closed enums must be exhaustive. Non-exhaustive switches require `[[assume(false)]]` plus `OMNI_UNREACHABLE()`. Functions must not return arbitrary default values when input is invalid.

**Rule 59 — No Lazy Data Structures or Algorithms**  
Use the right tool, not the convenient tool. Linear search is forbidden where O(1) lookup is feasible. String comparison is forbidden where symbol IDs suffice.

**Rule 60 — No Untested or Unverified Code Paths**  
Every branch, edge case, and error path must have explicit test coverage. "It compiles" is not verification. Only automated, reproducible tests count.

**Rule 61 — No Performance-Agnostic Implementation**  
Hot-path code must avoid allocations, exceptions, RTTI, virtual dispatch, and cache-unfriendly patterns. Performance is a feature. Ignoring it in implementation guarantees degradation.

**Rule 62 — No Deletion-by-Avoidance**  
Deleting, disabling, commenting out, or stubbing functionality because it is "too hard" or "too complex" is strictly forbidden. When encountering difficult problems: Decompose, Research, Prototype, Document, and Escalate.

**Rule 63 — No Fragile Implementations**  
All implementations MUST be resilient to malformed input, concurrent access, resource exhaustion, and platform or hardware variation. Fragile patterns, such as implicit ordering dependencies, global mutable state, and unchecked pointer arithmetic, are forbidden.

**Rule 64 — No Documentation Debt**  
Every public API, internal helper, IR node, pass, configuration knob, and non-obvious algorithm MUST have documentation at the point of definition covering: Purpose, Invariants, Rationale, Edge Cases, Cross-References. Stale documentation is treated as a bug with the same severity as stale code.

**Rule 65 — No Easy Fixes — Only Correctness-Preserving Performance Fixes**  
When fixing a bug, you must implement the fix that simultaneously preserves performance and correctness. Easy fixes that sacrifice either property are forbidden unless explicitly documented as temporary mitigations with tracking issues and removal deadlines.

**Rule 66 — Slop Detection Checklist**  
Every PR reviewer must verify: No unnamed numeric constants in logic; No duplicated code blocks; No silent fallbacks or unsafe default returns; No prohibited containers in hot paths; All invariants documented and validated; No premature abstractions without at least two consumers; No untracked workarounds or HACK comments; No target-specific logic outside the backend directory; All new code paths have test coverage; Hot-path changes justified with profiling; Every new guard has a FrameState attachment; Every speculative node carries metadata; Every GC reference across a safepoint has a stack map; Every deopt point is reachable; No raw object pointers held across safepoints without GC map entries; No getenv() or mutex-locking calls in dispatch loops; No atomic RMW in per-instruction hot paths unless justified; Every memory store of a reference executes the correct write barrier; W^X is maintained; Code publication is atomic with release semantics. Failure on any item blocks merge. No exceptions. No "small slop."

---

## Part VII: Omni Semantic Fidelity Laws

**Rule 67 — The Omni Core Specification Is the Semantic Oracle**  
All executable behavior must match the supported Omni reference semantics unless a divergence is explicitly documented, justified, versioned, and approved. Observable behavior includes: program output, exceptions and stack traces, side effects, object mutation, weak-reference behavior, finalization behavior, frame introspection, debugging and monitoring events, object identity semantics, module semantics, and documented builtin behavior. Any unapproved divergence is a correctness bug.

**Rule 68 — Observable Effects Must Not Be Reordered, Duplicated, or Deleted**  
No optimization may delete, duplicate, hoist, sink, merge, or reorder Omni-visible effects unless the effect system proves semantic equivalence. Omni-visible effects include: field and member reads/writes, global/module/builtin reads/writes, operator overload dispatch, dynamic member access hooks, shape-shifting/trait injection side effects, allocation side effects where visible, exceptions raised, finalizers, tracing/profiling events, I/O effects, changes to object identity, prototype chain mutations, and Symbol method invocations.

**Rule 69 — Only Provably Pure Expressions May Be Constant-Folded**  
Constant folding may only apply to expressions whose result is independent of: runtime state, object identity, hash randomization, environment variables, time, randomness, locale, filesystem state, import state, global/module mutation, builtin mutation, object layout and shape, GC state, reference-management state, thread scheduling, and Omni dynamic state including trait chains. No call with possible side effects may be constant-folded.

**Rule 70 — Dynamic Omni Features Are First-Class Correctness Requirements**  
The JIT must correctly handle or safely fall back for all dynamic features declared by the Omni Semantic Profile. Examples include: shape-shifting, trait injection, universal callability, universal iterability, implicit morphing, dynamic property get/set/delete, Proxy traps, prototype chain mutation, monkey patching of builtins, tracing/profiling hooks, WeakRef, FinalizationRegistry, and suspension constructs. If a dynamic feature cannot be optimized safely, the system must deoptimize. It must never silently produce wrong introspection or semantics.

**Rule 71 — Specialization Requires Versioned, Invalidatable Dependencies**  
Every specialization assumption must record a dependency on a versioned entity. Examples include: object shape and hidden class version, trait chain version, module version, global dictionary version, builtin/runtime library version, function/code object version, property descriptor version, elements kind version, signature version, import state version, bytecode version, Omni Semantic Profile version, profile version. If any dependency changes, all dependent compiled code must be invalidated or guarded.

**Rule 72 — Omni Numeric Semantics Must Be Preserved Exactly**  
Numeric specializations must preserve Omni numeric semantics exactly. This includes: IEEE 754 double-precision floating point for all Number operations, NaN propagation rules, negative zero semantics, BigInt arbitrary precision integer semantics, TypeError when mixing Number and BigInt in arithmetic, integer overflow behavior for bitwise operations, division by zero producing Infinity or NaN, modulo with negative operands, and implicit morphing rules for numeric coercion. Fast-math-style optimizations are forbidden unless explicitly scoped, proven safe, and disabled by default.

**Rule 73 — Escape Analysis Must Not Eliminate Observable Objects**  
Objects may be scalarized or eliminated only if they cannot be observed by: identity operators (===), WeakRef/WeakMap/WeakSet, FinalizationRegistry, GC introspection, exception stack traces, frame locals visible to debugger, profiling/debugging hooks, user code escaping the function, dynamic introspection via shape/trait reflection, native/FFI calls, monitoring events, or Proxy traps. If any escape path exists, the object must be materialized.

**Rule 74 — Omni Exceptions Are Control-Flow Values, Not Native Exceptions**  
Omni exceptions must be represented as runtime values and control-flow edges in the Sea of Nodes IR. Native C++ exceptions must not be used to implement Omni exception propagation. The JIT must preserve: exception type, exception value, stack trace, exception chaining via cause, exception context, source positions, frame association, finally-block semantics, and scope-exit cleanup semantics.

**Rule 75 — Frames Must Be Reconstructible on Demand**  
If the JIT inlines, merges, elides, or optimizes frames, it must be able to materialize a semantically correct Omni frame when required by: stack traces, exceptions, debuggers, frame introspection, reflection APIs, locals access, tracing/profiling, deoptimization, monitoring hooks, user introspection, or the Interactive REPL Recovery system. FrameState must be sufficient to reconstruct all required state.

**Rule 76 — Generators, Async Functions, and M:N Tasks Must Be JIT-Safe**  
Suspension points are semantic boundaries. The JIT must correctly handle: yield and resume for generators, await and resume for async functions, throw into resumable functions, return and close semantics, completion/cancellation exceptions, exception propagation across suspension, frame reconstruction after suspension, local state after resume, and finalization of unresumed objects. Suspension points must be valid deopt and safepoint candidates.

**Rule 77 — Debugging, Tracing, Profiling, and Explainable Errors Must Remain Correct**  
The JIT must not break Omni tooling. Supported tooling may include: tracers, profilers, debuggers, breakpoints, line/call/return/exception events, monitoring hooks, and the Explainable Error Engine. When tooling is active, the JIT must either emit correct events with correct semantics, run unoptimized lower-tier code, or fall back to Tier 0. Missing events, duplicate events, wrong source positions, or wrong exception events are correctness bugs.

**Rule 78 — Object Identity and Shape-Shifting Semantics Must Be Explicit**  
If Omni exposes object identity via ===, WeakMap keys, or WeakRef, the engine must preserve those semantics. If using a moving GC: object identity semantics must remain correct, moving objects must not expose unstable addresses to Omni code, pinned objects must be used where address identity is observable, and handles or stable identity mechanisms must be provided. No optimization may assume that object addresses are stable unless the object model explicitly pins the object.

**Rule 79 — No Assumptions About Hashes, Randomness, or Addresses**  
The compiler must not persist or bake assumptions about: hash values, hash seeds, Map and Set iteration order beyond language guarantees, ASLR addresses, object addresses, code addresses, randomized runtime values, or nondeterministic allocation order. Persistent artifacts must not contain address-dependent assumptions unless explicitly relocated and validated at load time.

**Rule 80 — Static Typing Is Not Runtime Proof Unless Certified**  
Optional Omni type hints or static annotations do not by themselves justify unsafe optimization at runtime. Static proofs must be based on: sealed types, finality guarantees, module isolation, absence of dynamic mutation, verified ABI constraints, verified import boundaries, absence of introspection and monkey patching, and mechanically checked proof artifacts. If static proof cannot be maintained, the code must use guards or fall back.

---

## Part VIII: Deoptimization and GC Integration Laws

**Rule 81 — Deoptimization Metadata Is a Required Compilation Output**  
A compilation is not complete until it has produced: deopt points, FrameState snapshots, stack maps, GC reference maps, live range information, materialization plan for VirtualObjects, interpreter re-entry information, dependency list, guard metadata, and exception-state reconstruction info. If deopt metadata cannot be generated, the compilation must fail and fall back.

**Rule 82 — FrameState Must Be Complete and Machine-Checkable**  
FrameState must describe enough information to reconstruct the exact lower-tier state. It must include, as applicable: bytecode offset and instruction position, register-to-interpreter slot mapping, stack slot types, object references, primitive values, constants, closure cells, free variables, global/module version, builtin/runtime version, exception state, tracing/monitoring state, suspension state, materialized object graph for VirtualObjects, reference-management state, and object shape/trait versions. The verifier must reject incomplete FrameState.

**Rule 83 — Guard Failure Must Produce Exact Lower-Tier State**  
When a guard fails, the runtime must resume in a state observationally indistinguishable from the state the lower tier would have reached at that point. This includes: local variables, stack state, exception state, side effects already committed, reference-management state, frame visibility, source position, monitoring/tracing state, and object materialization state for VirtualObjects. If exact reconstruction is impossible, the speculation must be rejected at compile time.

**Rule 84 — Deopt Loops Must Be Detected and Throttled**  
Repeated deoptimization at the same site is a performance and correctness hazard. The runtime must track: deopt count per site, deopt count per function, deopt reason, time window, tier history. If thresholds are exceeded, the system must: disable the failing speculation, recompile with weaker assumptions, downgrade tier, blacklist the function temporarily or permanently, and emit telemetry.

**Rule 85 — Speculative Side Effects Must Be Reversible or Deferred**  
Speculative optimization must not commit irreversible Omni-visible side effects before the speculation is proven. If an effect cannot be proven safe: defer it, guard before it, materialize fallback state, or do not perform the optimization. Memory stores that may be observed by Omni code, native code, finalizers, weak references, Proxy traps, or debugging tools are not freely rollback-able.

**Rule 86 — All GC References in JIT Code Must Be Tracked**  
Generated code must not hold raw object pointers in registers, stack slots, or embedded constants across safepoints unless those references are recorded in GC maps. Rules: every reference register across a call or safepoint must be in a stack map; every reference spill must be visible to GC; embedded object pointers must use handles or be otherwise tracked; object references must not be hidden in untracked integer registers unless explicitly tagged and supported by the object representation.

**Rule 87 — Read and Write Barriers Must Be Correct**  
If the Concurrent Generational GC requires write barriers, every store of a reference in generated code must execute the correct barrier. If the GC requires read barriers, load barriers, or forwarding checks, every relevant reference load must execute them. Missing barriers are blocker bugs.

**Rule 88 — Generated Code Must Poll Safepoints**  
JIT code must include safepoint polls at: loop backedges, function calls, allocation sites, long native transitions where specified, OSR entry and exit points, tier transition points, invalidation points where required. Safepoint latency must be bounded to ensure the Concurrent GC operates efficiently.

**Rule 89 — All JIT Frames Must Be Walkable**  
Every JIT frame must be walkable by: GC, deoptimizer, profiler, debugger, exception unwinder, stack overflow checks, diagnostic tools. Frame metadata must include: frame size, return address location, saved registers, stack map, deopt info, callee-saved register locations, Omni frame association, native and managed transition markers.

**Rule 90 — Stack Overflow and Recursion Limits Must Be Checked**  
JIT code must respect Omni recursion limits and native stack limits. Checks must occur: before entering JIT frames, before inlined calls, before recursive calls, before OSR entry where applicable, before native transitions where stack usage changes. Failure must produce the correct Omni `RecursionError`, not a crash.

**Rule 91 — Allocation Fast Paths Must Handle Failure Safely**  
Allocation fast paths may optimize the common case, but slow paths must handle: heap exhaustion, memory allocation failure, GC pressure, object finalization hooks, allocation callbacks where specified, Omni memory error semantics. Generated code must not abort the VM on allocation failure unless the VM is in an unrecoverable state defined by the runtime spec.

**Rule 92 — Runtime Call Transitions Must Preserve ABI and Runtime State**  
Calls from JIT code into runtime helpers must preserve: calling convention, stack alignment, callee-saved registers, GC state, exception state, thread state, Omni runtime state, floating-point and vector register state as required. Runtime helpers must not assume JIT register contents beyond ABI.

**Rule 93 — Native Interop and FFI Are Opaque Unless Proven**  
Calls into native extensions or foreign functions are opaque barriers unless a formal ABI and effect proof exists. Assume native calls may: mutate arbitrary Omni state, call back into Omni code, allocate, raise Omni exceptions, change types/modules/globals, invalidate specialization assumptions, acquire/release runtime locks, trigger GC, observe object layout and shape, corrupt assumptions if misused. Optimizations across FFI boundaries require explicit proof and invalidation rules.

**Rule 94 — Weak References, Finalizers, and GC Callbacks Must See Valid State**  
JIT code must not leave WeakRef, WeakMap, WeakSet, FinalizationRegistry, or GC callbacks in states where they observe: partially initialized objects, invalid forwarding pointers, untracked references, missing barriers, stale object headers and shapes, inconsistent reference counts, objects that should have been materialized but were not due to PEA.

**Rule 95 — Object Shape and Trait Mutation Must Invalidate Specialized Code**  
Any change to assumptions used by inline caches or specialization must invalidate dependent code. This includes: prototype/trait chain changes, property addition or deletion, property descriptor changes, Object.preventExtensions/seal/freeze, method redefinition, getter/setter installation, Proxy creation wrapping existing objects, global rebinding, module namespace mutation, builtin shadowing and monkey patching, Array.prototype and Object.prototype pollution.

**Rule 96 — Tier 0 Is the Universal Correctness Fallback**  
Every executable Omni function must be runnable in Tier 0. No feature may be "JIT-only". If Tier 1 or Tier 2 cannot compile, patch, deopt, or execute code correctly, execution must fall back to Tier 0.

---

## Part IX: Code Cache, Patching, and Security Laws

**Rule 97 — Executable Memory Must Be W^X**  
JIT memory pages must never be simultaneously writable and executable. Code generation and patching must use one of: write-then-execute with protection changes via mprotect, separate staging and executable pages, atomic patching of existing executable locations where safe, platform-approved JIT memory mechanisms.

**Rule 98 — Code Publication Must Be Atomic**  
Function entry points, OSR entry points, trampolines, and metadata pointers must be published atomically. No thread may observe: partially initialized code, uninitialized metadata, missing deopt info, missing GC maps, half-patched jump tables. Publication must use release semantics; consumers must use acquire semantics.

**Rule 99 — Runtime Patching Must Be Safe Against Concurrent Execution**  
Patching running code must be safe. Requirements: patch sites must be aligned and architecturally safe, instruction sequences must not create invalid intermediate instructions, instruction cache coherence must be handled where required, concurrent threads must never execute corrupted instructions, patching must either use safepoints or architecture-safe atomic sequences.

**Rule 100 — Old Code May Be Freed Only After Quiescence**  
Old compiled code, deopt metadata, and dependency records must not be reclaimed until no thread can be executing or depending on them. Use: epoch-based reclamation, RCU-like quiescence, safepoint-based retirement, reference counting for code objects where appropriate. Code reclamation must be distinct from IR node reclamation.

**Rule 101 — Every Compiled Artifact Must Record Dependencies**  
Every compiled function must record dependencies sufficient for invalidation. Dependency examples: code object identity and version, function identity and version, shape and hidden class versions, trait chain versions, global and builtin versions, module versions, profile version, IR version, compiler version, target feature set, ABI version, Omni Semantic Profile version, runtime configuration.

**Rule 102 — Generated Code Must Be Constrained**  
Generated code must only call approved runtime entrypoints and must not directly: perform arbitrary syscalls unless mediated by the runtime, write outside its own frame and runtime-approved memory, execute arbitrary user-provided machine code, load arbitrary dynamic libraries unless approved, bypass sandbox and security policy.

**Rule 103 — Platform Exploit Mitigations Must Be Enabled Where Available**  
JIT must integrate with platform security features where available: non-executable stack, non-executable heap, CFI (Control Flow Integrity), shadow stacks, PAC and BTI on ARM64, pointer authentication where supported, CET where supported, ASLR-safe code generation, code signing where required, sandbox compatibility. If a mitigation is unavailable, the risk must be documented and configurable.

**Rule 104 — JIT Spraying Defenses Are Required**  
The JIT must not turn attacker-controlled data into executable instruction streams without mitigation. Mitigations may include: constant blinding, avoiding embedding uncontrolled immediate sequences, separating executable code from embedded data, limiting executable constant islands, code cache entropy and randomization where appropriate, validating inputs that influence codegen.

**Rule 105 — Profiles, Bytecode, and Caches Are Untrusted**  
Profile data, serialized IR, caches, and bytecode inputs must be validated before use. Malformed inputs must not cause: undefined behavior, memory corruption, arbitrary code execution, VM crashes, silent miscompilation. Invalid artifacts must be rejected or ignored with telemetry.

**Rule 106 — Code Cache Pressure Must Be Managed**  
The code cache must have explicit budgets and eviction policies. The system must monitor: total code size, metadata size, dependency graph size, number of live compiled functions, number of invalidated functions, patchpoint count, deopt metadata size. When pressure exceeds budgets, the system must throttle compilation, evict cold code, or fall back.

**Rule 107 — Serialized Profile and Bytecode Artifacts Must Include a Compatibility Manifest**  
Serialized PGO profiles and bytecode caches must include: Omni Semantic Profile version and hash, IR version and hash, compiler version, pass pipeline hash, target architecture, target feature set, ABI hash, runtime configuration hash, dependency fingerprints, security policy version, creation metadata.

**Rule 108 — Serialized Artifacts Must Be Verified Before Loading**  
Loading serialized profiles or bytecode caches must verify: manifest compatibility, integrity checksum and signature where required, dependency validity, target feature support, ABI compatibility, security policy compatibility, Omni Semantic Profile compatibility. On mismatch, the artifact must be rejected. Silent loading of incompatible artifacts is forbidden.

---

## Part X: Concurrency, Compilation Scheduling, and Tiering Laws

**Rule 109 — Compiler, Runtime, and GC Shared State Must Be Race-Free**  
All shared state accessed by mutator threads, compiler threads, GC threads, and background services must be synchronized using documented atomic and locking protocols. TSAN-clean is mandatory for supported concurrent tests.

**Rule 110 — Function Pointer Swaps Must Be Safe and Reversible**  
Installing new code must: use atomic publication, preserve old code until safe, avoid torn calls, avoid invalidating metadata still needed by running threads, support rollback where possible. Function installation must be testable independently of compilation.

**Rule 111 — Safepoint Latency Must Be Bounded**  
Threads must be able to reach a safepoint within a documented bounded time. Long-running generated loops must contain polls. Native helpers that run for long durations must cooperate with suspension protocols.

**Rule 112 — Compilation Latency and Memory Budgets Must Be Defined**  
Each tier must have explicit budgets: Tier 1 compile latency (strictly under 1 millisecond), Tier 2 compile latency (10 to 30 milliseconds), Tier 2 memory usage, IR memory usage, pass fixpoint iteration limits, code size limits, deopt metadata limits. Budget violations must trigger fallback or cancellation, not mutator stalls.

**Rule 113 — Compilations Must Be Cancellable**  
If a function is invalidated while compiling, the compiler must be able to cancel or discard the result without leaking memory or installing stale code.

**Rule 114 — Hotness Counters Must Be Robust**  
Profiling counters must be: thread-safe or explicitly racy with bounded error, saturating or overflow-safe, decaying where appropriate, resistant to pathological overflow, correlated with deopt feedback. Undefined behavior from counter overflow is forbidden.

**Rule 115 — Recompilation Must Be Throttled**  
Repeated compilation of the same function must be limited by: maximum recompiles per function, exponential backoff, deopt-history awareness, code-cache pressure awareness, budget awareness. No function may cause unbounded compile churn.

**Rule 116 — OSR Entry and Exit Must Be Semantically Exact**  
On-stack replacement must preserve exact Omni program state at OSR entry and exit. OSR must handle: loop induction variables, iterator state, exception state, closure cells, locals, stack values, suspension state for generators and async functions, deopt from OSR code back to interpreter.

**Rule 117 — Invalidation Must Be Ordered and Visible**  
Invalidation of dependencies must be visible before new assumptions are relied upon. The system must avoid: executing stale code after invalidation beyond allowed grace, installing code based on already-invalid dependencies, racing invalidation with installation.

**Rule 118 — No Global Locks on Hot Runtime Paths (No GIL)**  
Global locks (GIL) are strictly forbidden in hot runtime paths. Omni operates with free-threading. Hot paths include: inline-cache updates, guard checks, function entry dispatch, allocation fast paths, read and write barriers, safepoint polls, basic object property access. Concurrency must be managed via fine-grained `sync` blocks, lock-free algorithms, or thread-local state.

**Rule 119 — Tier Transitions Must Be Observable**  
All tier transitions must be recorded: Tier 0 to Tier 1, Tier 1 to Tier 2, deopt to lower tier, code invalidation, blacklist events, fallback events, recompilation events. Telemetry must include reasons and counters.

**Rule 120 — Compiler Bugs Must Not Crash User Programs**  
A compiler failure should degrade performance, not terminate the application. Compiler and runtime JIT bugs should result in: fallback to Tier 0, disabled optimization, diagnostic log, telemetry, replay artifact where possible. Process aborts are only acceptable for unrecoverable VM corruption and must be treated as P0 bugs.

---

## Part XI: IR, Passes, and Backend Laws

**Rule 121 — The IR Must Have an Explicit Effect Model**  
The Omni Sea of Nodes IR must explicitly represent effects and ordering. Effect classes should include at least: pure computation, allocation, Omni object mutation, global/module/builtin mutation, import effects, exception effects, I/O effects, FFI effects, GC effects, monitoring/tracing effects, deopt and guard effects, memory reads and writes, reference-management barrier effects, Proxy trap effects, prototype/trait chain observation effects, Symbol method invocation effects, shape-shifting/trait injection effects, `sync` block acquisition effects. Passes must not reorder effects without proof.

**Rule 122 — Speculative Nodes Must Carry Metadata**  
Every speculative node must record: speculation kind, PGO or static source, confidence, guard plan, FrameState, deopt target, cost, invalidation dependency, rollback and deferred-effect plan. No implicit speculation is allowed.

**Rule 123 — Passes Must Declare Contracts**  
Each pass must declare: required IR properties, produced IR properties, invalidated analyses, supported tiers, supported Omni capabilities, budget, determinism requirements, target dependencies, required verifier checks, telemetry hooks. Passes that cannot satisfy their contract must fail safely.

**Rule 124 — Compilation Must Be Deterministic and Replayable**  
Given the same source and bytecode, compiler version, flags, profile snapshot, target description, RNG seed, Omni Semantic Profile, and feature configuration, compilation must produce deterministic IR and code selection, except for explicitly documented nondeterminism. Nondeterminism sources must be logged.

**Rule 125 — Passes Must Not Use Hidden Global Mutable State**  
Hot-path passes must not depend on hidden global mutable state. Allowed global state: immutable configuration, interned symbol tables with proper synchronization, read-only target descriptions, versioned caches with explicit invalidation. Hidden singletons in pass logic are forbidden.

**Rule 126 — The Verifier Must Check Deopt and GC Metadata**  
The graph verifier must check not only IR consistency but also: every guard has FrameState, every deopt point is reachable, every GC reference across safepoint has a map, every effect chain is continuous, every speculative node has invalidation info, every materialized object graph is acyclic or properly handled.

**Rule 127 — Backend Lowering Must Preserve IR Semantics**  
Lowering from high and mid IR to machine code must preserve: effect order, exception semantics, numeric semantics including NaN and negative zero, overflow behavior, GC reference liveness, safepoint placement, deopt point mapping, stack layout constraints, Omni observable behavior. Backend optimizations may not silently change IR semantics.

**Rule 128 — Register Allocation Must Be GC-Reference Safe**  
The register allocator must ensure: GC references are not lost across calls and safepoints, spills of references are tracked, register maps are generated, callee-saved and caller-saved conventions are respected, reference registers do not alias untracked integer registers unless allowed by the object representation.

**Rule 129 — Target Features Must Be Gated and Recorded**  
Use of CPU features must be: runtime-detected for JIT, build-time validated, recorded in code metadata, protected by feature guards where needed. Generated code must not execute unsupported instructions.

**Rule 130 — Every IR Node and Trampoline Must Have a Specification**  
No IR node, runtime stub, or trampoline may exist without documentation covering: semantics, effects, tier behavior, lowering, verifier constraints, deopt behavior, GC behavior, tests.

**Rule 131 — Static Proofs Must Be Mechanically Checked**  
Static optimizations may not rely on human-only proof. Static proofs must be represented as machine-checkable artifacts or verifier constraints. If proof cannot be checked automatically, the optimization must use guards or be disabled.

**Rule 132 — Every Optimization Must Have a Kill Switch**  
Every nontrivial optimization should be disableable by: compiler flag, environment variable, configuration knob, runtime feature gate, per-function annotation where appropriate. This enables bisection and incident response.

---

## Part XII: Testing, Observability, and Governance Laws

**Rule 133 — Differential Oracle Testing Must Run Continuously**  
CI must compare behavior across: Tier 0, Tier 1, Tier 2, Omni reference implementation where applicable. Tests must include: normal programs, exceptions, async functions, generators, async generators, native interop interactions, dynamic prototype and shape/trait mutation, tracing and profiling enabled, GC stress, low-memory stress, recursion limits, large integers, floats, NaN, negative zero, BigInt, Omni-specific edge cases including Proxy, dynamic evaluation, and private fields.

**Rule 134 — Fuzzing Must Cover Bytecode, IR, Profiles, and Artifacts**  
Fuzzing must target: Omni source and bytecode inputs, IR inputs, serialized profiles, code cache metadata, patching sequences, deopt metadata, GC barrier sequences, FFI boundaries, type and shape/trait mutation schedules. Untriaged fuzz failures block release.

**Rule 135 — Sanitizer Matrix Is Mandatory**  
CI must run supported configurations with: ASan, UBSan, TSan where concurrency exists, MSan where supported, debug asserts, release builds, interpreter-only mode, JIT-enabled mode.

**Rule 136 — GC and Deopt Stress Tests Must Be First-Class**  
Dedicated stress modes must: force frequent Concurrent GC, force moving GC where applicable, force allocation failure, force guard failure, force deopt at every supported point, force weakref and finalizer activity, force code invalidation under load, force prototype/trait chain mutation under load.

**Rule 137 — Code Installation and Patching Must Be Concurrency-Tested**  
CI must test: installing code while executing old code, invalidating code while running, patching under load, retiring code under load, OSR entry during invalidation, deopt during patching.

**Rule 138 — Performance Gates Must Measure More Than Throughput**  
Performance CI must measure: startup time, warmup time, peak throughput, tail latency, compile latency p50 and p99, deopt rate, guard overhead, code size, memory usage, GC pause impact, compile CPU cost, memory pressure, tier transition counts. A regression in any critical dimension requires waiver.

**Rule 139 — Telemetry Must Be Structured, Stable, and Privacy-Safe**  
Telemetry must record: compile attempts, compile failures, fallback reasons, guard failures, deopt reasons, invalidations, code cache pressure, budget violations, blacklist events, performance counters. Telemetry must not include source code, user data, or secrets unless explicitly opted in.

**Rule 140 — Replay Artifacts Must Be Sufficient for Debugging**  
A failed compilation or deopt event should be replayable from: source and bytecode hash, IR snapshot, pass pipeline state, profile snapshot, compiler flags, target description, RNG seed, runtime config, Omni Semantic Profile version, dependency versions, failure location. Debugging should start from replay, not anecdote.

**Rule 141 — ABI and FFI Must Have Dedicated Tests**  
Dedicated tests must cover: Omni to native calls, native to Omni callbacks, register clobbering, stack alignment, exception propagation through FFI, free-threading interactions, reference ownership transfer, struct passing where supported, varargs conventions where supported, error return conventions.

**Rule 142 — Security Tests Must Be Part of CI**  
Security checks should include: W^X scans, executable memory accounting, JIT spraying PoCs, malformed artifact loading, code-cache exhaustion, patch race attempts, sandbox escape tests where applicable, dependency vulnerability scans.

**Rule 143 — Omni Compatibility Must Be Tracked Explicitly**  
The project must maintain: supported Omni Core Specification version range, supported standard-library subset, known divergences, unsupported features, OmniTest pass requirements, allowed failure list with owners and expiry dates.

**Rule 144 — All Major Optimizations Must Be Feature-Gated**  
Every major optimization must be capable of being disabled independently for bisection and emergency response. Examples: inlining, PEA, SLP, LICM, GVN, effect reordering, inline-cache specialization, type specialization, unrolling, OSR, guard hoisting, guard elimination, range analysis, Tier 2 compilation.

**Rule 145 — Exceptions to Rules Require an Exception Register**  
No rule may be silently bypassed. Exceptions must include: rule ID, reason, owner, risk assessment, mitigation, telemetry, expiry date, tech lead approval. Expired exceptions automatically become release blockers.

**Rule 146 — Every Rule Must Have Enforcement Metadata**  
Each rule in this document must specify: enforcement mechanism, owner, severity, test coverage, waiver policy. Rules without enforcement should be moved to guidelines or given an enforcement plan.

**Rule 147 — Maintain a Compliance Matrix**  
The repository must maintain a mapping from each rule to: CI check, test suite, verifier, review checklist, documentation, owner. This matrix must be reviewed each release.

**Rule 148 — Architectural Decisions Require ADRs**  
Any significant compiler and runtime decision must have an Architecture Decision Record. ADRs must cover: context, options considered, decision, consequences, performance impact, correctness impact, security impact, rollback plan.

**Rule 149 — Builds Must Be Hermetic and Dependencies Must Be Pinned**  
Compiler and runtime builds must be reproducible. Requirements: pinned dependencies, locked toolchains where practical, no network access during tests, reproducible artifact hashes, supply-chain review for new dependencies.

**Rule 150 — Stale Documentation Is a Defect**  
Documentation must be updated in the same PR as behavior changes. This includes: rules doc, IR spec, effect system spec, ABI spec, bytecode spec, Omni Semantic Profile docs, pass documentation, runtime documentation, telemetry schema, compatibility matrix. Stale docs are treated like stale code.

---

## Part XIII: Definitions

**Omni:** High-performance dynamic language engine. The compiler and runtime system governed by this specification.  
**Omni Semantic Profile:** A versioned description of the Omni semantics, capabilities, object model, memory model, exception model, and runtime contracts that Omni must preserve.  
**Omni Semantic Oracle:** The Omni Core Specification and OmniTest suite used to validate Omni semantic fidelity.  
**Hot path:** Compiler pass execution, guard execution, inline-cache fast paths, allocation fast paths, deopt entry trampolines, and dispatch loops. Not deopt materialization, which may allocate under budget.  
**Guard:** A runtime mechanism that validates a speculative assumption. May be a hardware branch, shape/version check, patchpoint, trap, dependency invalidation, or hardware check.  
**FrameState:** A snapshot attached to a speculative node that allows the deoptimizer to reconstruct the exact lower-tier execution state.  
**Deopt:** The process of transferring execution from a higher tier to a lower tier, typically Tier 0, while preserving observable program state.  
**Safe point:** A point in generated code where the thread can safely pause for GC, deopt, or suspension. Must have bounded latency.  
**Observable behavior:** Program output, exceptions, side effects, object mutation, weak-reference behavior, finalization, frame introspection, shape-shifting, trait injection, and monitoring events, as defined by the Omni Semantic Profile.  
**Effect:** An Omni-visible operation that cannot be freely reordered, deleted, or duplicated without semantic proof.  
**Dependency:** A versioned entity that a specialization relies on. If the dependency changes, the specialization must be invalidated or guarded.  
**Code installation:** The atomic publication of a compiled function's entry point, metadata, and deopt info.  
**Quiescence:** A state where no thread is executing old code or depending on old metadata, allowing safe reclamation.  
**Speculation:** An optimization that assumes a runtime property holds, guarded by a mechanism that triggers deopt on failure.  
**Static proof:** A mechanically-checked artifact demonstrating that a property holds without runtime guards.  
**Profile confidence:** A metric combining sample count, stability, age, decay, variance, and deopt correlation. Low-confidence data must not trigger aggressive speculation.  
**Tier transition:** A change in execution tier, including Tier 0 to Tier 1 to Tier 2 or deopt to a lower tier. Must be observable and recorded.  
**Fallback:** Graceful degradation to a lower tier or disabled optimization when compilation or speculation fails.  
**Kill switch:** A mechanism to disable a specific optimization for bisection and incident response.  
**Shape-Shifting / Trait Injection:** The Omni capability allowing any object to dynamically adopt, remove, or override methods, properties, or callable/iterable behaviors at runtime.  
**Explainable Error Engine:** The Omni runtime subsystem responsible for generating context-aware diagnostics, state snapshots, and semantic auto-fix suggestions upon failure.  
**No GIL / Free-Threading:** The Omni concurrency model guaranteeing that no global interpreter lock exists, allowing true parallel execution of mutator threads via fine-grained `sync` blocks and lock-free data structures.

---

## Part XIV: Normative References

- Omni Core Specification (latest edition)
- OmniTest Test Suite
- `docs/ir_spec.md` — Omni IR node specifications
- `docs/effect_system.md` — effect model and effect-chain rules
- `docs/abi.md` — calling conventions and FFI
- `docs/bytecode_spec.md` — Omni Bytecode format
- `docs/omni_semantic_profile.md` — Omni Semantic Profile schema
- `docs/runtime_contracts.md` — runtime helper and hook contracts
- Omni telemetry schema
- Omni compatibility matrix
- Target platform ABI documents, such as SysV x86-64 and AAPCS64
- Security policy document

---

## Part XV: Rule Severity and Waiver Process

**Severity levels:**
- **P0 (Blocker):** Silent data corruption, security vulnerabilities, crashes on valid input. Blocks release.
- **P1 (Critical):** Wrong results, missing deopt metadata, GC unsafety. Blocks merge to main.
- **P2 (Major):** Performance regressions beyond threshold, missing tests, documentation debt. Requires waiver with expiry.
- **P3 (Minor):** Style, naming, minor optimizations. Tracked but non-blocking.

**Waiver process:**
1. File an exception in the exception register.
2. Include: rule ID, reason, owner, risk assessment, mitigation, telemetry, expiry date, tech lead approval.
3. Waivers auto-expire. Expired waivers become release blockers.
4. No rule may be silently bypassed. Silent bypass is itself a Rule 145 violation.

---

## Part XVI: Compliance Matrix

The full compliance matrix, mapping Rule to CI check, test suite, and owner, is maintained in `docs/compliance_matrix.md` and reviewed each release.

Example entries:

| Rule | Enforcement | Test Suite | Owner | Notes |
|---|---|---|---|---|
| 3 | IR verifier | guard_tests | compiler team | Every PGO decision needs a guard |
| 47 | static analysis | diagnostic_tests | compiler team | Explainable Error Engine mandates |
| 67 | differential CI | omni_compat | runtime team | Omni Core Spec is authoritative |
| 86 | GC map verifier | gc_stress | GC team | No untracked refs across safepoints |
| 97 | OS memory tests | security_ci | security team | W^X mandatory |
| 118 | TSAN CI | free_threading_tests | runtime team | No GIL, lock-free hot paths |

---

*End of Omni Compiler Laws & Architecture Specification.*  
*Compliance is not optional. It is the foundation of Omni.*
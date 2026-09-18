# Full Uncompressed Design

This is the complete design, not a summary.

The system is an adaptive, speculative, region-based, partial-deoptimizing JIT runtime for a radically dynamic language with:

```text
no GIL
M:N tasks
trait injection
universal callability
universal iterability
implicit morphing/coercion
operator overloading
deep pattern matching
advanced GC
interactive diagnostics
```

The architecture is built around one core principle:

> Optimized code is immutable.  
> Speculation is dependency-tracked.  
> Invalidation marks dirty closures.  
> Dirty closures are expanded into safe regions.  
> Safe regions are rebuilt as new versions.  
> New versions are activated atomically.  
> Anything uncertain falls back.

---

# 1. High-level architecture

```text
Source / Bytecode
        |
Semantic Bytecode Layer
        |
Adaptive Register Interpreter
        |
Quickened / Fused Interpreter State
        |
T1 Stencil Baseline JIT
        |
T2 Near-Linear Optimizer
        |
T3 Quadratic Partial-Deopt Optimizer
        |
T4 Stable-State Superoptimizer
        |
Machine Code Artifacts
        |
Runtime Dependency Index
        |
GC / Safepoint / Task System
        |
Diagnostics / REPL / Telemetry
```

The interpreter and all JIT tiers share:

```text
feedback
shape metadata
dependency invalidation
GC maps
deopt/fallback paths
safepoint system
diagnostic state
```

---

# 2. Core correctness model

Correctness is always provided by:

```text
semantic bytecode
register interpreter
generic dispatch
generic runtime operations
full deopt/fallback
```

Optimized tiers are performance salvage mechanisms.

They may be:

```text
speculative
partial
versioned
invalidated
recompiled
retired
```

But they must never be required for semantic correctness.

The runtime must always be able to say:

```text
This optimized artifact is suspect.
Discard it.
Continue with a lower tier.
```

---

# 3. OmniShape object model

The object model is not a traditional class system.

It is a capability-based, versioned, dynamic shape system.

## 3.1 Object representation

Every object has:

```text
Object {
  header
  payload
}
```

Header:

```text
ObjectHeader {
  shape_ref
  identity
  gc_bits
  sync_word
  flags
}
```

Fields:

```text
shape_ref
  atomic pointer to current OmniShape

identity
  stable object identity
  must survive shape transitions

gc_bits
  mark bits
  generation
  forwarding bits
  barrier bits

sync_word
  thin lock
  monitor pointer
  lock domain
  ownership info

flags
  shared
  frozen
  sealed
  monitored
  finalizable
  disposable
  weak
  native
  virtualized
```

Payload depends on layout:

```text
FixedStructPayload
DictionaryPayload
DenseArrayPayload
SparseArrayPayload
ClosureEnvPayload
FunctionPayload
NativeHandlePayload
ProxyPayload
BoxedPrimitivePayload
```

## 3.2 OmniShape descriptor

```text
OmniShape {
  shape_id
  shape_version
  epoch

  layout_kind

  field_map
  slot_layout
  array_layout?
  dictionary_layout?

  trait_set
  capability_bits
  method_table
  property_table

  transition_table
  prototype_link?
  class_link?

  sync_policy
  gc_descriptor
  debug_descriptor

  stability_score
  dependency_keys
}
```

An OmniShape describes:

```text
physical layout
dynamic traits
protocol capabilities
method resolution
transition behavior
synchronization policy
GC scanning behavior
debugging metadata
```

## 3.3 Layout kinds

```text
FixedStruct
  known slots for known fields

FlexibleDictionary
  dynamic key/value property storage

DenseArray
  contiguous elements

SparseArray
  sparse indexed storage

ClosureEnv
  captured variables

FunctionObject
  executable object with properties

NativeHandle
  external resource/object

ProxyObject
  interception object

BoxedPrimitive
  boxed int/float/bool/string/etc

VirtualShape
  compiler-only representation for virtual objects
```

## 3.4 Capability bits

```text
CALLABLE
ITERABLE
INDEXABLE
SLICABLE
HASHABLE
NUMERIC
STRINGABLE
BOOLABLE
MATCHABLE
DISPOSABLE
SYNCABLE
ASYNC_CALLABLE
PROXYLIKE
CLOSURE
NATIVE
VIRTUAL
```

Capability bits enable fast checks:

```text
if shape.has(CALLABLE):
    invoke call trait
else:
    dynamic fallback
```

## 3.5 Traits

Traits are injectable behavior units.

```text
Trait {
  trait_id
  trait_version
  methods[]
  properties[]
  required_capabilities[]
  provided_capabilities[]
  conflict_policy
}
```

Traits can provide:

```text
call
next
getindex
setindex
add
sub
mul
div
eq
lt
to_int
to_num
to_str
to_bool
to_async
dispose
destructure
```

Trait injection creates a new shape version.

```text
old_shape + trait -> new_shape
```

Old shape remains valid for already-running code until invalidated/retired.

## 3.6 Shape transitions

Transitions:

```text
AddField
RemoveField
RenameField
ChangeFieldType
AddTrait
RemoveTrait
OverrideMethod
ChangePrototype
MorphToClass
Freeze
Seal
Share
Asyncify
Dispose
```

Each transition creates:

```text
new_shape_version
new_dependency_epoch
new_transition_edge
```

Transition table:

```text
TransitionTable {
  (from_shape, action, key) -> to_shape
}
```

## 3.7 Class shape vs object shape

```text
ClassShape {
  class_id
  version
  default_traits
  method_table
  prototype_chain
  field_blueprint
  instance_shape_template
}

ObjectShape {
  object-specific shape
  derived from ClassShape
  may fork after object-level mutation
}
```

Rules:

```text
Class mutation can affect many objects.
Object mutation forks object shape.
Object-specific trait injection does not mutate class shape.
```

## 3.8 Shape stability score

```text
ShapeStability {
  transitions_per_second
  trait_injection_rate
  method_override_rate
  field_churn
  polymorphism_level
  operator_cache_hits
  deopt_rate
}
```

Use:

```text
stable shape -> aggressive optimization
unstable shape -> generic dispatch / fallback
```

---

# 4. Bytecode model

There are three bytecode layers.

## 4.1 Semantic bytecode

Canonical and immutable.

Examples:

```text
LOAD_ARG
LOAD_CONST
LOAD_LOCAL
STORE_LOCAL
GET_PROP
SET_PROP
ADD
SUB
MUL
DIV
EQ
LT
CALL
JUMP
BRANCH
RETURN
MAKE_OBJECT
MAKE_CLOSURE
GET_ITER
NEXT
SPAWN
AWAIT
SYNC_ENTER
SYNC_EXIT
RAISE
TRY_BEGIN
TRY_END
MATCH
```

Semantic bytecode is used for:

```text
debugging
tracebacks
REPL recovery
deopt reconstruction
tier demotion
```

## 4.2 Quickened bytecode

Specialized overlay.

Examples:

```text
LOAD_LOCAL_FAST
GET_PROP_MONO
SET_PROP_MONO
ADD_INT_FAST
ADD_FLOAT_FAST
ADD_STRING_CONCAT_FAST
CALL_MONO
CALL_DIRECT_SMALL
BRANCH_TAKEN_FAST
BRANCH_NOT_TAKEN_FAST
NEXT_SHAPE_FAST
TO_INT_FAST
```

Quickened bytecode is speculative.

It must always be able to fall back to semantic bytecode.

## 4.3 Fused bytecode / superinstructions

Hot sequences are fused.

Examples:

```text
ADD_INT_RR
ADD_INT_RC
ADD_STORE_LOCAL
GET_PROP_ADD_INT_CONST
GET_PROP_CALL_MONO
LOAD_ADD_STORE
CALL_MONO_RETURN
ITER_NEXT_BRANCH
GET_ADD_SET_MONO
```

Fused ops must expand back to original bytecode PCs for:

```text
errors
debugging
side exits
deopt
```

---

# 5. Register interpreter

Tier 0 is a register-based interpreter.

## 5.1 Interpreter frame

```text
InterpFrame {
  function
  closure_env
  pc
  regs[]
  shape_ic_table
  call_ic_table
  exception_state
  sync_context
  debug_scope
  gc_map
}
```

Registers hold tagged values:

```text
TaggedValue {
  int
  float
  string
  bool
  null
  object_ref
  closure_ref
  native_handle
}
```

Unboxed fast paths are allowed, but tagged fallback must exist.

## 5.2 Dispatch model

```text
Generic dispatch table
Quickened dispatch table
Fused dispatch table
IC miss stubs
Guard failure stubs
```

Implementation options:

```text
switch-based portable interpreter
computed-goto fast interpreter
template interpreter
```

Recommended:

```text
portable core
+
generated fast handlers
```

## 5.3 Site profile metadata

Every bytecode site has:

```text
SiteProfile {
  pc
  counter
  state
  shape_id
  shape_version
  type_feedback
  call_targets[]
  branch_bias
  failure_count
  fusion_group
  dependency_keys[]
}
```

States:

```text
GENERIC
QUICKENED
POLYMORPHIC
MEGAMORPHIC
FUSED
DISABLED
```

## 5.4 Adaptive quickening

Rules:

```text
if site is hot and monomorphic:
    quicken
else if site is hot and polymorphic:
    use inline cache
else if site is unstable:
    keep generic or disable speculation
```

Example:

```text
GET_PROP obj, "x"
```

becomes:

```text
GET_PROP_MONO obj, shape_id, offset, version
```

Runtime check:

```text
if obj.shape == expected_shape and shape.version == expected_version:
    load field
else:
    ic_miss(pc)
```

## 5.5 Speculative arithmetic

Generic:

```text
ADD r3, r1, r2
```

Fast:

```text
ADD_INT_FAST r3, r1, r2
```

Semantics:

```text
if is_int(r1) and is_int(r2):
    r3 = int_add(r1, r2)
else:
    fallback(pc)
```

Overflow, big integers, and operator overloads must fall back unless explicitly proven safe.

## 5.6 Inline caches

IC kinds:

```text
monomorphic
polymorphic
megamorphic
prototype-chain
trait-capability
operator-pair
coercion
iterator
```

IC uses:

```text
property access
method call
operator dispatch
iterator protocol
coercion
pattern matching
```

## 5.7 Fusion engine

Fusion conditions:

```text
sequence is hot
side effects are closed
no arbitrary call in the middle
no exception edge in the middle
no GC-sensitive publication in the middle
branch behavior is simple
all subops have compatible fallback PCs
```

Do not initially fuse across:

```text
arbitrary calls
await points
sync exits
exception handlers
dynamic eval
trait injection
class mutation
task spawn
```

Fused op metadata:

```text
FusedOp {
  fused_id
  original_pcs[]
  subop_refs[]
  guard_info
  profile
  dependency_keys[]
  fallback_pc
  debug_info
  gc_map
}
```

## 5.8 Interpreter concurrency

No-GIL rules:

```text
semantic bytecode immutable
quickening overlay atomically updated
IC tables lock-free or sharded
shape invalidation via dependency epochs
running tasks may temporarily use old quickened state
guard failure handles races
```

---

# 6. Tier system

## 6.1 Tier overview

```text
T0
  register interpreter

T1
  stencil baseline JIT

T2
  near-linear optimizer

T3
  quadratic partial-deopt optimizer

T4
  no-budget stable-state superoptimizer
```

## 6.2 Tier capability matrix

```text
T0:
  correctness
  feedback
  no speculation beyond quickening

T1:
  direct bytecode-to-machine stencils
  no IR
  guarded specialization
  side exit to interpreter

T2:
  IR optimizer
  O(N) / O(N log N)
  local/region optimization

T3:
  advanced optimizer
  O(N^2) max
  partial deopt
  region rebuilding

T4:
  stable-state superoptimizer
  no compile-time budget
  whole-cluster optimization
```

---

# 7. T1 stencil baseline JIT

T1 compiles bytecode directly to machine code.

No graph IR.

## 7.1 T1 principles

```text
compile fast
use interpreter feedback
preserve exact dynamic semantics
side-exit safely to interpreter
support GC
support invalidation
```

T1 does not do:

```text
sea-of-nodes IR
global optimization
PEA
cross-function virtualization
partial deopt
```

## 7.2 Stencils

A stencil is a machine-code template.

```text
Stencil {
  id
  opcode_or_fused_id
  template_code
  operand_holes[]
  register_constraints
  side_exits[]
  gc_points[]
  dependencies[]
  debug_pcs[]
}
```

Stencil families:

```text
LoadLocalStencil
StoreLocalStencil
AddGenericStencil
AddIntFastStencil
AddFloatFastStencil
GetPropMonoStencil
SetPropMonoStencil
CallMonoStencil
ReturnStencil
BranchStencil
AllocObjectStencil
SpawnTaskStencil
SyncEnterStencil
BarrierStencil
ExceptionStubStencil
```

## 7.3 Super stencils

Super stencils fuse multiple operations.

```text
SuperStencil {
  fused_id
  original_pcs[]
  stencil_sequence[]
  guard_plan
  side_exit_plan
  output_state
  gc_map
  debug_expansion
}
```

Examples:

```text
AddIntRegRegStencil
AddStoreLocalStencil
GetPropAddIntConstStencil
LoadAddReturnStencil
CallMonoReturnStencil
IterNextBranchStencil
GetPropCallMonoStencil
```

## 7.4 T1 frame strategy

Use interpreter-compatible frame:

```text
T1Frame {
  interp_regs[]
  spill_slots[]
  closure_env
  function
  pc_slot
  exception_state
  sync_context
  gc_roots
}
```

Side exit:

```text
spill live machine registers into interpreter register slots
set frame.pc = original bytecode pc
jump to interpreter loop
```

## 7.5 T1 register allocation

Simple scheme:

```text
reserved registers:
  vm_state
  frame_base
  current_pc
  temp0
  temp1

allocatable registers:
  small fixed pool for hot vregs
```

Rules:

```text
keep loop-carried vregs in registers if possible
spill before calls, allocations, barriers, side exits
```

## 7.6 T1 guards

Guard failure metadata:

```text
GuardFailure {
  native_pc
  original_bytecode_pc
  live_vregs
  register_spill_plan
  fallback_target
}
```

Fallback targets:

```text
interpreter entry at exact pc
quickened interpreter resume
IC miss stub
runtime deopt stub
```

## 7.7 T1 GC integration

Each stencil reports:

```text
does it allocate?
does it store object refs?
does it call?
does it reach a safepoint?
which registers/slots contain oops?
```

GC map:

```text
GCMap {
  native_pc
  oop_registers[]
  oop_stack_slots[]
  derived_pointers[]
  bytecode_pc
}
```

## 7.8 T1 invalidation

T1 code is immutable.

When assumptions break:

```text
mark code not entrant
new calls go to interpreter
already-running code finishes or hits guard
```

Dependency keys:

```text
ShapeVersion
TraitVersion
MethodVersion
OperatorDispatch
CoercionRule
ClassShapeVersion
CapabilityPresence
SyncPolicy
```

---

# 8. IR stack for T2/T3/T4

T2 and above use an uncompressed sea-of-nodes IR.

## 8.1 IR levels

```text
TIR
  high-level typed sea-of-nodes
  assumptions
  guards
  effects
  regions

SIR
  scheduled IR
  explicit memory/effect tokens
  machine-near structure

LIR
  low-level IR
  registers
  stack slots
  patchpoints
  safepoints
  GC maps
  deopt offsets
```

## 8.2 Node structure

```text
Node {
  id
  opcode
  type
  inputs[]
  outputs[]
  control_dep?
  memory_dep?
  effect_dep?
  effect_class
  assumptions[]
  guards[]
  debug_info
  source_origin
  region_id
  flags
}
```

Fields:

```text
id
  unique within compilation unit

opcode
  operation kind

type
  result type
  includes shape/tag info

inputs
  ordered data/control/memory/effect inputs

outputs
  reverse use-list

control_dep
  control dependency

memory_dep
  memory ordering dependency

effect_dep
  effect ordering dependency

effect_class
  Pure
  Read
  Write
  Call
  Alloc
  Sync
  GC
  Coerce
  Dispatch
  Exception

assumptions
  dependency keys

guards
  runtime checks

debug_info
  bytecode pc
  scope
  variable names

region_id
  containing recompilation region
```

## 8.3 Edge types

```text
Data
  value flow
  typed

Control
  execution order
  untyped

Memory
  serializes loads/stores
  carries MemoryState

Effect
  serializes side effects
  carries EffectToken

Deopt
  links guard to deopt state snapshot
```

## 8.4 Vector nodes

```text
VectorNode {
  opcode
  vector_type
  scalar_pack[]
  memory_pack?
  predicate_mask?
  reduction_kind?
  alignment_info
  dependency_keys
  region_id
  debug_info
}
```

Vector opcodes:

```text
VectorAdd
VectorSub
VectorMul
VectorDiv
VectorCmp
VectorSelect
VectorLoad
VectorStore
VectorGather
VectorScatter
VectorSplat
VectorShuffle
VectorExtract
VectorInsert
VectorReduceAdd
VectorReduceMin
VectorReduceMax
VectorBroadcast
```

Vector types:

```text
VectorType {
  lane_count
  element_type
  vector_kind
  scalable?
  target_register_class
}
```

## 8.5 IR design rules

```text
No implicit state.
Memory and effects are explicit.
Every node belongs to a region.
Assumptions are embedded on nodes.
Use-lists are explicit.
Graph mutation is controlled.
Live compiled artifacts are not mutated.
```

---

# 9. Region model

A region is a compilation unit with a contract.

## 9.1 Region structure

```text
Region {
  id
  kind
  parent_region?
  entry_control
  exit_controls[]
  exception_exits[]
  entry_values[]
  exit_values[]
  memory_in
  memory_out
  effect_in
  effect_out
  assumptions[]
  guards[]
  deopt_state
  code_handle
  version
}
```

Region kinds:

```text
MethodRegion
LoopTraceRegion
InlinedCalleeRegion
GuardIslandRegion
ExceptionHandlerRegion
BridgeTraceRegion
OSREntryRegion
VectorRegion
ProtocolRegion
OperatorRegion
```

## 9.2 Boundary contract

```text
BoundaryContract {
  entry_values[]
  exit_values[]
  memory_in
  memory_out
  effect_in
  effect_out
  exception_paths[]
  deopt_states[]
  calling_convention
}
```

A region can be independently recompiled if:

```text
all incoming values are known
all outgoing values are producible
memory state is correctly threaded
exceptions are handled
control transfer is well-defined
```

## 9.3 Region recipe

Do not rebuild from live graph.

Rebuild from recipe:

```text
RegionRecipe {
  region_id
  kind
  bytecode_range
  trace_template
  caller_context
  inline_stack
  feedback_snapshot
  speculation_policy
  optimization_pipeline
  boundary_contract
  parent_region
  assumptions_required
}
```

## 9.4 Region artifact

```text
RegionArtifact {
  region_id
  version
  recipe
  boundary_contract
  optimized_ir
  machine_code
  dependency_keys
  guard_map
  deopt_map
  gc_map
  patchpoints
  materialization_plans
  verification_report
}
```

---

# 10. Dependency system

Every speculative optimization registers dependency keys.

## 10.1 Dependency index

```text
DependencyIndex:
  key -> set<NodeRef>
  key -> set<RegionRef>
  key -> set<CodeVersionRef>
  key -> set<GuardSiteRef>
```

## 10.2 Dependency keys

Shape keys:

```text
ShapeVersion(shape_id, version)
LayoutKind(shape_id)
FieldMap(shape_id, field_set)
TraitSet(shape_id, trait_set)
Capability(shape_id, capability)
PrototypeChain(shape_id, version)
ClassShapeVersion(class_id, version)
ObjectTraitInjection(object_id)
```

Method/trait keys:

```text
MethodVersion(shape_id, method_name, version)
TraitVersion(trait_id, version)
FunctionSummary(callee_id, version)
ClosureTarget(alloc_site)
FunctionCodeVersion(function_id)
```

Dispatch keys:

```text
OperatorDispatch(op, lhs_shape, rhs_shape)
ReflectedOperatorVersion(op, shape_id)
CoercionRule(shape_id, ToInt)
CoercionMethodVersion(shape_id, to_int)
IterableCapability(shape_id)
NextMethodVersion(shape_id)
CallableCapability(shape_id)
```

Memory/PEA keys:

```text
AllocationSiteKind(site, version)
ObjectShape(shape_id, version)
NoEscape(region_id, alloc_site)
NoIdentityLeak(region_id, alloc_site)
NoClosureCapture(site)
NoWeakReferenceUsage(site)
FieldLayout(shape_id, field_set)
ArrayElementKind(array_site, element_type)
ArrayLayout(array_site, layout_version)
ArrayStride(site, stride)
NoAlias(base_a, base_b)
BoundsProven(array_site, length_fact)
```

Concurrency keys:

```text
SyncPolicy(shape_id)
SharingDomain(object_id)
PublicationState(object_id)
```

Vector keys:

```text
VectorLegality(vector_site)
VectorWidth(vector_site, width)
ReductionSemantics(reduction_kind)
NoException(vector_region)
NoSideEffect(vector_region)
```

## 10.3 Invalidation

When runtime event breaks assumption:

```text
invalidate(key)
```

Runtime marks:

```text
dependent nodes
dependent regions
dependent code versions
dependent guard sites
```

Then:

```text
running threads exit safely
compiler schedules rebuild or fallback
```

---

# 11. T2 near-linear optimizer

T2 uses only O(N) or O(N log N) passes.

```text
N = nodes + edges + facts in compile region
```

## 11.1 T2 principles

```text
region-local
budgeted
fallback-safe
incremental-friendly
```

Avoid:

```text
global pairwise CSE
graph-coloring regalloc
unrestricted inlining
unbounded alias analysis
quadratic dataflow
```

## 11.2 T2 pass pipeline

```text
1. Normalize IR
2. Build CFG / RPO
3. Build dominator tree
4. Detect loops
5. Constant fold
6. GVN / CSE
7. Copy propagation
8. Branch simplification
9. Dead code elimination
10. Profile specialization
11. Guard insertion/dedup
12. Budgeted inlining
13. Region PEA
14. Load/store forwarding
15. LICM
16. Bounds/range fact propagation
17. Effect/memory closure
18. Scheduling
19. Linear-scan register allocation
20. Lower to machine IR
```

## 11.3 Normalize IR

```text
lower syntax-level ops
canonicalize commutative operands
normalize constants
normalize shape/type constants
split critical edges if cheap
verify use-lists
```

Complexity:

```text
O(N)
```

## 11.4 CFG/RPO/dominators/loops

```text
CFG construction: O(N)
Reverse post-order: O(N)
Dominator tree: O(N log N)
Loop forest: O(N)
```

Used for:

```text
global value numbering
load forwarding
guard dominance
DCE
phi placement
LICM
```

## 11.5 Constant folding

Worklist with tiny lattice:

```text
Top
Constant
Bottom
```

Complexity:

```text
O(N)
```

Rules:

```text
fold only exact semantics
do not fold calls unless proven pure
```

## 11.6 GVN / CSE

Hash-based value numbering.

Value key:

```text
opcode
operand value numbers
type/shape info
memory/effect context
```

Complexity:

```text
O(N) average
O(N log N) with tree maps
```

No pairwise expression comparison.

## 11.7 Copy propagation

Use union-find:

```text
x = y
```

places x and y in same equivalence class.

Complexity:

```text
O(N α(N))
```

## 11.8 Branch simplification

Use constant facts and profile facts.

```text
if true:
    rewrite
if false:
    rewrite
if shape known:
    specialize
```

Complexity:

```text
O(N)
```

## 11.9 Dead code elimination

Roots:

```text
returns
calls
stores
exceptions
deopt states
GC roots
```

Complexity:

```text
O(N)
```

Do not eliminate:

```text
effectful nodes
guards protecting live speculative nodes
```

## 11.10 Profile specialization

Use interpreter feedback:

```text
int/float/string types
OmniShape ids
shape versions
call targets
branch probabilities
iteration protocol
operator dispatch
coercion rules
```

Transforms:

```text
ADD -> ADD_INT_FAST
GET_PROP -> GET_PROP_SHAPE
CALL -> CALL_MONO_TARGET
NEXT -> NEXT_SHAPE_SPECIALIZED
EQ -> EQ_INT_FAST
```

Each specialization adds guards.

Complexity:

```text
O(N)
```

## 11.11 Guard insertion/dedup

Guard node:

```text
Guard {
  predicate
  dependency_keys
  deopt_state
  probability
}
```

Guard kinds:

```text
type
shape
trait capability
prototype chain
field layout
call target
array bounds
integer range
coercion validity
sync policy
```

Dedup key:

```text
predicate + value + shape/version + dominance context
```

Complexity:

```text
O(N log N)
```

## 11.12 Budgeted inlining

Use priority queue:

```text
score =
  hotness
  smallness
  stability
  expected optimization benefit
- deopt risk
- compile cost
```

Limits:

```text
max inline budget
max callee size
max inline depth
max region growth
max guard growth
```

Complexity:

```text
O(N log N)
```

## 11.13 Region PEA

Steps:

```text
1. Find allocations.
2. Track uses.
3. Find escape fences.
4. Determine virtualizable objects.
5. Scalar replace fields.
6. Insert materialization points.
```

Escape fences:

```text
unknown call
store to unknown heap
exception exit
OSR exit
identity observation
closure capture
weak reference
finalizer
native handoff
dynamic eval
```

Complexity:

```text
O(N log N)
```

## 11.14 Load/store forwarding

Use dominator-tree walk with scoped maps.

Forward load if:

```text
same base
same field
same shape/version guard dominates
no intervening store/call/effect that may alias
```

Complexity:

```text
O(N log N)
```

## 11.15 LICM

Hoist if:

```text
operands defined outside loop
no side effects or effects are safe
no memory conflict inside loop
no exception risk
guard dependencies stable
```

Complexity:

```text
O(N)
```

## 11.16 Bounds/range fact propagation

Facts:

```text
x >= 0
x < length
x is int
index in bounds
```

Complexity:

```text
O(N log N)
```

Avoid full constraint solving.

## 11.17 Effect/memory closure

Check:

```text
memory_in -> region effects -> memory_out
effect ordering preserved
no dangling memory tokens
no duplicate side effects
exception effects handled
GC barriers present where needed
```

Complexity:

```text
O(N)
```

## 11.18 Scheduling

List scheduling with priority queue.

Priority:

```text
critical path
effect dependencies
memory dependencies
control dependencies
GC barriers
guard ordering
```

Complexity:

```text
O(N log N)
```

## 11.19 Linear-scan register allocation

```text
compute live intervals
sort by start
assign registers
spill when pressure high
```

Complexity:

```text
O(N log N)
```

---

# 12. T3 quadratic partial-deopt optimizer

T3 allows O(N), O(N log N), and O(N^2) passes.

No superquadratic passes.

## 12.1 T3 purpose

```text
advanced region optimization
cross-function virtualization
micro PEA
loop specialization
guard optimization
region splitting
partial recompilation
```

T3 compiles region artifacts, not monolithic methods.

## 12.2 T3 pass pipeline

```text
1. Import region recipe.
2. Build CFG/dominators/loops.
3. Constant fold, DCE, GVN.
4. Shape/type/trait specialization.
5. Guard insertion/dedup/placement.
6. Alias-pair analysis.
7. Effect-pair ordering.
8. Budgeted cluster inlining.
9. Region PEA.
10. Micro PEA.
11. Cross-function virtualization.
12. Loop versioning/peeling/hoisting.
13. Redundancy elimination.
14. Devirtualization.
15. Superword-level parallelism.
16. Region partitioning.
17. Verification.
18. Scheduling.
19. Register allocation.
20. Emit region artifact.
```

## 12.3 Alias-pair analysis

Alias classes:

```text
exact field
shape field class
array element class
dictionary class
closure environment
native handle
unknown heap
```

Checks:

```text
load A may alias store B?
effect X may invalidate load Y?
call C may escape object O?
```

Complexity:

```text
O(N^2)
```

## 12.4 Effect-pair ordering

Compute happens-before relation between effectful nodes.

Used for:

```text
safe motion
redundancy elimination
PEA
vectorization legality
partial deopt memory closure
```

Complexity:

```text
O(N^2)
```

## 12.5 Guard optimization

Tasks:

```text
remove duplicate guards
hoist dominated guards
merge equivalent guards
push guards closer to speculation
remove guards made redundant by stronger guards
assign dependency keys
attach deopt states
```

Complexity:

```text
O(G^2), G <= N
```

## 12.6 Region PEA in T3

More precise than T2.

Uses:

```text
alias-pair analysis
effect closure
escape fences
materialization planning
```

Complexity:

```text
O(N^2)
```

## 12.7 Micro PEA

Micro PEA is conditional/temporal virtualization.

Virtual object states:

```text
Virtual
ConditionalVirtual
Materialized
Dead
```

Materialization fences:

```text
unknown call
store to escaping heap
exception path
identity observation
closure capture
weak reference
finalizer
debug observation
task publication
sync publication
```

Complexity:

```text
O(N^2)
```

## 12.8 Cross-function virtualization

Requirements:

```text
function summaries
escape summaries
effect summaries
trait version dependencies
shape stability dependencies
```

Function summary:

```text
FunctionSummary {
  function_id
  version
  escape_behavior
  alloc_sites[]
  field_reads[]
  field_writes[]
  shape_transitions[]
  effect_class
  may_throw
  may_escape_arguments
  may_capture_arguments
}
```

Complexity:

```text
O(N^2)
```

## 12.9 Loop optimizations

Allowed:

```text
loop detection
loop versioning
loop specialization
bounds check elimination
induction variable analysis
loop-invariant code motion
loop peeling with budget
OSR entry generation
loop region extraction
```

Complexity:

```text
O(N^2)
```

## 12.10 Redundancy elimination

Targets:

```text
redundant shape guards
redundant capability checks
redundant loads
redundant type tests
redundant bounds checks
redundant operator dispatch lookups
```

Complexity:

```text
O(N^2)
```

## 12.11 Region partitioning

Candidate regions:

```text
inlined callee
loop body
guard island
exception scope
hot trace fragment
OSR loop region
volatile speculative island
stable core region
```

Evaluation:

```text
boundary value count
memory/effect closure
exception edges
side-effect density
deopt cost
recompile benefit
stability score
```

Complexity:

```text
O(N^2)
```

---

# 13. Partial deopt design

Partial deopt is:

```text
dependency-driven invalidation
+ minimal safe region recompilation
+ atomic swap
```

Do not mutate live optimized graph.

## 13.1 Compile-time requirements

Each optimized node carries:

```text
assumptions[]
guards[]
effect_class
region_id
debug_info
source_origin
```

Each region carries:

```text
entry_control
exit_controls[]
entry_values[]
exit_values[]
memory_state
effect_state
exception_exit
assumptions[]
guards[]
code_handle
```

## 13.2 Guard failure flow

```text
on_guard_failure(guard):
  state = capture_deopt_state(guard)

  if not partial_deopt_enabled:
      return full_deopt(state)

  if not within_deopt_budget():
      return full_deopt(state)

  dirty = mark_dependencies_from(guard)

  region = choose_minimal_safe_region(dirty)

  if region is unsafe:
      return full_deopt(state)

  run fallback_stub(state)
  schedule_partial_recompile(region)
```

The failing thread must not wait for recompilation.

## 13.3 Dirty-node closure

Initial:

```text
dirty_nodes = direct_dependents(invalidated_key)
```

Expand:

```text
data users
phi users
memory dependents
effect dependents
guard dependents
boundary users if contract changes
```

Then:

```text
region = minimal_region_containing(dirty_nodes)
```

If too large:

```text
escalate
```

## 13.4 Region safety checks

A valid partial-recompile region must have:

```text
clearly defined control entry
clearly defined control exits
no dangling uses leaving the region
no unhandled exceptions
memory state entry/exit
effect ordering preserved
phi nodes resolvable
no side-effect duplication
no ambiguous GC roots
reconstructable deopt state
```

If any check fails:

```text
fallback = full_method_deopt_or_baseline
```

## 13.5 Partial rebuild algorithm

```text
partial_recompile(region):
  old_graph = region.graph

  dirty_nodes = compute_dirty_closure(region)

  if too_large(dirty_nodes):
      escalate_full_recompile()

  frontier = compute_boundary(dirty_nodes)

  new_subgraph = rebuild_from_recipe(
      region.recipe,
      current_feedback,
      current_assumptions,
      frontier
  )

  insert_new_guards(new_subgraph)

  connect_inputs(frontier.inputs, new_subgraph)
  connect_outputs(new_subgraph, frontier.outputs)

  verify_graph(new_subgraph)

  if verification_fails:
      discard(new_subgraph)
      escalate_full_recompile()
  else:
      schedule_activation(new_subgraph)
```

## 13.6 Atomic activation

```text
new_code = compile(new_region)

register_with_safepoint_system(new_code)

at_safe_point:
  patch region entry to new_code
  retire old_code after no threads inside
```

Requirements:

```text
old code remains valid until all threads exit
old GC maps remain valid
no thread sees mixed old/new code
patch points are limited and audited
rollback is possible
```

## 13.7 Fallback ladder

```text
1. Side-exit to baseline/interpreter at guard.
2. OSR exit if inside loop.
3. Full method deopt.
4. Disable optimizations for method.
5. Disable partial deopt globally if failure rate high.
```

## 13.8 Deopt budget and hysteresis

Track:

```text
deopt_count
partial_recompile_count
failure_rate
time_since_last_deopt
compile_cost
```

Policy:

```text
if deopt_rate > threshold:
    disable partial deopt for method

if repeated_failure:
    downgrade method to baseline

if global_failure_rate > threshold:
    disable partial deopt globally
```

---

# 14. T4 stable-state superoptimizer

T4 is for code that is hot and stable for a long time.

Compile time is not the limiting factor.

Correctness is.

## 14.1 T4 entrance criteria

Promote when:

```text
hot
stable
low deopt rate
low shape churn
low trait injection rate
stable call targets
stable prototype chains
stable operator dispatch
stable coercion behavior
stable branch probabilities
long uptime since last invalidation
```

## 14.2 T4 compile unit

T4 compiles clusters:

```text
hot function clusters
hot call graph components
hot trace supergraphs
hot module subsets
hot object-shape ecosystems
hot trait/composition clusters
hot protocol dispatch families
```

Structure:

```text
T4Cluster {
  functions[]
  inlined callees[]
  hot traces[]
  shapes[]
  traits[]
  operator dispatch sites[]
  allocation sites[]
  dependency graph
  deopt plan
}
```

## 14.3 T4 pipeline

```text
1. Extract stable hot cluster.
2. Build global dependency graph.
3. Build global call/trace graph.
4. Build global shape/trait graph.
5. Build global allocation/escape graph.
6. Fully inline stable hot code.
7. Global SSA / sea-of-nodes construction.
8. Global type/shape inference.
9. Global alias analysis.
10. Global effect analysis.
11. Global PEA.
12. Cross-function virtualization.
13. Micro PEA.
14. Global guard minimization.
15. Global redundancy elimination.
16. Loop optimizations.
17. Shape specialization.
18. Trait specialization.
19. Operator dispatch specialization.
20. Superword-level parallelism.
21. Whole-cluster scheduling.
22. Whole-cluster register allocation.
23. Machine layout optimization.
24. Deopt/materialization planning.
25. Verification.
26. Atomic activation.
```

## 14.4 T4 inlining

Inline:

```text
hot functions
trait methods
protocol methods
call trait methods
next trait methods
to_int / to_str coercion methods
operator overload methods
small dynamic dispatch sites
stable closure calls
```

No compile-time budget.

## 14.5 T4 shape specialization

If shapes are stable:

```text
direct field offsets
no generic property lookup
minimal shape guards
direct operator dispatch
inlined trait methods
```

## 14.6 T4 trait specialization

Stable traits compile like static methods.

Examples:

```text
obj.call(...)
obj.next()
obj.to_int()
obj.dispose()
```

become direct specialized code.

## 14.7 T4 operator specialization

For stable operator behavior:

```text
a + b
```

can become:

```text
direct specialized add for observed shape pair
```

Mixed cases:

```text
Vector + scalar
scalar + Vector
Vector + Vector
Vector + coerceable scalar
```

## 14.8 T4 global PEA

Objects allocated in one function and used across several can be eliminated across entire cluster if no escape fence is reached.

## 14.9 T4 global guard minimization

Remove:

```text
dominated guards
implied guards
guards redundant due to entry contracts
guards redundant due to stronger previous guards
```

## 14.10 T4 activation

```text
T3 code continues running
T4 compiles in background
T4 verifies
at safepoint:
    switch entry to T4
    mark T3 not entrant
    retire T3 via epoch
```

If activation fails:

```text
keep T3
discard T4
log failure
```

---

# 15. Devirtualization

Devirtualization is capability resolution plus version-guarded direct call.

## 15.1 Dispatch sources

```text
obj.method(args)
obj(args)
iter.next()
a + b
a == b
a[index]
to_int(x)
to_str(x)
destructure(x)
dispose(x)
closure(args)
```

## 15.2 Devirtualization levels

```text
Level 0:
  generic dispatch

Level 1:
  monomorphic shape-guarded direct call

Level 2:
  polymorphic PIC direct calls

Level 3:
  capability/protocol direct calls

Level 4:
  inlined direct calls
```

## 15.3 Devirtualization rule

A virtual call becomes direct only if compiler can prove or guard:

```text
receiver/operand shape is known
capability exists
trait/method version is known
method cannot be overridden by later trait
prototype/class chain cannot interfere
dynamic mutation cannot hijack call before guard
```

## 15.4 IR transformation

Before:

```text
Call {
  kind: Virtual
  callee_name
  receiver
  args[]
}
```

After:

```text
Call {
  kind: DirectGuarded
  target_method
  target_version
  receiver
  args[]
  guards[]
  dependency_keys[]
  fallback_stub
}
```

If fully proven:

```text
Call {
  kind: DirectUnguarded
  target_method
  target_version
}
```

## 15.5 Call site states

```text
UNKNOWN
MONOMORPHIC
POLYMORPHIC
MEGAMORPHIC
UNSTABLE
BLACKLISTED
```

Policy:

```text
MONOMORPHIC:
    direct guarded call

POLYMORPHIC:
    PIC with direct calls

MEGAMORPHIC:
    generic dispatch

UNSTABLE:
    no speculation

BLACKLISTED:
    disable devirtualization
```

## 15.6 Devirtualization pass

```text
for each virtual call site:
    classify profile

    if monomorphic:
        insert shape/version guard
        replace with direct call

    elif small polymorphic:
        build PIC dispatch chain
        insert direct calls for each case

    elif protocol capability stable:
        devirtualize through capability method

    elif megamorphic/unstable:
        keep generic dispatch

    register dependency keys
    attach fallback stub
```

## 15.7 Universal callability devirtualization

For:

```text
obj(args)
```

Use:

```text
guard obj.shape == S
guard S.call_version == V
direct_call S.call_method
```

Dependency keys:

```text
CallableCapability(shape_id)
CallTraitVersion(shape_id)
ClosureTarget(closure_site)
FunctionCodeVersion(function_id)
```

## 15.8 Iterator devirtualization

For:

```text
for x in obj
```

Devirtualize:

```text
get_iterator
next
```

If stable:

```text
guard it.shape == IteratorShape
guard IteratorShape.next_version == V
direct_call next_method
```

Dependency keys:

```text
IterableCapability(shape_id)
NextMethodVersion(shape_id)
IteratorProtocolVersion(shape_id)
```

## 15.9 Operator devirtualization

For:

```text
a + b
```

Use shape pair:

```text
guard a.shape == ShapeA
guard b.shape == ShapeB
guard AddTraitVersion == V
direct_call ShapeA.add
```

Dependency keys:

```text
OperatorDispatch(add, ShapeA, ShapeB)
TraitVersion(add_trait)
ReflectedOperatorVersion(radd_trait)
```

## 15.10 Coercion devirtualization

For:

```text
to_int(x)
```

If stable:

```text
guard x.shape == S
guard S.to_int_version == V
direct_call S.to_int
```

Dependency keys:

```text
CoercionRule(shape_id, ToInt)
CoercionMethodVersion(shape_id, to_int)
```

Coercion is effectful.

It is also:

```text
escape fence
materialization point
```

## 15.11 Closure devirtualization

If closure target known:

```text
guard closure.target == adder_fn
direct_call adder_fn
```

Dependency keys:

```text
ClosureTarget(alloc_site)
ClosureEnvLayout(alloc_site)
FunctionCodeVersion(adder_fn)
```

## 15.12 Dangerous devirtualization cases

Do not devirtualize when:

```text
method can be overridden by trait injection
prototype chain is unstable
receiver is proxy-like
object is used in reflection
object may become callable/iterable dynamically
call may trigger coercion with side effects
operator overload may mutate shapes
receiver is megamorphic
call site is unstable
callee has unknown effect summary
dynamic eval can redefine callee
```

---

# 16. PEA and virtualization

## 16.1 PEA forms

```text
Local PEA
Region PEA
Cross-function PEA
Micro PEA
```

## 16.2 Virtual object

```text
VirtualObject {
  alloc_site
  shape
  fields[]
  state
  escape_fences[]
  materialization_points[]
  identity_observable
  gc_visible
  deopt_materialization_plan
}
```

States:

```text
Virtual
ConditionalVirtual
Materialized
Dead
```

## 16.3 Escape fences

```text
unknown call
store to unknown heap
exception exit
OSR exit
identity observation
closure capture
weak reference
finalizer
native handoff
dynamic eval
reflection
proxy publication
task publication
sync publication
debug observation
```

## 16.4 Region PEA

```text
PEARegion {
  region_id
  alloc_sites[]
  virtual_objects[]
  escape_fences[]
  materialization_points[]
  memory_effects[]
  dependencies[]
  deopt_materialization_plan
}
```

Rule:

```text
An object can be virtualized inside a region
if no observable reference to it can leave the region
before a materialization fence.
```

## 16.5 Cross-function virtualization

Use summaries:

```text
FunctionSummary {
  function_id
  version
  escape_behavior
  alloc_sites[]
  field_reads[]
  field_writes[]
  shape_transitions[]
  effect_class
  may_throw
  may_escape_arguments
  may_capture_arguments
}
```

If callee summary proves argument does not escape:

```text
keep argument virtual across call
```

Otherwise:

```text
materialize before call
```

## 16.6 Micro PEA

Micro PEA is delayed materialization.

Not time-based.

Semantic fence-based.

Example:

```text
obj = alloc
if rare_condition:
    escape(obj)
```

Compiler emits:

```text
virtual obj
if rare_condition:
    materialize obj
    call escape(obj)
else:
    continue with no allocation
```

## 16.7 PEA dependencies

```text
AllocationSiteKind(site, version)
ObjectShape(shape_id, version)
NoEscape(region_id, alloc_site)
NoIdentityLeak(region_id, alloc_site)
NoPrototypeInterference(shape_id)
CalleeEscapeSummary(callee_id, version)
FieldLayout(shape_id, field_set)
NoDynamicPropertyAccess(site)
NoClosureCapture(site)
NoWeakReferenceUsage(site)
```

If dependency breaks:

```text
mark virtualization dirty
expand to safe region
recompile with less virtualization
or fully materialize
```

---

# 17. Superword-level parallelism

SWLP is first-class vectorization.

## 17.1 SWLP forms

```text
straight-line basic block vectorization
packed arithmetic
packed loads/stores
interleaved memory access
reduction vectorization
compare/select vectorization
predicate/mask vectorization
loop vectorization
```

## 17.2 Vector pack

```text
Pack {
  pack_id
  nodes[]
  opcode_class
  element_type
  lane_count
  dependence_class
  memory_stride
  legality
  cost
}
```

## 17.3 SWLP pass

```text
1. Identify vectorizable scalar operations.
2. Build dependence graph within basic block or loop body.
3. Group operations by opcode and type.
4. Detect adjacency and stride.
5. Form candidate packs.
6. Check memory legality.
7. Check side-effect legality.
8. Check reduction legality.
9. Apply cost model.
10. Replace scalar nodes with vector nodes.
11. Emit scalar epilogue/remainder.
```

## 17.4 Vectorizable operations

Allowed initially:

```text
integer add
integer sub
integer mul if fixed-width semantics preserved
float add/sub/mul/div if FP semantics preserved
bitwise and/or/xor
shift with uniform shift count or vector shift support
compare
select
min/max
load primitive elements
store primitive elements
```

Not allowed initially:

```text
object field access
dynamic property access
trait dispatch
operator overload dispatch
coercion calls
GC-visible pointer stores
barrier-heavy stores
sync operations
exception-throwing operations
arbitrary calls
```

## 17.5 Memory legality

Must prove:

```text
no unsafe overlap
correct alignment or unaligned support
correct stride
no conflicting load/store ordering
no aliasing violation
no GC barrier violation
```

Legal:

```text
distinct arrays proven non-aliasing
same array with safe stride
contiguous load/store with no overlap
interleaved access with shuffle support
```

Illegal/conservative:

```text
unknown aliasing
overlapping forward/backward dependence
gather/scatter without target support
object arrays with GC barriers
dictionary-backed arrays
sparse arrays
dynamic length without bounds proof
```

## 17.6 Alignment facts

```text
AlignmentFact {
  base_value
  alignment
  modulo
  stride
}
```

If alignment unknown:

```text
use unaligned vector load if target supports it
or peel loop to align
or scalarize
```

## 17.7 Reductions

Patterns:

```text
sum
product
min
max
bitwise and/or/xor
```

Integer reductions easier if overflow semantics exact.

Floating point reductions require:

```text
language allows reassociation
or order-preserving tree reduction
```

Otherwise do not vectorize reduction.

## 17.8 Masks and predicates

```text
VectorSelect(mask, true_vec, false_vec)
MaskedVectorLoad(mask, base, offset)
MaskedVectorStore(mask, base, offset, value)
```

Used for:

```text
loop remainder
conditional vector lanes
branch-free scalar selects
boundary handling
```

## 17.9 SWLP and GC

Safe vectorization targets:

```text
primitive int arrays
primitive float arrays
byte arrays
boolean arrays
bit arrays
native numeric buffers
```

Unsafe by default:

```text
object arrays
dictionary arrays
closure arrays
shape-transitioning arrays
weak reference arrays
```

If vector store touches GC-visible memory:

```text
disable unless barrier lowering is proven equivalent
```

## 17.10 SWLP dependencies

```text
VectorLegality(vector_site)
VectorWidth(vector_site, width)
ArrayElementKind(array_site)
ArrayLayout(array_site)
NoAlias(array_a, array_b)
BoundsProven(array_site)
OperatorSemantics(op, shape_pair)
ReductionSemantics(reduction_kind)
NoException(vector_region)
NoSideEffect(vector_region)
```

## 17.11 SWLP tier placement

```text
T2:
  mini-SWLP with fixed window
  O(N)

T3:
  full SWLP and loop vectorization
  O(N^2)

T4:
  global vector clustering
  unbounded
```

---

# 18. Register allocation

## 18.1 T1 allocation

Interpreter-compatible frame.

Simple policy:

```text
hot vregs -> machine registers
others -> stack slots
side exit -> spill live machine registers back to frame
```

## 18.2 T2 linear scan

```text
compute live intervals
sort by start
assign registers
spill when pressure high
```

Complexity:

```text
O(N log N)
```

## 18.3 T3 linear scan + quadratic refinement

Base:

```text
linear scan
```

Refinement:

```text
coalesce move-related intervals
reduce spills using profile weights
bounded by interval count squared
```

Complexity:

```text
O(N^2)
```

## 18.4 T4 global graph coloring

Build interference graph from whole cluster liveness.

Use:

```text
Chaitin-Briggs style simplify/coalesce/freeze/spill
aggressive coalescing
global spill placement
rematerialization
```

Complexity:

```text
unbounded
```

## 18.5 Vector register allocation

Register classes:

```text
GPR
FPR
VR
MASK
```

Allocator handles:

```text
vector live intervals
wide spill slots
splat rematerialization
shuffle temporaries
mask registers
cross-class moves
scalar/vector copies
```

If vector pressure too high:

```text
reduce vector width
or scalarize
```

---

# 19. GC model

Use C2-style compiler/GC cooperation.

## 19.1 Compiler/GC contract

Compiled code obeys:

```text
safe object access through known pointers
GC barriers on reads/writes when required
safepoints at well-known locations
accurate live-reference maps
```

GC owns:

```text
collection policy
moving/non-moving
remembered sets
pause strategy
```

## 19.2 GC descriptor in OmniShape

```text
GCDescriptor {
  pointer_field_offsets[]
  weak_field_offsets[]
  finalizer?
  scan_mode
  write_barrier_class
}
```

Scan modes:

```text
FIXED_FIELDS
DICTIONARY
DENSE_ARRAY
SPARSE_ARRAY
CLOSURE_ENV
NATIVE_HANDLE
PROXY
FUNCTION_OBJECT
```

## 19.3 Barrier set

```text
OmniBarrierSet {
  pre_write_barrier
  post_write_barrier
  store_barrier
  load_barrier
  arraycopy_barrier
  shape_transition_barrier
  publication_barrier
  weak_ref_barrier
}
```

## 19.4 Generational barriers

Old-to-young stores require remembered set update.

```text
write_barrier(obj, field, value)
```

May lower to:

```text
card mark
dirty card queue
remembered set update
```

## 19.5 Concurrent mark barriers

Pre/post write barriers:

```text
pre_write_barrier(old_value)
post_write_barrier(new_value)
```

## 19.6 Moving collector barriers

Options:

```text
forwarding + fixup
load/read barriers
colored pointers
```

## 19.7 Shape transition barriers

Trait injection and shape changes can add references.

Therefore:

```text
trait injection is a GC-mutating event
```

It needs:

```text
write barrier
shape publication barrier
GC descriptor update visibility
```

## 19.8 Virtual objects and GC

Virtual objects are not GC roots.

They become GC-visible only when materialized.

At deopt:

```text
materialization plan reconstructs objects
```

## 19.9 Resource scoping

Do not rely on GC for resources.

```text
DisposableTrait {
  dispose()
}
```

`using` and `defer` lower to deterministic cleanup.

---

# 20. Concurrency model

No GIL.

M:N tasks.

## 20.1 Task system

```text
spawn compute()
```

creates lightweight task.

Tasks mapped to OS threads.

Runtime:

```text
TaskScheduler {
  task_queue
  worker_threads
  work_stealing
  suspension
  resume
  safepoint integration
}
```

## 20.2 Sync policies

```text
SyncPolicy {
  Confined
  Immutable
  AtomicFields
  Locked
  CopyOnWrite
  ChannelOwned
  SharedReadOnly
}
```

Shape carries:

```text
SyncDescriptor {
  policy
  lock_domain
  field_access_modes
  publish_barrier
  sharing_rules
}
```

## 20.3 Object crossing task boundary

```text
if confined:
    promote to shared policy

if immutable:
    safe share

if mutable and syncable:
    attach lock domain

if mutable and not syncable:
    copy, channel-transfer, or reject
```

## 20.4 `sync` blocks

```text
sync obj:
    ...
```

lowers to:

```text
acquire sync_word / monitor
execute region
release
```

JIT can elide locks if it proves:

```text
object is thread-confined
or already inside matching sync domain
or no concurrent publication occurred
```

## 20.5 Task safepoints

Safepoints:

```text
calls
loop backedges
allocations
OSR entries
OSR exits
region entries
region exits
sync acquisition
task spawn
await/suspend points
```

Task-aware safepoint system:

```text
TaskSafepointSystem {
  register_task(task)
  request_safepoint(task)
  suspend_task(task)
  resume_task(task)
  poll_safepoint(task)
}
```

## 20.6 Shape concurrency

```text
OmniShape descriptors immutable
object shape pointer updates atomic
shape registry sharded or lock-free
old shapes readable until epoch reclamation
```

---

# 21. Diagnostics

## 21.1 State snapshots

At errors:

```text
pc
function
registers
local names
object shapes
IC state
quickened state
fused op expansion
exception info
```

## 21.2 Debug descriptor

```text
DebugDescriptor {
  type_name
  property_names
  trait_names
  expected_capabilities
  recent_transitions
  source_origin
  construction_site
}
```

## 21.3 Semantic auto-fixes

Error metadata includes:

```text
missing_property
missing_trait
missing_sync_block
ambiguous_operator_overload
unhandled_disposable
```

Suggestions:

```text
add missing property
add trait
wrap in sync block
dispose resource
use correct coercion
```

## 21.4 REPL recovery

Dev-mode crash can:

```text
pause target task at safe point
materialize virtual objects
capture lock state
capture shape versions
expose local environment
allow mutation
resume with updated state
```

Production default:

```text
disabled
```

---

# 22. Verification gates

Verification is mandatory before activation.

## 22.1 IR verification

```text
types valid
no dangling inputs
phis consistent
control flow sound
memory tokens valid
effect order valid
guards have deopt state
```

## 22.2 Region verification

```text
entry contract satisfied
exit contract satisfied
exception paths valid
memory closure closed
effect closure closed
no duplicated side effects
no unbounded dependencies
```

## 22.3 Machine verification

```text
stack maps valid
GC maps valid
patchpoints valid
entry ABI valid
exit ABI valid
deopt offsets valid
```

## 22.4 Vector verification

```text
vector lane count matches operand packs
element types consistent
memory tokens preserved
no vectorized side effects
no vectorized exception edges unless handled
reduction semantics preserved
GC barriers preserved
alignment facts valid
remainder loop covers tail
deopt state can reconstruct scalar values
```

If verification fails:

```text
discard artifact
fallback
report telemetry
```

---

# 23. Telemetry

Track:

```text
guard failure reason
invalidated dependency key
selected region
dirty node count
region expansion reason
verification failures
fallback reason
partial recompile success rate
deopt-to-reopt latency
code patch failures
deopt loops
method blacklisting
memory/state mismatches
vectorization legality failures
vector speedup
devirtualization miss rate
IC state transitions
shape churn
trait injection rate
operator dispatch stability
compile duration by tier
code size
GC barrier overhead
safepoint latency
task suspension latency
```

Use telemetry for:

```text
adaptive promotion/demotion
blacklisting
shadow-mode validation
performance regression diagnosis
```

---

# 24. Production safety policy

```text
Correct path:
  interpreter/baseline always works

Optimized path:
  may partially recompile if safe
  otherwise full deopt
```

Feature flags:

```text
enable_partial_deopt
enable_partial_loop_recompile
enable_partial_inline_recompile
enable_cross_function_virtualization
enable_micro_pea
enable_swlp
enable_t4
allow_region_patching
partial_deopt_budget
max_dirty_region_size
verification_level
shadow_mode
```

Default early rollout:

```text
partial_deopt = disabled or shadow-only
T4 = disabled
microPEA = disabled
SWLP = conservative
```

Shadow mode:

```text
compute partial plan
verify it
do not activate it
compare with full deopt behavior
```

---

# 25. Implementation order

## Phase 1: Core runtime

```text
semantic bytecode
register interpreter
tagged values
OmniShape basics
GC-safe frames
exceptions
```

## Phase 2: Feedback

```text
site profiles
shape feedback
call feedback
branch counters
allocation counters
inline caches
```

## Phase 3: Quickening

```text
quickened overlay
monomorphic ICs
shape guards
fast arithmetic
fast property access
```

## Phase 4: T1 stencil JIT

```text
generic stencils
specialized stencils
super stencils
interpreter-compatible side exits
GC maps
invalidation
```

## Phase 5: T2 IR optimizer

```text
TIR
dominators
loops
constant folding
GVN
copy prop
DCE
guard insertion
linear scan regalloc
```

## Phase 6: Region model

```text
region recipes
boundary contracts
dependency keys
region artifacts
full deopt
```

## Phase 7: T3 partial deopt

```text
dirty closure
safe region selection
partial rebuild
atomic activation
epoch reclamation
```

## Phase 8: Advanced optimizations

```text
region PEA
cross-function virtualization
micro PEA
devirtualization
loop specialization
SWLP
```

## Phase 9: T4

```text
stable cluster detection
global inlining
global PEA
global guard minimization
whole-cluster regalloc
machine layout optimization
```

---

# 26. Final architecture rule

The full system obeys one rule:

```text
Every optimized artifact is a versioned object
with a stable boundary contract,
complete deopt metadata,
GC-safe maps,
and dependency-tracked assumptions.
```

If the system can prove a transformation is safe:

```text
optimize it
```

If it cannot:

```text
fallback
```

That is the complete, uncompressed design.
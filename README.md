# Omni

An adaptive, speculative, region-based, partial-deoptimizing JIT runtime for the Omni language — a radically dynamic, free-threaded (no GIL), shape-shifting language with M:N tasks, trait injection, universal callability, and deep pattern matching.

## Status

Phase 1 of the [implementation order](docs/architecture/DESIGN.md#25-implementation-order) is in progress. The Tier 0 register interpreter and supporting core libraries compile and pass their initial test suites.

## Building

Requires g++ 14.2+ (or clang++ 18+) with C++26 support.

```bash
make check   # build and run all unit tests
make         # build only
make clean   # remove build artifacts
```

Tests are in `tests/unit/`. Each test is a self-contained binary that exits 0 on success.

## Repository Layout

```
omni/
├── core/                          # Always-correct runtime (Rule 96)
│   ├── common/                    # Shared types: NodeId, SymbolId, SmallVector, Flags, Result
│   ├── object_model/              # OmniShape, TaggedValue, Object, Trait, capability bits
│   ├── bytecode/                  # Semantic/quickened/fused opcodes, 24-bit instruction, verifier
│   └── interpreter/               # Tier 0 register interpreter (DESIGN.md §5)
│       ├── interp_frame.hpp       # §5.1 frame, register file, GC map, sync stack
│       ├── dispatch.hpp           # §5.2 dispatch tables
│       ├── handlers_semantic.*    # One handler per semantic opcode (§4.1)
│       ├── site_profile.hpp      # §5.3 SiteProfile states and fields
│       ├── adaptive_quickening.*  # §5.4 hot/mono/poly rule engine
│       ├── speculative_arithmetic.*  # §5.5 ADD_INT_FAST, ADD_FLOAT_FAST, ...
│       ├── inline_cache.hpp      # §5.6 monomorphic/polymorphic ICs
│       ├── fusion_engine.*        # §5.7 superinstruction fusion
│       └── interpreter_concurrency.hpp  # §5.8 no-GIL, epoch-based invalidation
├── tests/unit/                    # Component-level tests
├── docs/architecture/            # Authoritative spec docs
│   ├── DESIGN.md                  # Full JIT/runtime architecture
│   ├── LAWS.md                    # 150 compiler laws + compliance matrix
│   └── LANGUAGE_FEATURES.md       # User-facing language features
└── Makefile
```

## Design Discipline

Every commit must comply with the [150 compiler laws](docs/architecture/LAWS.md). In particular:

- **Rule 15**: All IR edges are `NodeId` (uint32_t), never raw pointers.
- **Rule 16**: All identifiers are interned `SymbolId` (uint32_t), never `std::string`.
- **Rule 19**: Hot-path vectors with ≤4 elements use `SmallVector<T, N>`.
- **Rule 23**: No magic numbers in logic; all thresholds are named `constexpr`.
- **Rule 32**: All flag-like state uses `Flags<E>` bitmask wrappers.
- **Rule 61**: Hot paths have no allocations, no exceptions, no RTTI, no virtual dispatch.
- **Rule 71**: Every specialization records versioned, invalidatable dependencies.
- **Rule 86**: Every GC reference across a safepoint has a stack map entry.
- **Rule 96**: Tier 0 is the universal correctness fallback. Every feature works in Tier 0.
- **Rule 118**: No GIL on hot paths. Concurrency is fine-grained.

Each major module is its own translation unit (the "each pass gets its own file" rule). Opcode handlers are grouped by tier (`handlers_semantic.*`, `handlers_quickened.*`, `handlers_fused.*`).

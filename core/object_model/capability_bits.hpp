// core/object_model/capability_bits.hpp
//
// Capability bits for OmniShape.
//
// Purpose:
//   Implements DESIGN.md §3.4: capability bits enable fast checks like
//   `if shape.has(CALLABLE): invoke call trait`.
//
// Invariants:
//   - Each enumerator is a distinct power of two (required by Flags<E>).
//   - Bit positions are stable across Omni versions; new capabilities
//     are appended, never reused.
//
// Cross-references:
//   - DESIGN.md §3.4 (capability bits)
//   - LAWS.md Rule 32 (type-safe Flags<E> wrapper)
//   - core/common/flags.hpp

#pragma once

#include "core/common/flags.hpp"

namespace omni::object_model {

enum class Capability : uint32_t {
    None           = 0,
    Callable       = 1u << 0,   // supports obj()
    Iterable       = 1u << 1,   // supports for...in
    Indexable      = 1u << 2,   // supports obj[i]
    Slicable       = 1u << 3,   // supports obj[a:b]
    Hashable       = 1u << 4,   // supports hashing for set/map keys
    Numeric        = 1u << 5,   // supports arithmetic operators
    Stringable     = 1u << 6,   // supports to_str
    Boolable       = 1u << 7,   // supports to_bool
    Matchable      = 1u << 8,   // usable in match patterns
    Disposable     = 1u << 9,   // supports `using`/`defer`
    Syncable       = 1u << 10,  // participates in `sync` blocks
    AsyncCallable  = 1u << 11,  // supports await on call
    Proxylike      = 1u << 12,  // wraps another object via Proxy traps
    Closure        = 1u << 13,  // is a closure (captures free vars)
    Native         = 1u << 14,  // wraps a native (C/Rust) object
    Virtual        = 1u << 15,  // compiler-only virtual object (PEA)
};

using CapabilityBits = common::Flags<Capability>;

}  // namespace omni::object_model

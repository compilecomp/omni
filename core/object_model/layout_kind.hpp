// core/object_model/layout_kind.hpp
//
// Layout kinds for OmniShape and Object payloads.
//
// Purpose:
//   Implements DESIGN.md §3.3: payload kind determines how the runtime
//   accesses fields/elements. The shape's layout_kind is the authoritative
//   selector for which payload union member is valid.
//
// Invariants:
//   - LayoutKind is a stable enum. New layouts are appended; none are reused.
//   - VirtualShape is compiler-only; the runtime never instantiates it.
//
// Cross-references:
//   - DESIGN.md §3.3 (Layout kinds: FixedStruct, FlexibleDictionary, ...)
//   - DESIGN.md §3.1 (Object.payload union)

#pragma once

#include <cstdint>

namespace omni::object_model {

enum class LayoutKind : uint8_t {
    FixedStruct          = 0,  // known slots for known fields
    FlexibleDictionary   = 1,  // dynamic key/value property storage
    DenseArray           = 2,  // contiguous elements
    SparseArray          = 3,  // sparse indexed storage
    ClosureEnv           = 4,  // captured variables
    FunctionObject       = 5,  // executable object with properties
    NativeHandle         = 6,  // external resource/object
    ProxyObject          = 7,  // interception object
    BoxedPrimitive       = 8,  // boxed int/float/bool/string/etc
    VirtualShape         = 9,  // compiler-only (PEA); never instantiated at runtime
};

}  // namespace omni::object_model

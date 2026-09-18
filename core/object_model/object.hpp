// core/object_model/object.hpp
//
// Omni object — header + payload.
//
// Purpose:
//   Implements DESIGN.md §3.1: every object has {header, payload}.
//   The payload union is selected by header.shape_ref->layout_kind().
//
// Invariants:
//   - Every Object starts with an ObjectHeader (fixed 32-byte offset).
//   - The runtime accesses payload via the layout_kind() of the current
//     shape. Direct casting without a layout check is forbidden.
//   - Object lifetime is managed by the GC; user code holds TaggedValue
//     references, not raw Object* pointers (except inside generated JIT
//     code where the GC map tracks them).
//
// Edge cases:
//   - BoxedPrimitive is the layout for boxed ints/floats/bools/strings.
//     Unboxed primitives are TaggedValues directly.
//   - ProxyObject forwards every operation through Proxy traps; the
//     payload is the trap table.
//   - VirtualShape is compiler-only and never instantiated at runtime.
//
// Cross-references:
//   - DESIGN.md §3.1 (Object, ObjectHeader, payload kinds)
//   - LAWS.md Rule 86 (GC references must be tracked)
//   - LAWS.md Rule 78 (object identity survives GC moves)

#pragma once

#include <cstdint>

#include "core/object_model/layout_kind.hpp"
#include "core/object_model/object_header.hpp"
#include "core/object_model/omni_shape.hpp"
#include "core/object_model/tagged_value.hpp"

namespace omni::object_model {

struct FixedStructPayload {
    /// Slot storage. Each slot holds a TaggedValue.
    /// Field name -> slot index mapping is in the OmniShape's PropertyEntry.
    TaggedValue* slots;
    uint32_t slot_count;
};

struct DictionaryPayload {
    /// Hash table of (SymbolId -> TaggedValue). Uses an open-addressing
    /// robin-map per Laws Rule 17.
    void* table;  // tsl::robin_map<SymbolId, TaggedValue>*
    uint32_t entry_count;
};

struct DenseArrayPayload {
    TaggedValue* elements;
    uint32_t length;
    uint32_t capacity;
};

struct SparseArrayPayload {
    void* sparse_map;  // tsl::robin_map<uint64_t, TaggedValue>*
    uint64_t max_index;
};

struct ClosureEnvPayload {
    TaggedValue* captured;
    uint32_t capture_count;
    /// True if any captured variable is shared with another closure;
    /// forces a write barrier on store.
    bool shared;
};

struct FunctionPayload {
    /// BytecodeModule id for this function's body.
    uint32_t module_id;
    /// Function index within the module's function table.
    /// B2-1 fix: was missing; CALL handler hardcoded 0.
    uint32_t function_index;
    /// Entry bytecode pc.
    uint32_t entry_pc;
    /// Parameter count and default-argument table.
    uint16_t param_count;
    uint16_t default_count;
    /// Closure environment (may be null for top-level functions).
    ClosureEnvPayload* env;
};

struct NativeHandlePayload {
    void* handle;
    /// Function pointers for dispose/finalize, called by the GC.
    void (*dispose_fn)(void*);
    void (*finalize_fn)(void*);
    /// ABI tag: 0 = C, 1 = Rust, 2 = C++. Affects calling convention
    /// for FFI calls (Laws Rule 29).
    uint8_t abi_tag;
};

struct ProxyPayload {
    /// Trap table: get, set, has, delete, call, iterate, etc.
    /// Each entry is a TaggedValue (likely a ClosureRef) or null.
    /// Sized by common::PROXY_TRAP_COUNT (Rule 23).
    TaggedValue traps[common::PROXY_TRAP_COUNT];
    /// The target object, if non-null. May be null for fully-virtual
    /// proxies (every operation goes through traps).
    Object* target;
};

struct BoxedPrimitivePayload {
    TaggedValue value;
};

class Object {
public:
    ObjectHeader header;

    /// Payload union. Only the member corresponding to
    /// header.shape_ref.load()->layout_kind() is valid.
    /// All payloads are pointer-sized or larger; this union takes the
    /// largest member.
    union Payload {
        FixedStructPayload fixed_struct;
        DictionaryPayload dict;
        DenseArrayPayload dense_array;
        SparseArrayPayload sparse_array;
        ClosureEnvPayload closure_env;
        FunctionPayload function;
        NativeHandlePayload native;
        ProxyPayload proxy;
        BoxedPrimitivePayload boxed;

        Payload() : fixed_struct{} {}
    } payload;

    /// Convenience: get the current shape.
    [[nodiscard]] OmniShape* shape() const noexcept {
        return header.shape_ref.load(std::memory_order_acquire);
    }

    /// Convenience: layout kind of the current shape.
    [[nodiscard]] LayoutKind layout_kind() const noexcept {
        OmniShape* s = shape();
        return s ? s->layout_kind() : LayoutKind::FixedStruct;
    }

    /// Object identity. Stable across GC moves and shape transitions.
    [[nodiscard]] uint64_t identity() const noexcept {
        return header.identity;
    }
};

}  // namespace omni::object_model

// core/object_model/tagged_value.hpp
//
// Tagged value type for interpreter registers.
//
// Purpose:
//   Implements DESIGN.md §5.1: registers hold tagged values. A TaggedValue
//   is a 16-byte discriminated union covering int, float, string, bool,
//   null, object_ref, closure_ref, and native_handle.
//
//   Tagged fallback must always exist (DESIGN.md §5.1: "Unboxed fast paths
//   are allowed, but tagged fallback must exist"). The Tier 0 interpreter
//   always operates on TaggedValue; JIT tiers may unbox.
//
// Invariants:
//   - TaggedValue is exactly 16 bytes (tag + 8-byte payload, 8-byte aligned).
//   - Tag is in the low 4 bits of the first byte; payload is the remaining
//     15 bytes. NaN-boxing is intentionally avoided for clarity and
//     debuggability; the cost is one byte of overhead.
//   - Discriminated access is via tag() + as_int()/as_float()/etc.
//     Unsafe casts are forbidden; the type switch is exhaustive.
//   - Copying is trivial (POD). Moving is identical to copying.
//
// Edge cases:
//   - NaN, -0.0, and +0.0 are distinct float values; as_float() preserves
//     the exact bit pattern (Laws Rule 72: Omni numeric semantics must be
//     preserved exactly).
//   - Bool and int are distinct tags. `1` (int) and `true` (bool) are
//     not the same value.
//   - null is a separate tag; `null != 0`.
//   - object_ref is an opaque pointer; the runtime owns the object. The
//     interpreter does not dereference it directly — it routes through
//     the object model.
//
// Cross-references:
//   - DESIGN.md §5.1 (TaggedValue union, unboxed fast paths)
//   - LAWS.md Rule 33 (no implicit conversions in IR; explicit nodes)
//   - LAWS.md Rule 72 (numeric semantics preserved exactly)

#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "core/common/types.hpp"

namespace omni::object_model {

// Forward declarations.
class Object;

enum class Tag : uint8_t {
    Null        = 0,
    Bool        = 1,
    Int         = 2,
    Float       = 3,
    Str         = 4,  // interned SymbolId in payload.int_val
    ObjectRef   = 5,
    ClosureRef  = 6,
    NativeHandle = 7,
    // Reserved: 8-15 for future use.
};

/// Tagged value, 16 bytes total. The payload is a union; only the field
/// corresponding to tag() is valid.
class TaggedValue {
public:
    constexpr TaggedValue() noexcept : payload_{}, tag_(Tag::Null) {}

    // --- Constructors for each tag ---
    static constexpr TaggedValue make_null() noexcept {
        return TaggedValue{};
    }
    static constexpr TaggedValue make_bool(bool b) noexcept {
        TaggedValue v;
        v.tag_ = Tag::Bool;
        v.payload_.int_val = b ? 1 : 0;
        return v;
    }
    static constexpr TaggedValue make_int(int64_t i) noexcept {
        TaggedValue v;
        v.tag_ = Tag::Int;
        v.payload_.int_val = i;
        return v;
    }
    static TaggedValue make_float(double f) noexcept {
        TaggedValue v;
        v.tag_ = Tag::Float;
        // Copy bits to avoid any rounding through the union.
        std::memcpy(&v.payload_.float_val, &f, sizeof(double));
        return v;
    }
    static constexpr TaggedValue make_str(common::SymbolId sym) noexcept {
        TaggedValue v;
        v.tag_ = Tag::Str;
        v.payload_.symbol_val = sym;
        return v;
    }
    static TaggedValue make_object(Object* obj) noexcept {
        TaggedValue v;
        v.tag_ = Tag::ObjectRef;
        v.payload_.object_val = obj;
        return v;
    }
    static TaggedValue make_closure(Object* closure) noexcept {
        TaggedValue v;
        v.tag_ = Tag::ClosureRef;
        v.payload_.object_val = closure;
        return v;
    }
    static TaggedValue make_native_handle(void* h) noexcept {
        TaggedValue v;
        v.tag_ = Tag::NativeHandle;
        v.payload_.native_val = h;
        return v;
    }

    [[nodiscard]] constexpr Tag tag() const noexcept { return tag_; }

    [[nodiscard]] constexpr bool is_null() const noexcept { return tag_ == Tag::Null; }
    [[nodiscard]] constexpr bool is_bool() const noexcept { return tag_ == Tag::Bool; }
    [[nodiscard]] constexpr bool is_int() const noexcept { return tag_ == Tag::Int; }
    [[nodiscard]] constexpr bool is_float() const noexcept { return tag_ == Tag::Float; }
    [[nodiscard]] constexpr bool is_str() const noexcept { return tag_ == Tag::Str; }
    [[nodiscard]] constexpr bool is_object_ref() const noexcept { return tag_ == Tag::ObjectRef; }
    [[nodiscard]] constexpr bool is_closure_ref() const noexcept { return tag_ == Tag::ClosureRef; }
    [[nodiscard]] constexpr bool is_native_handle() const noexcept { return tag_ == Tag::NativeHandle; }

    // --- Accessors (rule: caller must check tag first) ---
    [[nodiscard]] constexpr bool as_bool() const noexcept {
        // Caller is responsible for checking is_bool() first.
        return payload_.int_val != 0;
    }
    [[nodiscard]] constexpr int64_t as_int() const noexcept {
        return payload_.int_val;
    }
    [[nodiscard]] double as_float() const noexcept {
        double f;
        std::memcpy(&f, &payload_.float_val, sizeof(double));
        return f;
    }
    [[nodiscard]] constexpr common::SymbolId as_str() const noexcept {
        return payload_.symbol_val;
    }
    [[nodiscard]] constexpr Object* as_object() const noexcept {
        return payload_.object_val;
    }
    [[nodiscard]] constexpr Object* as_closure() const noexcept {
        return payload_.object_val;
    }
    [[nodiscard]] constexpr void* as_native_handle() const noexcept {
        return payload_.native_val;
    }

    /// True if both tag and payload are bitwise equal. Used by tests.
    /// Note: object identity is determined by Object::identity, not by
    /// pointer equality here.
    [[nodiscard]] constexpr bool bitwise_eq(const TaggedValue& other) const noexcept {
        if (tag_ != other.tag_) return false;
        if (tag_ == Tag::Float) {
            // Bitwise compare preserves NaN != NaN (Laws Rule 72).
            uint64_t a, b;
            std::memcpy(&a, &payload_.float_val, sizeof(uint64_t));
            std::memcpy(&b, &other.payload_.float_val, sizeof(uint64_t));
            return a == b;
        }
        return payload_.int_val == other.payload_.int_val;
    }

private:
    // Layout: payload first (8 bytes, 8-byte aligned), then tag (1 byte),
    // then 7 bytes of padding to reach 16 bytes total. Putting the tag
    // after the payload avoids the compiler inserting padding between
    // the 1-byte tag and the 8-byte-aligned payload.
    union Payload {
        constexpr Payload() : int_val(0) {}
        int64_t int_val;
        double float_val;
        common::SymbolId symbol_val;
        Object* object_val;
        void* native_val;
    } payload_;
    Tag tag_;
    uint8_t pad_[7] = {0, 0, 0, 0, 0, 0, 0};
};
static_assert(sizeof(TaggedValue) == 16, "TaggedValue must be 16 bytes");

}  // namespace omni::object_model

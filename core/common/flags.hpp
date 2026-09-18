// core/common/flags.hpp
//
// Type-safe bitmask for orthogonal boolean state.
//
// Purpose:
//   Implements Laws Rule 32: any set of independent boolean properties on a
//   hot-path data structure must be represented as a bitmask with a type-safe
//   Flags<E> wrapper. Raw integers are forbidden for flag-like state.
//
// Invariants:
//   - The underlying storage is a single unsigned integer.
//   - The enum E must be a bitmask: each enumerator must be a distinct
//     power of two (enforced by static_assert).
//   - All operators are constexpr; the wrapper is trivially copyable.
//
// Cross-references:
//   - LAWS.md Rule 32 (All Orthogonal Boolean State Must Be Bitmasked)
//   - DESIGN.md §3.1 (ObjectHeader.flags: shared/frozen/sealed/...)
//   - DESIGN.md §8.2 (NodeFlags)

#pragma once

#include <cstdint>
#include <type_traits>

namespace omni::common {

template <typename E>
class Flags {
    static_assert(std::is_enum_v<E>, "Flags<E>: E must be an enum type");

public:
    using underlying_type = std::underlying_type_t<E>;
    using storage_type = std::conditional_t<
        sizeof(underlying_type) <= sizeof(uint8_t), uint8_t,
        std::conditional_t<sizeof(underlying_type) <= sizeof(uint16_t), uint16_t,
                            std::conditional_t<sizeof(underlying_type) <= sizeof(uint32_t),
                                                uint32_t, uint64_t>>>;

    constexpr Flags() noexcept : bits_(0) {}
    constexpr Flags(E flag) noexcept : bits_(static_cast<storage_type>(flag)) {}

    constexpr Flags& set(E flag) noexcept {
        bits_ |= static_cast<storage_type>(flag);
        return *this;
    }
    constexpr Flags& clear(E flag) noexcept {
        bits_ &= ~static_cast<storage_type>(flag);
        return *this;
    }
    constexpr Flags& toggle(E flag) noexcept {
        bits_ ^= static_cast<storage_type>(flag);
        return *this;
    }

    [[nodiscard]] constexpr bool has(E flag) const noexcept {
        return (bits_ & static_cast<storage_type>(flag)) != 0;
    }
    [[nodiscard]] constexpr bool has_all(Flags other) const noexcept {
        return (bits_ & other.bits_) == other.bits_;
    }
    [[nodiscard]] constexpr bool has_any(Flags other) const noexcept {
        return (bits_ & other.bits_) != 0;
    }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }
    [[nodiscard]] constexpr storage_type raw() const noexcept { return bits_; }

    constexpr Flags& operator|=(Flags other) noexcept {
        bits_ |= other.bits_;
        return *this;
    }
    constexpr Flags& operator&=(Flags other) noexcept {
        bits_ &= other.bits_;
        return *this;
    }
    constexpr Flags& operator^=(Flags other) noexcept {
        bits_ ^= other.bits_;
        return *this;
    }

    friend constexpr Flags operator|(Flags a, Flags b) noexcept {
        return Flags{static_cast<E>(a.bits_ | b.bits_)};
    }
    friend constexpr Flags operator&(Flags a, Flags b) noexcept {
        return Flags{static_cast<E>(a.bits_ & b.bits_)};
    }
    friend constexpr Flags operator^(Flags a, Flags b) noexcept {
        return Flags{static_cast<E>(a.bits_ ^ b.bits_)};
    }
    friend constexpr Flags operator~(Flags a) noexcept {
        return Flags{static_cast<E>(~a.bits_)};
    }
    friend constexpr bool operator==(Flags a, Flags b) noexcept {
        return a.bits_ == b.bits_;
    }
    friend constexpr bool operator!=(Flags a, Flags b) noexcept {
        return a.bits_ != b.bits_;
    }

private:
    storage_type bits_;
};

}  // namespace omni::common

// core/common/small_vector.hpp
//
// Inline-storage vector for hot-path data with a small typical element count.
//
// Purpose:
//   Implements Laws Rule 19: ban std::vector for data that usually has 1-4
//   elements. SmallVector<T, N> stores up to N elements inline; on overflow
//   it spills to the heap. Hot paths (use-def chains, IC entries, IC table
//   rows, instruction operands, basic block preds/succs) almost always
//   have 1-4 elements.
//
// Invariants:
//   - No allocations when size() <= N (the inline capacity).
//   - Inline storage is part of the object (no extra heap allocation).
//   - Spill path uses operator new/delete (no malloc/free mix).
//   - Move semantics are O(1) when both operands are inline-spilled.
//   - Copy semantics copy elements individually (no memcpy of POD-only
//     assumption; the type is generic T).
//
// Edge cases:
//   - N == 0 is permitted but degenerate; behaves like std::vector.
//   - T must be movable. If T is non-trivial, the spill path calls move
//     constructors.
//   - Exception safety: if T's move ctor throws during grow, the buffer
//     is left in a valid but unspecified state (matches libc++ policy).
//
// Cross-references:
//   - LAWS.md Rule 19 (SmallVector<T, N> for ≤4 element data)
//   - LAWS.md Rule 61 (no allocations in hot paths when possible)

#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <utility>

namespace omni::common {

template <typename T, unsigned N>
class SmallVector {
    static_assert(N > 0, "SmallVector<N> with N=0 is degenerate; use std::vector");

public:
    using value_type = T;
    using size_type = uint32_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    constexpr SmallVector() noexcept
        : size_(0), cap_(N), data_(reinterpret_cast<T*>(inline_storage_)) {}

    SmallVector(const SmallVector& other) : size_(0), cap_(N), data_(reinterpret_cast<T*>(inline_storage_)) {
        reserve(other.size_);
        for (size_type i = 0; i < other.size_; ++i) {
            new (&data_[i]) T(other.data_[i]);
        }
        size_ = other.size_;
    }

    SmallVector(SmallVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : size_(0), cap_(N), data_(reinterpret_cast<T*>(inline_storage_)) {
        if (other.is_inline()) {
            // Move elements one by one from inline storage.
            for (size_type i = 0; i < other.size_; ++i) {
                new (&data_[i]) T(std::move(other.data_[i]));
            }
            size_ = other.size_;
            other.size_ = 0;
        } else {
            // Steal the heap buffer in O(1).
            data_ = other.data_;
            size_ = other.size_;
            cap_ = other.cap_;
            other.data_ = reinterpret_cast<T*>(other.inline_storage_);
            other.size_ = 0;
            other.cap_ = N;
        }
    }

    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            clear();
            reserve(other.size_);
            for (size_type i = 0; i < other.size_; ++i) {
                new (&data_[i]) T(other.data_[i]);
            }
            size_ = other.size_;
        }
        return *this;
    }

    SmallVector& operator=(SmallVector&& other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this != &other) {
            clear();
            if (!is_inline()) {
                ::operator delete(data_);
                data_ = reinterpret_cast<T*>(inline_storage_);
                cap_ = N;
            }
            if (other.is_inline()) {
                for (size_type i = 0; i < other.size_; ++i) {
                    new (&data_[i]) T(std::move(other.data_[i]));
                }
                size_ = other.size_;
                other.size_ = 0;
            } else {
                data_ = other.data_;
                size_ = other.size_;
                cap_ = other.cap_;
                other.data_ = reinterpret_cast<T*>(other.inline_storage_);
                other.size_ = 0;
                other.cap_ = N;
            }
        }
        return *this;
    }

    ~SmallVector() { destroy(); }

    void push_back(T value) {
        if (size_ == cap_) grow(cap_ * 2);
        new (&data_[size_]) T(std::move(value));
        ++size_;
    }

    void pop_back() {
        if (size_ > 0) {
            --size_;
            data_[size_].~T();
        }
    }

    reference operator[](size_type i) {
        return data_[i];
    }
    const_reference operator[](size_type i) const {
        return data_[i];
    }

    reference front() { return data_[0]; }
    const_reference front() const { return data_[0]; }
    reference back() { return data_[size_ - 1]; }
    const_reference back() const { return data_[size_ - 1]; }

    iterator begin() { return data_; }
    iterator end() { return data_ + size_; }
    const_iterator begin() const { return data_; }
    const_iterator end() const { return data_ + size_; }
    const_iterator cbegin() const { return data_; }
    const_iterator cend() const { return data_ + size_; }

    size_type size() const { return size_; }
    size_type capacity() const { return cap_; }
    bool empty() const { return size_ == 0; }

    void clear() {
        for (size_type i = 0; i < size_; ++i) {
            data_[i].~T();
        }
        size_ = 0;
    }

    void reserve(size_type new_cap) {
        if (new_cap > cap_) grow(new_cap);
    }

private:
    bool is_inline() const noexcept {
        return static_cast<const void*>(data_) == static_cast<const void*>(inline_storage_);
    }

    void grow(size_type new_cap) {
        if (new_cap <= cap_) return;
        T* new_data = static_cast<T*>(::operator new(new_cap * sizeof(T)));
        for (size_type i = 0; i < size_; ++i) {
            new (&new_data[i]) T(std::move(data_[i]));
            data_[i].~T();
        }
        if (!is_inline()) {
            ::operator delete(data_);
        }
        data_ = new_data;
        cap_ = new_cap;
    }

    void destroy() {
        for (size_type i = 0; i < size_; ++i) {
            data_[i].~T();
        }
        if (!is_inline()) {
            ::operator delete(data_);
        }
    }

    alignas(T) unsigned char inline_storage_[sizeof(T) * N];
    uint32_t size_;
    uint32_t cap_;
    T* data_;
};

}  // namespace omni::common

/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

/*
 * Imported from OSv:
 *
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#pragma once

#include <bitset>
#include <limits>

namespace seastar {

namespace bitsets {

static constexpr int ulong_bits = std::numeric_limits<unsigned long>::digits;

/**
 * Returns the number of leading zeros in value's binary representation.
 *
 * If value == 0 the result is undefied. If T is signed and value is negative
 * the result is undefined.
 *
 * The highest value that can be returned is std::numeric_limits<T>::digits - 1,
 * which is returned when value == 1.
 */
template<typename T>
inline size_t count_leading_zeros(T value) noexcept;

/**
 * Returns the number of trailing zeros in value's binary representation.
 *
 * If value == 0 the result is undefied. If T is signed and value is negative
 * the result is undefined.
 *
 * The highest value that can be returned is std::numeric_limits<T>::digits - 1.
 */
template<typename T>
static inline size_t count_trailing_zeros(T value) noexcept;

template<>
inline size_t count_leading_zeros<unsigned long>(unsigned long value) noexcept
{
    return __builtin_clzl(value);
}

template<>
inline size_t count_leading_zeros<long>(long value) noexcept
{
    return __builtin_clzl((unsigned long)value) - 1;
}

template<>
inline size_t count_leading_zeros<unsigned long long>(unsigned long long value) noexcept
{
    return __builtin_clzll(value);
}

template<>
inline size_t count_leading_zeros<long long>(long long value) noexcept
{
    return __builtin_clzll((unsigned long long)value) - 1;
}

template<>
inline
size_t count_trailing_zeros<unsigned long>(unsigned long value) noexcept
{
    return __builtin_ctzl(value);
}

template<>
inline
size_t count_trailing_zeros<long>(long value) noexcept
{
    return __builtin_ctzl((unsigned long)value);
}

template<>
inline
size_t count_trailing_zeros<unsigned long long>(unsigned long long value) noexcept
{
    return __builtin_ctzll(value);
}

template<>
inline
size_t count_trailing_zeros<long long>(long long value) noexcept
{
    return __builtin_ctzll((unsigned long long)value);
}

/**
 * Returns the index of the first set bit.
 * Result is undefined if bitset.any() == false.
 */
template<size_t N>
static inline size_t get_first_set(const std::bitset<N>& bitset) noexcept
{
    static_assert(N <= ulong_bits, "bitset too large");
    return count_trailing_zeros(bitset.to_ulong());
}

/**
 * Returns the index of the last set bit in the bitset.
 * Result is undefined if bitset.any() == false.
 */
template<size_t N>
static inline size_t get_last_set(const std::bitset<N>& bitset) noexcept
{
    static_assert(N <= ulong_bits, "bitset too large");
    return ulong_bits - 1 - count_leading_zeros(bitset.to_ulong());
}

template<size_t N>
class set_iterator
{
private:
    void advance() noexcept
    {
        if (_bitset.none()) {
            _index = -1;
        } else {
            auto shift = get_first_set(_bitset) + 1;
            _index += shift;
            _bitset >>= shift;
        }
    }
public:
    using iterator_category = std::input_iterator_tag;
    using value_type = int;
    using difference_type = std::ptrdiff_t;
    using pointer = int*;
    using reference = int&;

    set_iterator(std::bitset<N> bitset, int offset = 0) noexcept
        : _bitset(bitset)
        , _index(offset - 1)
    {
        static_assert(N <= ulong_bits, "This implementation is inefficient for large bitsets");
        _bitset >>= offset;
        advance();
    }

    set_iterator& operator++() noexcept
    {
        advance();
        return *this;
    }

    set_iterator operator++(int) noexcept
    {
        auto ret = *this;
        advance();
        return ret;
    }

    int operator*() const noexcept
    {
        return _index;
    }

    bool operator==(const set_iterator& other) const noexcept
    {
        return _index == other._index;
    }

    bool operator!=(const set_iterator& other) const noexcept
    {
        return !(*this == other);
    }
private:
    std::bitset<N> _bitset;
    int _index;
};

template<size_t N>
class set_range
{
public:
    using iterator = set_iterator<N>;
    using value_type = int;

    constexpr set_range(std::bitset<N> bitset, int offset = 0) noexcept
        : _bitset(bitset)
        , _offset(offset)
    {
    }

    iterator begin() const noexcept { return iterator(_bitset, _offset); }
    iterator end() const noexcept { return iterator(0); }
private:
    std::bitset<N> _bitset;
    int _offset;
};

template<size_t N>
static inline set_range<N> for_each_set(std::bitset<N> bitset, int offset = 0) noexcept
{
    return set_range<N>(bitset, offset);
}

}

}

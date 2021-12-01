/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
/*
 * Copyright (C) 2017 ScyllaDB
 */

#pragma once

// A fixed capacity double-ended queue container that can be efficiently
// extended (and shrunk) from both ends.  Implementation is a single
// storage vector.
//
// Similar to libstdc++'s std::deque, except that it uses a single level
// store, and so is more efficient for simple stored items.

#include <type_traits>
#include <cstddef>
#include <iterator>
#include <utility>


/// \file

namespace seastar {

/// A fixed-capacity container (like boost::static_vector) that can insert
/// and remove at both ends (like std::deque). Does not allocate.
///
/// Does not perform overflow checking when size exceeds capacity.
///
/// \tparam T type of objects stored in the container; must be noexcept move enabled
/// \tparam Capacity maximum number of objects that can be stored in the container; must be a power of 2
template <typename T, size_t Capacity>
class circular_buffer_fixed_capacity {
    size_t _begin = 0;
    size_t _end = 0;
    union maybe_storage {
        T data;
        maybe_storage() noexcept {}
        ~maybe_storage() {}
    };
    maybe_storage _storage[Capacity];
private:
    static size_t mask(size_t idx) { return idx % Capacity; }
    T* obj(size_t idx) { return &_storage[mask(idx)].data; }
    const T* obj(size_t idx) const { return &_storage[mask(idx)].data; }
public:
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static_assert(std::is_nothrow_move_constructible<T>::value && std::is_nothrow_move_assignable<T>::value,
            "circular_buffer_fixed_capacity only supports nothrow-move value types");
    using value_type = T;
    using size_type = size_t;
    using reference = T&;
    using pointer = T*;
    using const_reference = const T&;
    using const_pointer = const T*;
    using difference_type = ssize_t;
public:
    template <typename ValueType>
    class cbiterator {
        using holder = std::conditional_t<std::is_const<ValueType>::value, const maybe_storage, maybe_storage>;
        holder* _start;
        size_t _idx;
    private:
        cbiterator(holder* start, size_t idx) noexcept : _start(start), _idx(idx) {}
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = ValueType;
        using difference_type = ssize_t;
        using pointer = ValueType*;
        using reference = ValueType&;
    public:
        cbiterator();
        ValueType& operator*() const { return _start[mask(_idx)].data; }
        ValueType* operator->() const { return &operator*(); }
        // prefix
        cbiterator& operator++() {
            ++_idx;
            return *this;
        }
        // postfix
        cbiterator operator++(int) {
            auto v = *this;
            ++_idx;
            return v;
        }
        // prefix
        cbiterator& operator--() {
            --_idx;
            return *this;
        }
        // postfix
        cbiterator operator--(int) {
            auto v = *this;
            --_idx;
            return v;
        }
        cbiterator operator+(difference_type n) const {
            return cbiterator{_start, _idx + n};
        }
        friend cbiterator operator+(difference_type n, cbiterator i) {
            return i + n;
        }
        cbiterator operator-(difference_type n) const {
            return cbiterator{_start, _idx - n};
        }
        cbiterator& operator+=(difference_type n) {
            _idx += n;
            return *this;
        }
        cbiterator& operator-=(difference_type n) {
            _idx -= n;
            return *this;
        }
        bool operator==(const cbiterator& rhs) const {
            return _idx == rhs._idx;
        }
        bool operator!=(const cbiterator& rhs) const {
            return _idx != rhs._idx;
        }
        bool operator<(const cbiterator& rhs) const {
            return ssize_t(_idx - rhs._idx) < 0;
        }
        bool operator>(const cbiterator& rhs) const {
            return ssize_t(_idx - rhs._idx) > 0;
        }
        bool operator<=(const cbiterator& rhs) const {
            return ssize_t(_idx - rhs._idx) <= 0;
        }
        bool operator>=(const cbiterator& rhs) const {
            return ssize_t(_idx - rhs._idx) >= 0;
        }
        difference_type operator-(const cbiterator& rhs) const {
            return _idx - rhs._idx;
        }
        friend class circular_buffer_fixed_capacity;
    };
public:
    using iterator = cbiterator<T>;
    using const_iterator = cbiterator<const T>;
public:
    circular_buffer_fixed_capacity() = default;
    circular_buffer_fixed_capacity(circular_buffer_fixed_capacity&& x) noexcept;
    ~circular_buffer_fixed_capacity();
    circular_buffer_fixed_capacity& operator=(circular_buffer_fixed_capacity&& x) noexcept;
    void push_front(const T& data);
    void push_front(T&& data);
    template <typename... A>
    T& emplace_front(A&&... args);
    void push_back(const T& data);
    void push_back(T&& data);
    template <typename... A>
    T& emplace_back(A&&... args);
    T& front();
    T& back();
    void pop_front();
    void pop_back();
    bool empty() const;
    size_t size() const;
    size_t capacity() const;
    T& operator[](size_t idx);
    void clear();
    iterator begin() {
        return iterator(_storage, _begin);
    }
    const_iterator begin() const {
        return const_iterator(_storage, _begin);
    }
    iterator end() {
        return iterator(_storage, _end);
    }
    const_iterator end() const {
        return const_iterator(_storage, _end);
    }
    const_iterator cbegin() const {
        return const_iterator(_storage, _begin);
    }
    const_iterator cend() const {
        return const_iterator(_storage, _end);
    }
    iterator erase(iterator first, iterator last);
};

template <typename T, size_t Capacity>
inline
bool
circular_buffer_fixed_capacity<T, Capacity>::empty() const {
    return _begin == _end;
}

template <typename T, size_t Capacity>
inline
size_t
circular_buffer_fixed_capacity<T, Capacity>::size() const {
    return _end - _begin;
}

template <typename T, size_t Capacity>
inline
size_t
circular_buffer_fixed_capacity<T, Capacity>::capacity() const {
    return Capacity;
}

template <typename T, size_t Capacity>
inline
circular_buffer_fixed_capacity<T, Capacity>::circular_buffer_fixed_capacity(circular_buffer_fixed_capacity&& x) noexcept
        : _begin(x._begin), _end(x._end) {
    // This is std::uninitialized_move, but that is c++17 only
    auto dest = begin();
    for (auto& obj : x) {
        new (&*dest++) T(std::move(obj));
    }
}

template <typename T, size_t Capacity>
inline
circular_buffer_fixed_capacity<T, Capacity>&
circular_buffer_fixed_capacity<T, Capacity>::operator=(circular_buffer_fixed_capacity&& x) noexcept {
    if (this != &x) {
        this->~circular_buffer_fixed_capacity();
        new (this) circular_buffer_fixed_capacity(std::move(x));
    }
    return *this;
}

template <typename T, size_t Capacity>
inline
circular_buffer_fixed_capacity<T, Capacity>::~circular_buffer_fixed_capacity() {
    clear();
}

template <typename T, size_t Capacity>
inline
void
circular_buffer_fixed_capacity<T, Capacity>::push_front(const T& data) {
    new (obj(_begin - 1)) T(data);
    --_begin;
}

template <typename T, size_t Capacity>
inline
void
circular_buffer_fixed_capacity<T, Capacity>::push_front(T&& data) {
    new (obj(_begin - 1)) T(std::move(data));
    --_begin;
}

template <typename T, size_t Capacity>
template <typename... Args>
inline
T&
circular_buffer_fixed_capacity<T, Capacity>::emplace_front(Args&&... args) {
    auto p = new (obj(_begin - 1)) T(std::forward<Args>(args)...);
    --_begin;
    return *p;
}

template <typename T, size_t Capacity>
inline
void
circular_buffer_fixed_capacity<T, Capacity>::push_back(const T& data) {
    new (obj(_end)) T(data);
    ++_end;
}

template <typename T, size_t Capacity>
inline
void
circular_buffer_fixed_capacity<T, Capacity>::push_back(T&& data) {
    new (obj(_end)) T(std::move(data));
    ++_end;
}

template <typename T, size_t Capacity>
template <typename... Args>
inline
T&
circular_buffer_fixed_capacity<T, Capacity>::emplace_back(Args&&... args) {
    auto p = new (obj(_end)) T(std::forward<Args>(args)...);
    ++_end;
    return *p;
}

template <typename T, size_t Capacity>
inline
T&
circular_buffer_fixed_capacity<T, Capacity>::front() {
    return *obj(_begin);
}

template <typename T, size_t Capacity>
inline
T&
circular_buffer_fixed_capacity<T, Capacity>::back() {
    return *obj(_end - 1);
}

template <typename T, size_t Capacity>
inline
void
circular_buffer_fixed_capacity<T, Capacity>::pop_front() {
    obj(_begin)->~T();
    ++_begin;
}

template <typename T, size_t Capacity>
inline
void
circular_buffer_fixed_capacity<T, Capacity>::pop_back() {
    obj(_end - 1)->~T();
    --_end;
}

template <typename T, size_t Capacity>
inline
T&
circular_buffer_fixed_capacity<T, Capacity>::operator[](size_t idx) {
    return *obj(_begin + idx);
}

template <typename T, size_t Capacity>
inline
typename circular_buffer_fixed_capacity<T, Capacity>::iterator
circular_buffer_fixed_capacity<T, Capacity>::erase(iterator first, iterator last) {
    static_assert(std::is_nothrow_move_assignable<T>::value, "erase() assumes move assignment does not throw");
    if (first == last) {
        return last;
    }
    // Move to the left or right depending on which would result in least amount of moves.
    // This also guarantees that iterators will be stable when removing from either front or back.
    if (std::distance(begin(), first) < std::distance(last, end())) {
        auto new_start = std::move_backward(begin(), first, last);
        auto i = begin();
        while (i < new_start) {
            *i++.~T();
        }
        _begin = new_start.idx;
        return last;
    } else {
        auto new_end = std::move(last, end(), first);
        auto i = new_end;
        auto e = end();
        while (i < e) {
            *i++.~T();
        }
        _end = new_end.idx;
        return first;
    }
}

template <typename T, size_t Capacity>
inline
void
circular_buffer_fixed_capacity<T, Capacity>::clear() {
    for (auto& obj : *this) {
        obj.~T();
    }
    _begin = _end = 0;
}

}


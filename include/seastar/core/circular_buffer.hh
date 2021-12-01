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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/core/transfer.hh>
#include <seastar/core/bitops.hh>
#include <seastar/util/concepts.hh>
#include <memory>
#include <algorithm>

namespace seastar {

/// A growable double-ended queue container that can be efficiently
/// extended (and shrunk) from both ends. Implementation is a single
/// storage vector.
///
/// Similar to libstdc++'s std::deque, except that it uses a single
/// level store, and so is more efficient for simple stored items.
/// Similar to boost::circular_buffer_space_optimized, except it uses
/// uninitialized storage for unoccupied elements (and thus move/copy
/// constructors instead of move/copy assignments, which are less
/// efficient).
///
/// The storage of the circular_buffer is expanded automatically in
/// exponential increments.
/// When adding new elements:
/// * if size + 1 > capacity: all iterators and references are
///     invalidated,
/// * otherwise only the begin() or end() iterator is invalidated:
///     * push_front() and emplace_front() will invalidate begin() and
///     * push_back() and emplace_back() will invalidate end().
///
/// Removing elements never invalidates any references and only
/// invalidates begin() or end() iterators:
///     * pop_front() will invalidate begin() and
///     * pop_back() will invalidate end().
///
/// reserve() may also invalidate all iterators and references.
template <typename T, typename Alloc = std::allocator<T>>
class circular_buffer {
    struct impl : Alloc {
        T* storage = nullptr;
        // begin, end interpreted (mod capacity)
        size_t begin = 0;
        size_t end = 0;
        size_t capacity = 0;

        impl(Alloc a) noexcept : Alloc(std::move(a)) { }
        void reset() {
            storage = {};
            begin = 0;
            end = 0;
            capacity = 0;
        }
    };
    static_assert(!std::is_default_constructible_v<Alloc>
                  || std::is_nothrow_default_constructible_v<Alloc>);
    static_assert(std::is_nothrow_move_constructible_v<Alloc>);
    impl _impl;
public:
    using value_type = T;
    using size_type = size_t;
    using reference = T&;
    using pointer = T*;
    using const_reference = const T&;
    using const_pointer = const T*;
public:
    circular_buffer() noexcept SEASTAR_CONCEPT(requires std::default_initializable<Alloc>) : circular_buffer(Alloc()) {}
    circular_buffer(Alloc alloc) noexcept;
    circular_buffer(circular_buffer&& X) noexcept;
    circular_buffer(const circular_buffer& X) = delete;
    ~circular_buffer();
    circular_buffer& operator=(const circular_buffer&) = delete;
    circular_buffer& operator=(circular_buffer&& b) noexcept;
    void push_front(const T& data);
    void push_front(T&& data);
    template <typename... A>
    void emplace_front(A&&... args);
    void push_back(const T& data);
    void push_back(T&& data);
    template <typename... A>
    void emplace_back(A&&... args);
    T& front() noexcept;
    const T& front() const noexcept;
    T& back() noexcept;
    const T& back() const noexcept;
    void pop_front() noexcept;
    void pop_back() noexcept;
    bool empty() const noexcept;
    size_t size() const noexcept;
    size_t capacity() const noexcept;
    void reserve(size_t);
    void clear() noexcept;
    T& operator[](size_t idx) noexcept;
    const T& operator[](size_t idx) const noexcept;
    template <typename Func>
    void for_each(Func func);
    // access an element, may return wrong or destroyed element
    // only useful if you do not rely on data accuracy (e.g. prefetch)
    T& access_element_unsafe(size_t idx) noexcept;
private:
    void expand();
    void expand(size_t);
    void maybe_expand(size_t nr = 1);
    size_t mask(size_t idx) const;

    template<typename CB, typename ValueType>
    struct cbiterator : std::iterator<std::random_access_iterator_tag, ValueType> {
        typedef std::iterator<std::random_access_iterator_tag, ValueType> super_t;

        ValueType& operator*() const noexcept { return cb->_impl.storage[cb->mask(idx)]; }
        ValueType* operator->() const noexcept { return &cb->_impl.storage[cb->mask(idx)]; }
        // prefix
        cbiterator<CB, ValueType>& operator++() noexcept {
            idx++;
            return *this;
        }
        // postfix
        cbiterator<CB, ValueType> operator++(int unused) noexcept {
            auto v = *this;
            idx++;
            return v;
        }
        // prefix
        cbiterator<CB, ValueType>& operator--() noexcept {
            idx--;
            return *this;
        }
        // postfix
        cbiterator<CB, ValueType> operator--(int unused) noexcept {
            auto v = *this;
            idx--;
            return v;
        }
        cbiterator<CB, ValueType> operator+(typename super_t::difference_type n) const noexcept {
            return cbiterator<CB, ValueType>(cb, idx + n);
        }
        cbiterator<CB, ValueType> operator-(typename super_t::difference_type n) const noexcept {
            return cbiterator<CB, ValueType>(cb, idx - n);
        }
        cbiterator<CB, ValueType>& operator+=(typename super_t::difference_type n) noexcept {
            idx += n;
            return *this;
        }
        cbiterator<CB, ValueType>& operator-=(typename super_t::difference_type n) noexcept {
            idx -= n;
            return *this;
        }
        bool operator==(const cbiterator<CB, ValueType>& rhs) const noexcept {
            return idx == rhs.idx;
        }
        bool operator!=(const cbiterator<CB, ValueType>& rhs) const noexcept {
            return idx != rhs.idx;
        }
        bool operator<(const cbiterator<CB, ValueType>& rhs) const noexcept {
            return idx < rhs.idx;
        }
        bool operator>(const cbiterator<CB, ValueType>& rhs) const noexcept {
            return idx > rhs.idx;
        }
        bool operator>=(const cbiterator<CB, ValueType>& rhs) const noexcept {
            return idx >= rhs.idx;
        }
        bool operator<=(const cbiterator<CB, ValueType>& rhs) const noexcept {
            return idx <= rhs.idx;
        }
       typename super_t::difference_type operator-(const cbiterator<CB, ValueType>& rhs) const noexcept {
            return idx - rhs.idx;
        }
    private:
        CB* cb;
        size_t idx;
        cbiterator(CB* b, size_t i) noexcept : cb(b), idx(i) {}
        friend class circular_buffer;
    };
    friend class iterator;

public:
    typedef cbiterator<circular_buffer, T> iterator;
    typedef cbiterator<const circular_buffer, const T> const_iterator;

    iterator begin() noexcept {
        return iterator(this, _impl.begin);
    }
    const_iterator begin() const noexcept {
        return const_iterator(this, _impl.begin);
    }
    iterator end() noexcept {
        return iterator(this, _impl.end);
    }
    const_iterator end() const noexcept {
        return const_iterator(this, _impl.end);
    }
    const_iterator cbegin() const noexcept {
        return const_iterator(this, _impl.begin);
    }
    const_iterator cend() const noexcept {
        return const_iterator(this, _impl.end);
    }
    iterator erase(iterator first, iterator last) noexcept;
};

template <typename T, typename Alloc>
inline
size_t
circular_buffer<T, Alloc>::mask(size_t idx) const {
    return idx & (_impl.capacity - 1);
}

template <typename T, typename Alloc>
inline
bool
circular_buffer<T, Alloc>::empty() const noexcept {
    return _impl.begin == _impl.end;
}

template <typename T, typename Alloc>
inline
size_t
circular_buffer<T, Alloc>::size() const noexcept {
    return _impl.end - _impl.begin;
}

template <typename T, typename Alloc>
inline
size_t
circular_buffer<T, Alloc>::capacity() const noexcept {
    return _impl.capacity;
}

template <typename T, typename Alloc>
inline
void
circular_buffer<T, Alloc>::reserve(size_t size) {
    if (capacity() < size) {
        // Make sure that the new capacity is a power of two.
        expand(size_t(1) << log2ceil(size));
    }
}

template <typename T, typename Alloc>
inline
void
circular_buffer<T, Alloc>::clear() noexcept {
    erase(begin(), end());
}

template <typename T, typename Alloc>
inline
circular_buffer<T, Alloc>::circular_buffer(Alloc alloc) noexcept
    : _impl(std::move(alloc)) {
}

template <typename T, typename Alloc>
inline
circular_buffer<T, Alloc>::circular_buffer(circular_buffer&& x) noexcept
    : _impl(std::move(x._impl)) {
    x._impl.reset();
}

template <typename T, typename Alloc>
inline
circular_buffer<T, Alloc>& circular_buffer<T, Alloc>::operator=(circular_buffer&& x) noexcept {
    if (this != &x) {
        this->~circular_buffer();
        new (this) circular_buffer(std::move(x));
    }
    return *this;
}

template <typename T, typename Alloc>
template <typename Func>
inline
void
circular_buffer<T, Alloc>::for_each(Func func) {
    auto s = _impl.storage;
    auto m = _impl.capacity - 1;
    for (auto i = _impl.begin; i != _impl.end; ++i) {
        func(s[i & m]);
    }
}

template <typename T, typename Alloc>
inline
circular_buffer<T, Alloc>::~circular_buffer() {
    for_each([this] (T& obj) {
        std::allocator_traits<Alloc>::destroy(_impl, &obj);
    });
    _impl.deallocate(_impl.storage, _impl.capacity);
}

template <typename T, typename Alloc>
void
circular_buffer<T, Alloc>::expand() {
    expand(std::max<size_t>(_impl.capacity * 2, 1));
}

template <typename T, typename Alloc>
void
circular_buffer<T, Alloc>::expand(size_t new_cap) {
    auto new_storage = _impl.allocate(new_cap);
    auto p = new_storage;
    try {
        for_each([this, &p] (T& obj) {
            transfer_pass1(_impl, &obj, p);
            p++;
        });
    } catch (...) {
        while (p != new_storage) {
            std::allocator_traits<Alloc>::destroy(_impl, --p);
        }
        _impl.deallocate(new_storage, new_cap);
        throw;
    }
    p = new_storage;
    for_each([this, &p] (T& obj) {
        transfer_pass2(_impl, &obj, p++);
    });
    std::swap(_impl.storage, new_storage);
    std::swap(_impl.capacity, new_cap);
    _impl.begin = 0;
    _impl.end = p - _impl.storage;
    _impl.deallocate(new_storage, new_cap);
}

template <typename T, typename Alloc>
inline
void
circular_buffer<T, Alloc>::maybe_expand(size_t nr) {
    if (_impl.end - _impl.begin + nr > _impl.capacity) {
        expand();
    }
}

template <typename T, typename Alloc>
inline
void
circular_buffer<T, Alloc>::push_front(const T& data) {
    maybe_expand();
    auto p = &_impl.storage[mask(_impl.begin - 1)];
    std::allocator_traits<Alloc>::construct(_impl, p, data);
    --_impl.begin;
}

template <typename T, typename Alloc>
inline
void
circular_buffer<T, Alloc>::push_front(T&& data) {
    maybe_expand();
    auto p = &_impl.storage[mask(_impl.begin - 1)];
    std::allocator_traits<Alloc>::construct(_impl, p, std::move(data));
    --_impl.begin;
}

template <typename T, typename Alloc>
template <typename... Args>
inline
void
circular_buffer<T, Alloc>::emplace_front(Args&&... args) {
    maybe_expand();
    auto p = &_impl.storage[mask(_impl.begin - 1)];
    std::allocator_traits<Alloc>::construct(_impl, p, std::forward<Args>(args)...);
    --_impl.begin;
}

template <typename T, typename Alloc>
inline
void
circular_buffer<T, Alloc>::push_back(const T& data) {
    maybe_expand();
    auto p = &_impl.storage[mask(_impl.end)];
    std::allocator_traits<Alloc>::construct(_impl, p, data);
    ++_impl.end;
}

template <typename T, typename Alloc>
inline
void
circular_buffer<T, Alloc>::push_back(T&& data) {
    maybe_expand();
    auto p = &_impl.storage[mask(_impl.end)];
    std::allocator_traits<Alloc>::construct(_impl, p, std::move(data));
    ++_impl.end;
}

template <typename T, typename Alloc>
template <typename... Args>
inline
void
circular_buffer<T, Alloc>::emplace_back(Args&&... args) {
    maybe_expand();
    auto p = &_impl.storage[mask(_impl.end)];
    std::allocator_traits<Alloc>::construct(_impl, p, std::forward<Args>(args)...);
    ++_impl.end;
}

template <typename T, typename Alloc>
inline
T&
circular_buffer<T, Alloc>::front() noexcept {
    return _impl.storage[mask(_impl.begin)];
}

template <typename T, typename Alloc>
inline
const T&
circular_buffer<T, Alloc>::front() const noexcept {
    return _impl.storage[mask(_impl.begin)];
}

template <typename T, typename Alloc>
inline
T&
circular_buffer<T, Alloc>::back() noexcept {
    return _impl.storage[mask(_impl.end - 1)];
}

template <typename T, typename Alloc>
inline
const T&
circular_buffer<T, Alloc>::back() const noexcept {
    return _impl.storage[mask(_impl.end - 1)];
}

template <typename T, typename Alloc>
inline
void
circular_buffer<T, Alloc>::pop_front() noexcept {
    std::allocator_traits<Alloc>::destroy(_impl, &front());
    ++_impl.begin;
}

template <typename T, typename Alloc>
inline
void
circular_buffer<T, Alloc>::pop_back() noexcept {
    std::allocator_traits<Alloc>::destroy(_impl, &back());
    --_impl.end;
}

template <typename T, typename Alloc>
inline
T&
circular_buffer<T, Alloc>::operator[](size_t idx) noexcept {
    return _impl.storage[mask(_impl.begin + idx)];
}

template <typename T, typename Alloc>
inline
const T&
circular_buffer<T, Alloc>::operator[](size_t idx) const noexcept {
    return _impl.storage[mask(_impl.begin + idx)];
}

template <typename T, typename Alloc>
inline
T&
circular_buffer<T, Alloc>::access_element_unsafe(size_t idx) noexcept {
    return _impl.storage[mask(_impl.begin + idx)];
}

template <typename T, typename Alloc>
inline
typename circular_buffer<T, Alloc>::iterator
circular_buffer<T, Alloc>::erase(iterator first, iterator last) noexcept {
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
            std::allocator_traits<Alloc>::destroy(_impl, &*i++);
        }
        _impl.begin = new_start.idx;
        return last;
    } else {
        auto new_end = std::move(last, end(), first);
        auto i = new_end;
        auto e = end();
        while (i < e) {
            std::allocator_traits<Alloc>::destroy(_impl, &*i++);
        }
        _impl.end = new_end.idx;
        return first;
    }
}

}

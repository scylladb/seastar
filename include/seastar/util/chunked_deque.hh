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
 * Copyright (C) 2022-present ScyllaDB Ltd.
 */

#pragma once

#include <memory>

#include <seastar/core/on_internal_error.hh>

namespace seastar {

/// \brief An unbounded double-ended queue of objects of type T.
///
/// It provides operations to push items in both ends of the queue, and pop them
/// from either end of the queue - both operations are guaranteed O(1)
/// (not just amortized O(1)). The size() operation is also O(1).
/// chunked_dequeue also guarantees that the largest contiguous memory allocation
/// it does is O(1). The total memory used is, of course, O(N).
///
template <typename T, size_t items_per_chunk = 128, int save_free_chunks = 1>
class chunked_deque {
    static_assert((items_per_chunk & (items_per_chunk - 1)) == 0,
            "chunked_deque chunk size must be power of two");

    union maybe_item {
        maybe_item() noexcept {}
        ~maybe_item() {}
        T data;
    };

    struct chunk {
        maybe_item items[items_per_chunk];
        struct chunk* next;
        struct chunk* prev;
        // begin and end interpreted mod items_per_chunk
        int begin;
        int end;

        size_t size() const noexcept {
            return end - begin;
        }

        bool empty() const noexcept {
            return begin == end;
        }
    };

    chunk* _front_chunk = nullptr;
    chunk* _back_chunk = nullptr;

    // We want an O(1) size but don't want to maintain a size() counter
    // because this will slow down every push and pop operation just for
    // the rare size() call. Instead, we just keep a count of chunks (which
    // doesn't change on every push or pop), from which we can calculate
    // size() when needed, and still be O(1).
    // This assumes the invariant that all middle chunks (except the front
    // and back) are always full.
    size_t _nchunks = 0;
    // A list of freed chunks, to support reserve() and to improve
    // performance of repeated push and pop, especially on an empty queue.
    // It is a performance/memory tradeoff how many freed chunks to keep
    // here (see save_free_chunks constant below).
    chunk* _free_chunks = nullptr;
    size_t _nfree_chunks = 0;
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = ssize_t;
    using reference = T&;
    using pointer = T*;
    using const_reference = const T&;
    using const_pointer = const T*;

private:
    template <typename U>
    class basic_iterator {
        friend class chunked_deque;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = U;
        using pointer = U*;
        using reference = U&;

    protected:
        const chunked_deque& _deq;
        chunk* _chunk = nullptr;
        // _item_index < 0: before begin
        // _item_index >= 0 < items_per_chunk: in chunk
        // _item_index > items_per_chunk: after end
        int _item_index = 0;

        basic_iterator(const chunked_deque& deq, chunk* c, int item_index) noexcept
            : _deq(deq)
            , _chunk(c)
            , _item_index(item_index)
        {
            SEASTAR_DEBUG_ASSERT((_chunk && (_item_index >= _chunk->begin && _item_index < _chunk->end))
                              || (!_chunk && (_item_index < 0 || _item_index >= items_per_chunk)));
        }

    public:
        basic_iterator() = default;
        basic_iterator(basic_iterator&& o) noexcept
            : _deq(o._deq)
            , _chunk(std::exchange(o._chunk, nullptr))
            , _item_index(std::exchange(o._item_index, items_per_chunk))
        {}
        basic_iterator(const basic_iterator& o) = default;

        bool operator==(const basic_iterator& o) const noexcept {
            return &_deq == &o._deq && _chunk == o._chunk
                    && (_item_index == o._item_index || !_chunk);
        }
        bool operator!=(const basic_iterator& o) const noexcept {
            return !operator==(o);
        }
        pointer operator->() const noexcept {
            SEASTAR_DEBUG_ASSERT(_chunk);
            SEASTAR_DEBUG_ASSERT(_item_index >= _chunk->begin);
            SEASTAR_DEBUG_ASSERT(_item_index < _chunk->end);
            return &_chunk->items[_item_index].data;
        }
        reference operator*() const noexcept {
            SEASTAR_DEBUG_ASSERT(_chunk);
            SEASTAR_DEBUG_ASSERT(_item_index >= _chunk->begin);
            SEASTAR_DEBUG_ASSERT(_item_index < _chunk->end);
            return _chunk->items[_item_index].data;
        }

        inline basic_iterator operator++(int) noexcept;
        inline basic_iterator& operator++() noexcept;
        inline basic_iterator operator--(int) noexcept;
        inline basic_iterator& operator--() noexcept;
    };

    template <typename U>
    class basic_forward_iterator : public basic_iterator<U> {
    protected:
        basic_forward_iterator(const chunked_deque& deq, chunk* c) noexcept
            : basic_iterator<U>(deq, c, c ? c->begin : items_per_chunk)
        {}
    };

    template <typename U>
    class basic_reverse_iterator : public basic_iterator<U> {
    protected:
        explicit basic_reverse_iterator(const chunked_deque& deq, chunk* c) noexcept
            : basic_iterator<U>(deq, c, c ? c->end - 1 : -1)
        {}

    public:
        inline basic_reverse_iterator operator++(int) noexcept {
            auto ret = *this;
            --*dynamic_cast<basic_iterator<U>*>(this);
            return ret;
        }
        inline basic_reverse_iterator& operator++() noexcept {
            --*dynamic_cast<basic_iterator<U>*>(this);
            return *this;
        }
        inline basic_reverse_iterator operator--(int) noexcept {
            auto ret = *this;
            ++*dynamic_cast<basic_iterator<U>*>(this);
            return ret;
}
        inline basic_reverse_iterator& operator--() noexcept {
            ++*dynamic_cast<basic_iterator<U>*>(this);
            return *this;
        }
    };

public:
    class iterator : public basic_forward_iterator<T> {
        using basic_forward_iterator<T>::basic_forward_iterator;
        friend class chunked_deque;
    };
    class const_iterator : public basic_forward_iterator<const T> {
        using basic_forward_iterator<const T>::basic_forward_iterator;
        friend class chunked_deque;
    public:
        const_iterator(const iterator& o) noexcept : basic_forward_iterator<const T>(o._deq, o._chunk, o._item_index) {}
        const_iterator(iterator&& o) noexcept : basic_forward_iterator<const T>(o._deq, o._chunk, o._item_index) {}
    };

    class reverse_iterator : public basic_reverse_iterator<T> {
        using basic_reverse_iterator<T>::basic_reverse_iterator;
        friend class chunked_deque;
    };
    class const_reverse_iterator : public basic_reverse_iterator<const T> {
        using basic_reverse_iterator<const T>::basic_reverse_iterator;
        friend class chunked_deque;
    public:
        const_reverse_iterator(const reverse_iterator& o) noexcept : basic_reverse_iterator<const T>(o._deq, o._chunk, o._item_index) {}
        const_reverse_iterator(reverse_iterator&& o) noexcept : basic_reverse_iterator<const T>(o._deq, o._chunk, o._item_index) {}
    };

public:
    chunked_deque() noexcept = default;
    chunked_deque(chunked_deque&& x) noexcept;
    chunked_deque(const chunked_deque& X) = delete;
    ~chunked_deque();
    chunked_deque& operator=(const chunked_deque&) = delete;
    chunked_deque& operator=(chunked_deque&&) noexcept;

    template <typename... A>
    inline void emplace_front(A&&... args);
    inline void push_front(const T& data);
    inline void push_front(T&& data);
    inline T& front() noexcept;
    inline const T& front() const noexcept;
    inline void pop_front() noexcept;

    template <typename... A>
    inline void emplace_back(A&&... args);
    inline void push_back(const T& data);
    inline void push_back(T&& data);
    inline T& back() noexcept;
    inline const T& back() const noexcept;
    inline void pop_back() noexcept;

    inline bool empty() const noexcept;
    inline size_t size() const noexcept;
    void clear() noexcept;
    // reserve_front(n) ensures that at least (n - size()) further push_front() calls can
    // be served without needing new memory allocation.
    // Calling pop()s between these push()es is also allowed and does not
    // alter this guarantee.
    // Note that reserve() does not reduce the amount of memory already
    // reserved - use shrink_to_fit() for that.
    void reserve_front(size_t n);
    // reserve_back(n) ensures that at least (n - size()) further push_back() calls can
    // be served without needing new memory allocation.
    void reserve_back(size_t n);
    // shrink_to_fit() frees memory held, but unused, by the queue. Such
    // unused memory might exist after pops, or because of reserve().
    void shrink_to_fit() noexcept;

    inline iterator begin() noexcept { return iterator(*this, _front_chunk); }
    inline iterator end() noexcept { return iterator(*this, nullptr); }
    inline const_iterator begin() const noexcept { return const_iterator(*this, _front_chunk); }
    inline const_iterator end() const noexcept { return const_iterator(*this, nullptr); }
    inline const_iterator cbegin() const noexcept { return const_iterator(*this, _front_chunk); }
    inline const_iterator cend() const noexcept { return const_iterator(*this, nullptr); }

    inline reverse_iterator rbegin() noexcept { return reverse_iterator(*this, _back_chunk); }
    inline reverse_iterator rend() noexcept { return reverse_iterator(*this, nullptr); }
    inline const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(*this, _back_chunk); }
    inline const_reverse_iterator rend() const noexcept { return const_reverse_iterator(*this, nullptr); }
    inline const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(*this, _back_chunk); }
    inline const_reverse_iterator crend() const noexcept { return const_reverse_iterator(*this, nullptr); }

private:
    chunk* new_chunk();
    inline void ensure_room_front();
    inline void ensure_room_back();
    void undo_room_front() noexcept;
    void undo_room_back() noexcept;
    void delete_front_chunk() noexcept;
    void delete_back_chunk() noexcept;
    void reserve_chunks(size_t nchunks);
};

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline
chunked_deque<T, items_per_chunk, save_free_chunks>::chunked_deque(chunked_deque&& x) noexcept
        : _front_chunk(std::exchange(x._front_chunk, nullptr))
        , _back_chunk(std::exchange(x._back_chunk, nullptr))
        , _nchunks(std::exchange(x._nchunks, 0))
        , _free_chunks(std::exchange(x._free_chunks, nullptr))
        , _nfree_chunks(std::exchange(x._nfree_chunks, 0))
{}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline
chunked_deque<T, items_per_chunk, save_free_chunks>&
chunked_deque<T, items_per_chunk, save_free_chunks>::operator=(chunked_deque&& x) noexcept {
    if (&x != this) {
        this->~chunked_deque();
        new (this) chunked_deque(std::move(x));
    }
    return *this;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline bool
chunked_deque<T, items_per_chunk, save_free_chunks>::empty() const noexcept {
    return _front_chunk == nullptr;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline size_t
chunked_deque<T, items_per_chunk, save_free_chunks>::size() const noexcept{
    if (_front_chunk == nullptr) {
        return 0;
    } else if (_back_chunk == _front_chunk) {
        // Single chunk.
        return _front_chunk->size();
    } else {
        return _front_chunk->size() + _back_chunk->size()
                + (_nchunks - 2) * items_per_chunk;
    }
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
void chunked_deque<T, items_per_chunk, save_free_chunks>::clear() noexcept {
    while (!empty()) {
        pop_front();
    }
}

template <typename T, size_t items_per_chunk, int save_free_chunks> void
chunked_deque<T, items_per_chunk, save_free_chunks>::shrink_to_fit() noexcept {
    while (_free_chunks) {
        auto next = _free_chunks->next;
        delete _free_chunks;
        _free_chunks = next;
    }
    _nfree_chunks = 0;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
chunked_deque<T, items_per_chunk, save_free_chunks>::~chunked_deque() {
    clear();
    shrink_to_fit();
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
typename chunked_deque<T, items_per_chunk, save_free_chunks>::chunk*
chunked_deque<T, items_per_chunk, save_free_chunks>::new_chunk() {
    chunk* ret;
    if (_free_chunks) {
        ret = _free_chunks;
        _free_chunks = _free_chunks->next;
        --_nfree_chunks;
    } else {
        ret = new chunk;
    }
    _nchunks++;
    return ret;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::ensure_room_front() {
    // If we don't have a back chunk or it's full, we need to create a new one
    if (_front_chunk != nullptr && _front_chunk->begin > 0) {
        return;
    }
    chunk* n = new_chunk();
    n->prev = nullptr;
    if (_front_chunk) {
        _front_chunk->prev = n;
        n->begin = n->end = items_per_chunk;
    } else {
        _back_chunk = n;
        n->begin = n->end = items_per_chunk / 2;
    }
    n->next = _front_chunk;
    _front_chunk = n;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::ensure_room_back() {
    // If we don't have a back chunk or it's full, we need to create a new one
    if (_back_chunk != nullptr && _back_chunk->end < items_per_chunk) {
        return;
    }
    chunk* n = new_chunk();
    n->next = nullptr;
    if (_back_chunk) {
        _back_chunk->next = n;
        n->begin = n->end = 0;
    } else {
        _front_chunk = n;
        n->begin = n->end = items_per_chunk / 2;
    }
    n->prev = _back_chunk;
    _back_chunk = n;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
void
chunked_deque<T, items_per_chunk, save_free_chunks>::undo_room_front() noexcept {
    if (!_front_chunk->empty()) {
        return;
    }
    chunk* old = _front_chunk;
    _front_chunk = old->next;
    if (_front_chunk) {
        _front_chunk->prev = nullptr;
    } else {
        _back_chunk = nullptr;
    }
    --_nchunks;
    delete old;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
void
chunked_deque<T, items_per_chunk, save_free_chunks>::undo_room_back() noexcept {
    if (!_back_chunk->empty()) {
        return;
    }
    chunk* old = _back_chunk;
    _back_chunk = old->prev;
    if (_back_chunk) {
        _back_chunk->next = nullptr;
    } else {
        _front_chunk = nullptr;
    }
    --_nchunks;
    delete old;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
template <typename... Args>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::emplace_front(Args&&... args) {
    ensure_room_front();
    auto next = _front_chunk->begin - 1;
    auto p = &_front_chunk->items[next].data;
    try {
        new (p) T(std::forward<Args>(args)...);
    } catch(...) {
        undo_room_front();
        throw;
    }
    _front_chunk->begin = next;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::push_front(const T& data) {
    ensure_room_front();
    auto next = _front_chunk->begin - 1;
    auto p = &_front_chunk->items[next].data;
    try {
        new (p) T(data);
    } catch(...) {
        undo_room_front();
        throw;
    }
    _front_chunk->begin = next;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::push_front(T&& data) {
    ensure_room_front();
    auto next = _front_chunk->begin - 1;
    auto p = &_front_chunk->items[next].data;
    try {
        new (p) T(std::move(data));
    } catch(...) {
        undo_room_front();
        throw;
    }
    _front_chunk->begin = next;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
template <typename... Args>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::emplace_back(Args&&... args) {
    ensure_room_back();
    auto p = &_back_chunk->items[_back_chunk->end].data;
    try {
        new(p) T(std::forward<Args>(args)...);
    } catch(...) {
        undo_room_back();
        throw;
    }
    ++_back_chunk->end;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::push_back(const T& data) {
    ensure_room_back();
    auto p = &_back_chunk->items[_back_chunk->end].data;
    try {
        new(p) T(data);
    } catch(...) {
        undo_room_back();
        throw;
    }
    ++_back_chunk->end;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::push_back(T&& data) {
    ensure_room_back();
    auto p = &_back_chunk->items[_back_chunk->end].data;
    try {
        new(p) T(std::move(data));
    } catch(...) {
        undo_room_back();
        throw;
    }
    ++_back_chunk->end;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline T&
chunked_deque<T, items_per_chunk, save_free_chunks>::front() noexcept {
    return _front_chunk->items[_front_chunk->begin].data;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline const T&
chunked_deque<T, items_per_chunk, save_free_chunks>::front() const noexcept {
    return _front_chunk->items[_front_chunk->begin].data;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline
T&
chunked_deque<T, items_per_chunk, save_free_chunks>::back() noexcept {
    return _back_chunk->items[_back_chunk->end - 1].data;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline
const T&
chunked_deque<T, items_per_chunk, save_free_chunks>::back() const noexcept {
    return _back_chunk->items[_back_chunk->end - 1].data;
}

// Certain use cases may need to repeatedly allocate and free a chunk -
// an obvious example is an empty queue to which we push, and then pop,
// repeatedly. Another example is pushing and popping to a non-empty deque
// we push and pop at different chunks so we need to free and allocate a
// chunk every items_per_chunk operations.
// The solution is to keep a list of freed chunks instead of freeing them
// immediately. There is a performance/memory tradeoff of how many freed
// chunks to save: If we save them all, the queue can never shrink from
// its maximum memory use (this is how circular_buffer behaves).
// The ad-hoc choice made here is to limit the number of saved chunks to 1,
// but this could easily be made a configuration option.
template <typename T, size_t items_per_chunk, int save_free_chunks>
void
chunked_deque<T, items_per_chunk, save_free_chunks>::delete_front_chunk() noexcept {
    chunk *next = _front_chunk->next;
    if (_nfree_chunks < save_free_chunks) {
        _front_chunk->next = _free_chunks;
        _free_chunks = _front_chunk;
        ++_nfree_chunks;
    } else {
        delete _front_chunk;
    }
    if ((_front_chunk = next)) {
        next->prev = nullptr;
    } else {
        _back_chunk = nullptr;
    }
    --_nchunks;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
void
chunked_deque<T, items_per_chunk, save_free_chunks>::delete_back_chunk() noexcept {
    chunk *prev = _back_chunk->prev;
    if (_nfree_chunks < save_free_chunks) {
        _back_chunk->next = _free_chunks;
        _free_chunks = _back_chunk;
        ++_nfree_chunks;
    } else {
        delete _back_chunk;
    }
    if ((_back_chunk = prev)) {
        prev->next = nullptr;
    } else {
        _front_chunk = nullptr;
    }
    --_nchunks;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::pop_front() noexcept {
    front().~T();
    if (++_front_chunk->begin == _front_chunk->end) {
        delete_front_chunk();
    }
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
inline void
chunked_deque<T, items_per_chunk, save_free_chunks>::pop_back() noexcept {
    back().~T();
    if (--_back_chunk->end == _back_chunk->begin) {
        delete_back_chunk();
    }
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
void chunked_deque<T, items_per_chunk, save_free_chunks>::reserve_chunks(size_t needed_chunks) {
    // If we already have some freed chunks saved, we need to allocate fewer
    // additional chunks, or none at all
    if (needed_chunks <= _nfree_chunks) {
        return;
    }
    needed_chunks -= _nfree_chunks;
    while (needed_chunks--) {
        chunk *c = new chunk;
        c->next = _free_chunks;
        _free_chunks = c;
        ++_nfree_chunks;
    }
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
void chunked_deque<T, items_per_chunk, save_free_chunks>::reserve_front(size_t n) {
    // reserve() guarantees that (n - size()) additional push()es will
    // succeed without reallocation:
    if (n <= size()) {
        return;
    }
    size_t need = n - size();
    // If we already have a back chunk, it might have room for some pushes
    // before filling up, so decrease "need":
    if (_front_chunk && _front_chunk->begin) {
        size_t remaining = _front_chunk->begin - 1;
        if (remaining >= need) {
            return;
        }
        need -= remaining;
    }
    reserve_chunks((need + items_per_chunk - 1) / items_per_chunk);
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
void chunked_deque<T, items_per_chunk, save_free_chunks>::reserve_back(size_t n) {
    // reserve() guarantees that (n - size()) additional push()es will
    // succeed without reallocation:
    if (n <= size()) {
        return;
    }
    size_t need = n - size();
    // If we already have a back chunk, it might have room for some pushes
    // before filling up, so decrease "need":
    if (_back_chunk) {
        size_t remaining = items_per_chunk - _back_chunk->end;
        if (remaining >= need) {
            return;
        }
        need -= remaining;
    }
    reserve_chunks((need + items_per_chunk - 1) / items_per_chunk);
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
template <typename U>
inline typename chunked_deque<T, items_per_chunk, save_free_chunks>::template basic_iterator<U>
chunked_deque<T, items_per_chunk, save_free_chunks>::basic_iterator<U>::operator++(int) noexcept {
    auto it = *this;
    ++(*this);
    return it;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
template <typename U>
inline typename chunked_deque<T, items_per_chunk, save_free_chunks>::template basic_iterator<U>&
chunked_deque<T, items_per_chunk, save_free_chunks>::basic_iterator<U>::operator++() noexcept {
    if (__builtin_expect(_item_index < 0, false)) {
        SEASTAR_DEBUG_ASSERT(_chunk == nullptr);
        _chunk = _deq._front_chunk;
        _item_index = _chunk ? _chunk->begin : items_per_chunk;
    } else if (_chunk) {
        ++_item_index;
        if (_item_index == _chunk->end) {
            _chunk = _chunk->next;
            _item_index = _chunk ? _chunk->begin : items_per_chunk;
        }
    } else {
        SEASTAR_DEBUG_ASSERT(_item_index >= items_per_chunk);
    }
    return *this;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
template <typename U>
inline typename chunked_deque<T, items_per_chunk, save_free_chunks>::template basic_iterator<U>
chunked_deque<T, items_per_chunk, save_free_chunks>::basic_iterator<U>::operator--(int) noexcept {
    auto it = *this;
    --(*this);
    return it;
}

template <typename T, size_t items_per_chunk, int save_free_chunks>
template <typename U>
typename chunked_deque<T, items_per_chunk, save_free_chunks>::template basic_iterator<U>&
chunked_deque<T, items_per_chunk, save_free_chunks>::basic_iterator<U>::operator--() noexcept {
    if (__builtin_expect(_item_index >= items_per_chunk, false)) {
        SEASTAR_DEBUG_ASSERT(_chunk == nullptr);
        _chunk = _deq._back_chunk;
        _item_index = _chunk ? _chunk->end - 1 : -1;
    } else if (_chunk) {
        if (_item_index == _chunk->begin) {
            _chunk = _chunk->prev;
            _item_index = _chunk ? _chunk->end - 1 : -1;
        } else {
            --_item_index;
        }
    } else {
        SEASTAR_DEBUG_ASSERT(_item_index < 0);
    }
    return *this;
}

}

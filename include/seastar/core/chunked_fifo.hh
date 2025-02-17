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
 * Copyright (C) 2016 ScyllaDB Ltd.
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <algorithm>
#include <cassert>
#include <type_traits>
#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>
#endif

namespace seastar {

// An unbounded FIFO queue of objects of type T.
//
// It provides operations to push items in one end of the queue, and pop them
// from the other end of the queue - both operations are guaranteed O(1)
// (not just amortized O(1)). The size() operation is also O(1).
// chunked_fifo also guarantees that the largest contiguous memory allocation
// it does is O(1). The total memory used is, of course, O(N).
//
// How does chunked_fifo differ from std::list<>, circular_buffer<> and
// std::deque()?
//
// std::list<> can also make all the above guarantees, but is inefficient -
// both at run speed (every operation requires an allocation), and in memory
// use. Much more efficient than std::list<> is our circular_buffer<>, which
// allocates a contiguous array to hold the items and only reallocates it,
// exponentially, when the queue grows. On one test of several different
// push/pop scenarios, circular_buffer<> was between 5 and 20 times faster
// than std::list, and also used considerably less memory.
// The problem with circular_buffer<> is that gives up on the last guarantee
// we made above: circular_buffer<> allocates all the items in one large
// contiguous allocation - that might not be possible when the memory is
// highly fragmented.
// std::deque<> aims to solve the contiguous allocation problem by allocating
// smaller chunks of the queue, and keeping a list of them in an array. This
// array is necessary to allow for O(1) random access to any element, a
// feature which we do not need; But this array is itself contiguous so
// std::deque<> attempts larger contiguous allocations the larger the queue
// gets: std::deque<>'s contiguous allocation is still O(N) and in fact
// exactly 1/64 of the size of circular_buffer<>'s contiguous allocation.
// So it's an improvement over circular_buffer<>, but not a full solution.
//
// chunked_fifo<> is such a solution: it also allocates the queue in fixed-
// size chunks (just like std::deque) but holds them in a linked list, not
// a contiguous array, so there are no large contiguous allocations.
//
// Unlike std::deque<> or circular_buffer<>, chunked_fifo only provides the
// operations needed by std::queue, i.e.,: empty(), size(), front(), back(),
// push_back() and pop_front(). For simplicity, we do *not* implement other
// possible operations, like inserting or deleting elements from the "wrong"
// side of the queue or from the middle, nor random-access to items in the
// middle of the queue. However, chunked_fifo does allow iterating over all
// of the queue's elements without popping them, a feature which std::queue
// is missing.
//
// Another feature of chunked_fifo which std::deque is missing is the ability
// to control the chunk size, as a template parameter. In std::deque the
// chunk size is undocumented and fixed - in gcc, it is always 512 bytes.
// chunked_fifo, on the other hand, makes the chunk size (in number of items
// instead of bytes) a template parameter; In situations where the queue is
// expected to become very long, using a larger chunk size might make sense
// because it will result in fewer allocations.
//
// chunked_fifo uses uninitialized storage for unoccupied elements, and thus
// uses move/copy constructors instead of move/copy assignments, which are
// less efficient.

SEASTAR_MODULE_EXPORT
template <typename T, size_t items_per_chunk = 128>
class chunked_fifo {
    static_assert((items_per_chunk & (items_per_chunk - 1)) == 0,
            "chunked_fifo chunk size must be power of two");
    union maybe_item {
        maybe_item() noexcept {}
        ~maybe_item() {}
        T data;
    };
    struct chunk {
        maybe_item items[items_per_chunk];
        struct chunk* next;
        // begin and end interpreted mod items_per_chunk
        unsigned begin;
        unsigned end;

        // the number of elements in this chunk
        size_t size() const {
            return end - begin;
        }
    };
    // We pop from the chunk at _front_chunk. This chunk is then linked to
    // the following chunks via the "next" link. _back_chunk points to the
    // last chunk in this list, and it is where we push.
    chunk* _front_chunk = nullptr; // where we pop
    chunk* _back_chunk = nullptr; // where we push
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
    using reference = T&;
    using pointer = T*;
    using const_reference = const T&;
    using const_pointer = const T*;

private:
    template <bool IsConst>
    class basic_iterator {
        friend class chunked_fifo;

    public:
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = std::conditional_t<IsConst, const T, T>;
        using pointer = value_type*;
        using reference = value_type&;

    private:
        using chunk_t = std::conditional_t<IsConst, const chunk, chunk>;
        chunk_t* _chunk = nullptr;
        size_t _item_index = 0;

    protected:
        inline explicit basic_iterator(chunk_t* c) noexcept;
        inline basic_iterator(chunk_t* c, size_t item_index) noexcept;

    public:
        basic_iterator() noexcept = default;
        template<bool OtherIsConst, std::enable_if_t<IsConst && !OtherIsConst, int> = 0>
        inline basic_iterator(const basic_iterator<OtherIsConst>& o) noexcept
	  :  basic_iterator{o._chunk, o._item_index} {}
        inline bool operator==(const basic_iterator& o) const noexcept;
        inline bool operator!=(const basic_iterator& o) const noexcept;
        inline pointer operator->() const noexcept;
        inline reference operator*() const noexcept;
        inline basic_iterator operator++(int) noexcept;
        basic_iterator& operator++() noexcept;
    };

public:
    using iterator = basic_iterator<false>;
    using const_iterator = basic_iterator<true>;

public:
    chunked_fifo() noexcept = default;
    chunked_fifo(chunked_fifo&& x) noexcept;
    chunked_fifo(const chunked_fifo& X) = delete;
    ~chunked_fifo();
    chunked_fifo& operator=(const chunked_fifo&) = delete;
    chunked_fifo& operator=(chunked_fifo&&) noexcept;
    inline void push_back(const T& data);
    inline void push_back(T&& data);
    T& back() noexcept;
    const T& back() const noexcept;
    template <typename... A>
    inline void emplace_back(A&&... args);
    inline T& front() const noexcept;
    inline void pop_front() noexcept;
    inline bool empty() const noexcept;
    inline size_t size() const noexcept;
    // Pop the first n elements from the fifo. Equivalent to calling pop_front()
    // n times, though likely to be faster for n greater than 1. The fifo must
    // contain at least n elements or the behavior is undefined.
    void pop_front_n(size_t n) noexcept;
    void clear() noexcept;
    // reserve(n) ensures that at least (n - size()) further push() calls can
    // be served without needing new memory allocation.
    // Calling pop()s between these push()es is also allowed and does not
    // alter this guarantee.
    // Note that reserve() does not reduce the amount of memory already
    // reserved - use shrink_to_fit() for that.
    void reserve(size_t n);
    // shrink_to_fit() frees memory held, but unused, by the queue. Such
    // unused memory might exist after pops, or because of reserve().
    void shrink_to_fit() noexcept;
    inline iterator begin() noexcept;
    inline iterator end() noexcept;
    inline const_iterator begin() const noexcept;
    inline const_iterator end() const noexcept;
    inline const_iterator cbegin() const noexcept;
    inline const_iterator cend() const noexcept;
private:
    void back_chunk_new();
    void front_chunk_delete() noexcept;
    inline void ensure_room_back();
    void undo_room_back() noexcept;
    static inline size_t mask(size_t idx) noexcept;

};

template <typename T, size_t items_per_chunk>
template <bool IsConst>
inline
chunked_fifo<T, items_per_chunk>::basic_iterator<IsConst>::basic_iterator(chunk_t* c) noexcept : _chunk(c), _item_index(_chunk ? _chunk->begin : 0) {
}

template <typename T, size_t items_per_chunk>
template <bool IsConst>
inline
chunked_fifo<T, items_per_chunk>::basic_iterator<IsConst>::basic_iterator(chunk_t* c, size_t item_index) noexcept : _chunk(c), _item_index(item_index) {
}

template <typename T, size_t items_per_chunk>
template <bool IsConst>
inline bool
chunked_fifo<T, items_per_chunk>::basic_iterator<IsConst>::operator==(const basic_iterator& o) const noexcept {
    return _chunk == o._chunk && _item_index == o._item_index;
}

template <typename T, size_t items_per_chunk>
template <bool IsConst>
inline bool
chunked_fifo<T, items_per_chunk>::basic_iterator<IsConst>::operator!=(const basic_iterator& o) const noexcept {
    return !(*this == o);
}

template <typename T, size_t items_per_chunk>
template <bool IsConst>
inline typename chunked_fifo<T, items_per_chunk>::template basic_iterator<IsConst>::pointer
chunked_fifo<T, items_per_chunk>::basic_iterator<IsConst>::operator->() const noexcept {
    return &_chunk->items[chunked_fifo::mask(_item_index)].data;
}

template <typename T, size_t items_per_chunk>
template <bool IsConst>
inline typename chunked_fifo<T, items_per_chunk>::template basic_iterator<IsConst>::reference
chunked_fifo<T, items_per_chunk>::basic_iterator<IsConst>::operator*() const noexcept {
    return _chunk->items[chunked_fifo::mask(_item_index)].data;
}

template <typename T, size_t items_per_chunk>
template <bool IsConst>
inline typename chunked_fifo<T, items_per_chunk>::template basic_iterator<IsConst>
chunked_fifo<T, items_per_chunk>::basic_iterator<IsConst>::operator++(int) noexcept {
    auto it = *this;
    ++(*this);
    return it;
}

template <typename T, size_t items_per_chunk>
template <bool IsConst>
typename chunked_fifo<T, items_per_chunk>::template basic_iterator<IsConst>&
chunked_fifo<T, items_per_chunk>::basic_iterator<IsConst>::operator++() noexcept {
    ++_item_index;
    if (_item_index == _chunk->end) {
        _chunk = _chunk->next;
        _item_index = _chunk ? _chunk->begin : 0;
    }
    return *this;
}

template <typename T, size_t items_per_chunk>
inline
chunked_fifo<T, items_per_chunk>::chunked_fifo(chunked_fifo&& x) noexcept
        : _front_chunk(x._front_chunk)
        , _back_chunk(x._back_chunk)
        , _nchunks(x._nchunks)
        , _free_chunks(x._free_chunks)
        , _nfree_chunks(x._nfree_chunks) {
    x._front_chunk = nullptr;
    x._back_chunk = nullptr;
    x._nchunks = 0;
    x._free_chunks = nullptr;
    x._nfree_chunks = 0;
}

template <typename T, size_t items_per_chunk>
inline
chunked_fifo<T, items_per_chunk>&
chunked_fifo<T, items_per_chunk>::operator=(chunked_fifo&& x) noexcept {
    if (&x != this) {
        this->~chunked_fifo();
        new (this) chunked_fifo(std::move(x));
    }
    return *this;
}

template <typename T, size_t items_per_chunk>
inline size_t
chunked_fifo<T, items_per_chunk>::mask(size_t idx) noexcept {
    return idx & (items_per_chunk - 1);
}

template <typename T, size_t items_per_chunk>
inline bool
chunked_fifo<T, items_per_chunk>::empty() const noexcept {
    return _front_chunk == nullptr;
}

template <typename T, size_t items_per_chunk>
inline size_t
chunked_fifo<T, items_per_chunk>::size() const noexcept{
    if (_front_chunk == nullptr) {
        return 0;
    } else if (_back_chunk == _front_chunk) {
        // Single chunk.
        return _front_chunk->end - _front_chunk->begin;
    } else {
        return _front_chunk->end - _front_chunk->begin
                +_back_chunk->end - _back_chunk->begin
                + (_nchunks - 2) * items_per_chunk;
    }
}

template <typename T, size_t items_per_chunk>
void chunked_fifo<T, items_per_chunk>::clear() noexcept {
    pop_front_n(size());
}

template <typename T, size_t items_per_chunk>
void chunked_fifo<T, items_per_chunk>::pop_front_n(size_t n) noexcept {
    while (n) {
        SEASTAR_ASSERT(_front_chunk && "pop_front_n n too large");

        auto target = _front_chunk;
        unsigned delete_count = std::min(target->size(), n);

        for (auto i = target->begin, e = i + delete_count; i != e; i++) {
            target->items[mask(i)].data.~T();
        }

        target->begin += delete_count;
        n -= delete_count;

        if (target->size() == 0) {
            front_chunk_delete();
        }
    }
}



template <typename T, size_t items_per_chunk> void
chunked_fifo<T, items_per_chunk>::shrink_to_fit() noexcept {
    while (_free_chunks) {
        auto next = _free_chunks->next;
        delete _free_chunks;
        _free_chunks = next;
    }
    _nfree_chunks = 0;
}

template <typename T, size_t items_per_chunk>
chunked_fifo<T, items_per_chunk>::~chunked_fifo() {
    clear();
    shrink_to_fit();
}

template <typename T, size_t items_per_chunk>
void
chunked_fifo<T, items_per_chunk>::back_chunk_new() {
    chunk *old = _back_chunk;
    if (_free_chunks) {
        _back_chunk = _free_chunks;
        _free_chunks = _free_chunks->next;
        --_nfree_chunks;
    } else {
        _back_chunk = new chunk;
    }
    _back_chunk->next = nullptr;
    _back_chunk->begin = 0;
    _back_chunk->end = 0;
    if (old) {
        old->next = _back_chunk;
    }
    if (_front_chunk == nullptr) {
        _front_chunk = _back_chunk;
    }
    _nchunks++;
}


template <typename T, size_t items_per_chunk>
inline void
chunked_fifo<T, items_per_chunk>::ensure_room_back() {
    // If we don't have a back chunk or it's full, we need to create a new one
    if (_back_chunk == nullptr ||
            (_back_chunk->end - _back_chunk->begin) == items_per_chunk) {
        back_chunk_new();
    }
}

template <typename T, size_t items_per_chunk>
void
chunked_fifo<T, items_per_chunk>::undo_room_back() noexcept {
    // If we failed creating a new item after ensure_room_back() created a
    // new empty chunk, we must remove it, or empty() will be incorrect
    // (either immediately, if the fifo was empty, or when all the items are
    // popped, if it already had items).
    if (_back_chunk->begin == _back_chunk->end) {
        delete _back_chunk;
        --_nchunks;
        if (_nchunks == 0) {
            _back_chunk = nullptr;
            _front_chunk = nullptr;
        } else {
            // Because we don't usually pop from the back, we don't have a "prev"
            // pointer so we need to find the previous chunk the hard and slow
            // way. B
            chunk *old = _back_chunk;
            _back_chunk = _front_chunk;
            while (_back_chunk->next != old) {
                _back_chunk = _back_chunk->next;
            }
            _back_chunk->next = nullptr;
        }
    }

}

template <typename T, size_t items_per_chunk>
template <typename... Args>
inline void
chunked_fifo<T, items_per_chunk>::emplace_back(Args&&... args) {
    ensure_room_back();
    auto p = &_back_chunk->items[mask(_back_chunk->end)].data;
    try {
        new(p) T(std::forward<Args>(args)...);
    } catch(...) {
        undo_room_back();
        throw;
    }
    ++_back_chunk->end;
}

template <typename T, size_t items_per_chunk>
inline void
chunked_fifo<T, items_per_chunk>::push_back(const T& data) {
    ensure_room_back();
    auto p = &_back_chunk->items[mask(_back_chunk->end)].data;
    try {
        new(p) T(data);
    } catch(...) {
        undo_room_back();
        throw;
    }
    ++_back_chunk->end;
}

template <typename T, size_t items_per_chunk>
inline void
chunked_fifo<T, items_per_chunk>::push_back(T&& data) {
    ensure_room_back();
    auto p = &_back_chunk->items[mask(_back_chunk->end)].data;
    try {
        new(p) T(std::move(data));
    } catch(...) {
        undo_room_back();
        throw;
    }
    ++_back_chunk->end;
}

template <typename T, size_t items_per_chunk>
inline
T&
chunked_fifo<T, items_per_chunk>::back() noexcept {
    return _back_chunk->items[mask(_back_chunk->end - 1)].data;
}

template <typename T, size_t items_per_chunk>
inline
const T&
chunked_fifo<T, items_per_chunk>::back() const noexcept {
    return _back_chunk->items[mask(_back_chunk->end - 1)].data;
}

template <typename T, size_t items_per_chunk>
inline T&
chunked_fifo<T, items_per_chunk>::front() const noexcept {
    return _front_chunk->items[mask(_front_chunk->begin)].data;
}

template <typename T, size_t items_per_chunk>
inline void
chunked_fifo<T, items_per_chunk>::front_chunk_delete() noexcept {
    chunk *next = _front_chunk->next;
    // Certain use cases may need to repeatedly allocate and free a chunk -
    // an obvious example is an empty queue to which we push, and then pop,
    // repeatedly. Another example is pushing and popping to a non-empty queue
    // we push and pop at different chunks so we need to free and allocate a
    // chunk every items_per_chunk operations.
    // The solution is to keep a list of freed chunks instead of freeing them
    // immediately. There is a performance/memory tradeoff of how many freed
    // chunks to save: If we save them all, the queue can never shrink from
    // its maximum memory use (this is how circular_buffer behaves).
    // The ad-hoc choice made here is to limit the number of saved chunks to 1,
    // but this could easily be made a configuration option.
    static constexpr int save_free_chunks = 1;
    if (_nfree_chunks < save_free_chunks) {
        _front_chunk->next = _free_chunks;
        _free_chunks = _front_chunk;
        ++_nfree_chunks;
    } else {
        delete _front_chunk;
    }
    // If we only had one chunk, _back_chunk is gone too.
    if (_back_chunk == _front_chunk) {
        _back_chunk = nullptr;
    }
    _front_chunk = next;
    --_nchunks;
}

template <typename T, size_t items_per_chunk>
inline void
chunked_fifo<T, items_per_chunk>::pop_front() noexcept {
    front().~T();
    // If the front chunk has become empty, we need to free remove it and use
    // the next one.
    if (++_front_chunk->begin == _front_chunk->end) {
        front_chunk_delete();
    }
}

template <typename T, size_t items_per_chunk>
void chunked_fifo<T, items_per_chunk>::reserve(size_t n) {
    // reserve() guarantees that (n - size()) additional push()es will
    // succeed without reallocation:
    if (n <= size()) {
        return;
    }
    size_t need = n - size();
    // If we already have a back chunk, it might have room for some pushes
    // before filling up, so decrease "need":
    if (_back_chunk) {
        size_t back_chunk_n = items_per_chunk - (_back_chunk->end - _back_chunk->begin);
        need -= std::min(back_chunk_n, need);
    }
    size_t needed_chunks = (need + items_per_chunk - 1) / items_per_chunk;
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

template <typename T, size_t items_per_chunk>
inline typename chunked_fifo<T, items_per_chunk>::iterator
chunked_fifo<T, items_per_chunk>::begin() noexcept {
    return iterator(_front_chunk);
}

template <typename T, size_t items_per_chunk>
inline typename chunked_fifo<T, items_per_chunk>::iterator
chunked_fifo<T, items_per_chunk>::end() noexcept {
    return iterator(nullptr);
}

template <typename T, size_t items_per_chunk>
inline typename chunked_fifo<T, items_per_chunk>::const_iterator
chunked_fifo<T, items_per_chunk>::begin() const noexcept {
    return const_iterator(_front_chunk);
}

template <typename T, size_t items_per_chunk>
inline typename chunked_fifo<T, items_per_chunk>::const_iterator
chunked_fifo<T, items_per_chunk>::end() const noexcept {
    return const_iterator(nullptr);
}

template <typename T, size_t items_per_chunk>
inline typename chunked_fifo<T, items_per_chunk>::const_iterator
chunked_fifo<T, items_per_chunk>::cbegin() const noexcept {
    return const_iterator(_front_chunk);
}

template <typename T, size_t items_per_chunk>
inline typename chunked_fifo<T, items_per_chunk>::const_iterator
chunked_fifo<T, items_per_chunk>::cend() const noexcept {
    return const_iterator(nullptr);
}

}

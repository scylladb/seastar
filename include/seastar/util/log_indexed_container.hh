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
 * Copyright (C) 2026-present ScyllaDB
 */

#pragma once

#include <concepts>
#include <optional>
#include <boost/container/deque.hpp>

#include <seastar/util/assert.hh>

namespace seastar {

/// \brief A container that maps monotonically increasing indices to values,
/// providing O(1) access by index.
///
/// Internally uses a deque of slots indexed by `(index - base_index)`.
/// Completed/cleared slots are nulled in-place; the front is trimmed lazily
/// (via trim_front()) to reclaim memory.
///
/// For value types that already encode an empty/absent state natively
/// (raw pointer, unique_ptr, shared_ptr, and similar pointer-like types
/// that are default-constructible to a null state and convertible to bool),
/// slots store the value directly using its natural empty state.
/// For all other types, slots are wrapped in std::optional.
///
/// Unlike a hash map, the backing deque grows in fixed-size chunks.
/// boost::container::deque guarantees that memory blocks are released when
/// items are popped from the ends, so trim_front() actually reclaims memory
/// (unlike std::deque in libstdc++, which retains blocks).
///
/// The container does not require indices to be inserted in strictly
/// increasing order. Gaps between occupied slots are allowed.
/// Out-of-range or absent lookups return nullptr safely.
///
/// \tparam T         Value type to store.
/// \tparam IndexType Integer-like type used as the index (default: size_t).
///                   Must support: operator-(IndexType) → ptrdiff_t-compatible,
///                   operator<, and prefix operator++.
template<typename T, typename IndexType = size_t>
class log_indexed_container {
    // Detect pointer-like types that already carry a null/absent state:
    //   - default-constructs to an "empty" value
    //   - convertible to bool (true = has value, false = empty)
    //   - is a raw pointer OR has an element_type member (smart pointer)
    // Plain integers and enums satisfy the first two but not the third,
    // so they fall through to optional<T> wrapping as intended.
    template<typename U>
    static constexpr bool is_nullable =
        std::is_default_constructible_v<U> &&
        requires(const U& u) { { static_cast<bool>(u) } -> std::same_as<bool>; } &&
        (std::is_pointer_v<U> || requires { typename U::element_type; });

    using slot_t = std::conditional_t<is_nullable<T>, T, std::optional<T>>;

    static bool slot_occupied(const slot_t& s) noexcept {
        if constexpr (is_nullable<T>) {
            return static_cast<bool>(s);
        } else {
            return s.has_value();
        }
    }

    // boost::container::deque releases memory blocks on pop_front/pop_back,
    // unlike std::deque (libstdc++) which may retain them indefinitely.
    boost::container::deque<slot_t> _slots;
    IndexType _base_idx{};
    size_t _count = 0; // number of occupied (non-empty) slots

public:
    /// Returns a pointer to the value at the given index,
    /// or nullptr if the slot is empty or out of range.
    T* find(IndexType idx) noexcept {
        if (_slots.empty() || idx < _base_idx) {
            return nullptr;
        }
        auto off = static_cast<size_t>(idx - _base_idx);
        if (off >= _slots.size()) {
            return nullptr;
        }
        auto& slot = _slots[off];
        if constexpr (is_nullable<T>) {
            return slot ? std::addressof(slot) : nullptr;
        } else {
            return slot.has_value() ? std::addressof(*slot) : nullptr;
        }
    }

    /// Inserts a value at the given index.
    /// If idx < base_index(), empty slots are prepended to accommodate it.
    /// The target slot must not already be occupied.
    /// Returns a reference to the inserted value.
    T& emplace(IndexType idx, T value) {
        if (_slots.empty()) {
            _base_idx = idx;
            if constexpr (is_nullable<T>) {
                _slots.push_back(std::move(value));
            } else {
                _slots.emplace_back(std::move(value));
            }
        } else if (idx < _base_idx) {
            // New index is before current base: prepend empty slots.
            auto count = static_cast<size_t>(_base_idx - idx);
            for (size_t i = 0; i < count; ++i) {
                _slots.emplace_front();
            }
            _base_idx = idx;
            if constexpr (is_nullable<T>) {
                _slots.front() = std::move(value);
            } else {
                _slots.front().emplace(std::move(value));
            }
        } else {
            auto off = static_cast<size_t>(idx - _base_idx);
            if (off >= _slots.size()) {
                // Extend to cover off, default-constructing new slots.
                _slots.resize(off + 1);
            }
            SEASTAR_ASSERT(!slot_occupied(_slots[off]));
            if constexpr (is_nullable<T>) {
                _slots[off] = std::move(value);
            } else {
                _slots[off].emplace(std::move(value));
            }
        }
        ++_count;
        auto off = static_cast<size_t>(idx - _base_idx);
        if constexpr (is_nullable<T>) {
            return _slots[off];
        } else {
            return *_slots[off];
        }
    }

    /// Removes and returns the value at the given index.
    /// Returns a default-constructed T if the slot is absent or empty.
    T take(IndexType idx) noexcept {
        if (_slots.empty() || idx < _base_idx) {
            return T{};
        }
        auto off = static_cast<size_t>(idx - _base_idx);
        if (off >= _slots.size() || !slot_occupied(_slots[off])) {
            return T{};
        }
        T value;
        if constexpr (is_nullable<T>) {
            value = std::move(_slots[off]);
            _slots[off] = T{};
        } else {
            value = std::move(*_slots[off]);
            _slots[off].reset();
        }
        --_count;
        trim_front();
        return value;
    }

    /// Clears the slot at the given index. The slot must be occupied.
    void clear_at(IndexType idx) noexcept {
        SEASTAR_ASSERT(idx >= _base_idx);
        auto off = static_cast<size_t>(idx - _base_idx);
        SEASTAR_ASSERT(off < _slots.size() && slot_occupied(_slots[off]));
        _slots[off] = slot_t{};
        --_count;
    }

    /// Removes empty slots from the front of the deque,
    /// advancing base_index() accordingly.
    void trim_front() noexcept {
        while (!_slots.empty() && !slot_occupied(_slots.front())) {
            _slots.pop_front();
            ++_base_idx;
        }
    }

    /// Returns true if there are no slots (occupied or gap).
    [[nodiscard]] bool empty() const noexcept {
        return _slots.empty();
    }

    /// Returns the number of occupied (non-empty) slots.
    [[nodiscard]] size_t size() const noexcept {
        return _count;
    }

    /// Returns the base index (the index corresponding to _slots[0]).
    IndexType base_index() const noexcept {
        return _base_idx;
    }

    /// Iterates over all occupied slots, calling f(IndexType, T&) for each.
    /// The callback may call clear_at() on the current element, but must
    /// not otherwise modify the container during iteration.
    /// trim_front() is called automatically after iteration.
    template<typename F>
    requires std::invocable<F, IndexType, T&>
    void for_each(F&& f) {
        IndexType idx = _base_idx;
        for (size_t i = 0; i < _slots.size(); ++i, ++idx) {
            if (slot_occupied(_slots[i])) {
                if constexpr (is_nullable<T>) {
                    f(idx, _slots[i]);
                } else {
                    f(idx, *_slots[i]);
                }
            }
        }
        trim_front();
    }

    /// Clears all slots and resets the base index to IndexType{}.
    void clear() noexcept {
        _slots.clear();
        _base_idx = IndexType{};
        _count = 0;
    }
};

} // namespace seastar

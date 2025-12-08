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
 * Copyright (C) 2025 ScyllaDB
 */

#pragma once

#include <array>

namespace seastar {
namespace internal {

template <typename T, unsigned F, T* T::*Member>
class intrusive_split_list {
    std::array<T*, F> _first;
    std::array<T**, F> _last_p;
    unsigned _tail_idx;
    unsigned _head_idx;

public:
    intrusive_split_list() noexcept
            : _tail_idx(0)
            , _head_idx(0)
    {
        for (unsigned i = 0; i < F; i++) {
            _first[i] = nullptr;
            _last_p[i] = &_first[i];
        }
    }

    intrusive_split_list(const intrusive_split_list&) = delete;
    intrusive_split_list(intrusive_split_list&&) = delete;

    void push_back(T* element) noexcept {
        auto i = _tail_idx++ % F;
        *_last_p[i] = element;
        _last_p[i] = &(element->*Member);
        element->*Member = nullptr;
    }

    void push_front(T* element) noexcept {
        auto i = --_head_idx % F;
        element->*Member = _first[i];
        _first[i] = element;
        if (_last_p[i] == &_first[i]) {
            _last_p[i] = &(element->*Member);
        }
    }

    bool empty() const noexcept {
        return _head_idx == _tail_idx;
    }

    size_t size() const noexcept {
        return _tail_idx - _head_idx;
    }

    T* pop_front() noexcept {
        auto i = _head_idx++ % F;
        T* ret = _first[i];
        _first[i] = _first[i]->*Member;
        if (_last_p[i] == &(ret->*Member)) {
            _last_p[i] = &_first[i];
        }
        return ret;
    }

    class iterator {
        std::array<T*, F> _cur;
        unsigned _idx;

    public:
        iterator(unsigned idx) noexcept
                : _cur({nullptr})
                , _idx(idx)
        {}

        iterator(const intrusive_split_list& sl) noexcept
                : _cur(sl._first)
                , _idx(sl._head_idx)
        {}

        iterator& operator++() noexcept {
            _cur[_idx % F] = _cur[_idx % F]->*Member;
            _idx++;
            return *this;
        }

        T* operator->() noexcept {
            return _cur[_idx % F];
        }

        T& operator*() noexcept {
            return *_cur[_idx % F];
        }

        bool operator==(const iterator& o) noexcept {
            return _idx == o._idx;
        }

        bool operator!=(const iterator& o) noexcept {
            return _idx != o._idx;
        }
    };

    iterator begin() const noexcept { return iterator(*this); }
    iterator end() const noexcept { return iterator(_tail_idx); }
};

} // internal namespace
} // seastar namespace

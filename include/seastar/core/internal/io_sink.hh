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
 * Copyright 2021 ScyllaDB
 */

#pragma once

#include <seastar/core/chunked_fifo.hh>
#include <seastar/core/internal/io_request.hh>

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace seastar {

class io_completion;

namespace internal {

class io_sink;

class pending_io_request : private internal::io_request {
    friend class io_sink;
    io_completion* _completion;

public:
    pending_io_request(internal::io_request req, io_completion* desc) noexcept
            : io_request(std::move(req))
            , _completion(desc)
    { }
};

class io_sink {
    chunked_fifo<pending_io_request> _pending_io;
public:
    void submit(io_completion* desc, internal::io_request req) noexcept;

    template <typename Fn>
    // Fn should return whether the request was consumed and
    // draining should try to drain more
    requires std::is_invocable_r<bool, Fn, internal::io_request&, io_completion*>::value
    size_t drain(Fn&& consume) {
        size_t drained = 0;

        for (auto& req : _pending_io) {
            if (!consume(req, req._completion)) {
                break;
            }
            drained++;
        }

        _pending_io.pop_front_n(drained);
        return drained;
    }
};

} // namespace internal

} // namespace seastar

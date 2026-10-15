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
 * Copyright (C) 2018 ScyllaDB
 */


#include <ranges>

#include <seastar/core/sharded.hh>
#include <seastar/core/loop.hh>

namespace seastar {

namespace internal {


future<>
sharded_on_all_shards(unsigned nr_shards, smp_submit_to_options options, std::function<future<> ()> func) noexcept {
    // parallel_for_each() calls the lambda for every shard before returning,
    // so all copies of func are made while func is still alive.
    return parallel_for_each(std::views::iota(0u, nr_shards), [&func, options] (unsigned c) {
        return smp::submit_to(c, options, [func] {
            return func();
        });
    });
}

future<>
sharded_start(unsigned nr_shards, std::function<future<> ()> start_one,
        void* self, future<> (*stop)(void* self)) noexcept {
    return sharded_on_all_shards(nr_shards, smp_submit_to_options{}, std::move(start_one)).handle_exception([self, stop] (std::exception_ptr e) {
        return stop(self).then([e = std::move(e)] () mutable {
            std::rethrow_exception(std::move(e));
        });
    });
}

future<>
sharded_stop(unsigned nr_shards, void* self, future<> (*stop_one)(void* self),
        future<> (*delete_one)(void* self), void (*clear)(void* self)) noexcept {
    return sharded_on_all_shards(nr_shards, smp_submit_to_options{}, [self, stop_one] {
        return stop_one(self);
    }).then_wrapped([nr_shards, self, delete_one, clear] (future<> fut) {
        return sharded_on_all_shards(nr_shards, smp_submit_to_options{}, [self, delete_one] {
            return delete_one(self);
        }).finally([self, clear, fut = std::move(fut)] () mutable {
            clear(self);
            return std::move(fut);
        });
    });
}

}

}

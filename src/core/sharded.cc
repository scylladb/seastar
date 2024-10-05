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

#ifdef SEASTAR_MODULE
module;
#endif

#include <ranges>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/sharded.hh>
#include <seastar/core/loop.hh>
#endif

namespace seastar {

namespace internal {


future<>
sharded_parallel_for_each(unsigned nr_shards, on_each_shard_func on_each_shard) noexcept(std::is_nothrow_move_constructible_v<on_each_shard_func>) {
    return parallel_for_each(std::views::iota(0u, nr_shards), std::move(on_each_shard));
}

}

}

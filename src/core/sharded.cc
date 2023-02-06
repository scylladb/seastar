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

#include <seastar/core/sharded.hh>
#include <seastar/core/loop.hh>
#include <boost/range/irange.hpp>

namespace seastar {

namespace internal {


future<>
sharded_parallel_for_each(unsigned nr_shards, on_each_shard_func on_each_shard) noexcept(std::is_nothrow_move_constructible_v<on_each_shard_func>) {
    return parallel_for_each(boost::irange<unsigned>(0, nr_shards), std::move(on_each_shard));
}

}

}

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
 * Copyright (C) 2026 ScyllaDB Ltd.
 */

#include <seastar/core/memory.hh>
#include <seastar/core/smp.hh>
#include <seastar/testing/test_case.hh>

using namespace seastar;

// Run with -m 512M; total_memory() must reflect the per-shard share, not a fixed 1 GiB.
SEASTAR_TEST_CASE(test_stats_report_the_configured_memory) {
    const size_t configured_per_shard = (size_t(512) << 20) / this_smp_shard_count();
    const size_t total = memory::stats().total_memory();

    BOOST_REQUIRE_LE(total, configured_per_shard);
    // The allocator keeps a little of the share for itself, but not most of it.
    BOOST_REQUIRE_GT(total, configured_per_shard / 2);
    return make_ready_future<>();
}

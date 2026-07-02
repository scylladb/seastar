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
 * Copyright (C) 2026 ScyllaDB
 */

//
// Measures the per-call overhead of a dormant LTTng-UST tracepoint.
//
// Run with no active lttng session to measure the not-taken-branch cost:
//
//   ./lttng_overhead_perf
//
// To also observe the active-session overhead, start an lttng session first:
//
//   lttng create; lttng enable-event -u 'seastar_tp_bench:*'; lttng start
//   ./lttng_overhead_perf
//   lttng stop; lttng destroy
//

#define TRACEPOINT_CREATE_PROBES
#define TRACEPOINT_DEFINE
#include "lttng_overhead_tp.h"

#include <chrono>
#include <cstdint>
#include <cstdio>

static constexpr uint64_t WARMUP = 1'000'000;
static constexpr uint64_t N      = 100'000'000;

// Prevents the loop from being optimized away without adding real work.
// Both baseline and tracepoint loops use it so it cancels out in the diff.
static inline void touch(uint64_t i) {
    __asm__ volatile("" : : "r"(i) : );
}

[[gnu::noinline]]
static double measure_baseline() {
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N; i++) {
        touch(i);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
}

[[gnu::noinline]]
static double measure_tracepoint() {
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N; i++) {
        tracepoint(seastar_tp_bench, nop);
        touch(i);
    }
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
}

int main() {
    // Warmup: bring the probe-state cache line into L1 and stabilise branch
    // predictor before we start measuring.
    for (uint64_t i = 0; i < WARMUP; i++) {
        tracepoint(seastar_tp_bench, nop);
    }

    const double baseline_ns = measure_baseline();
    const double trace_ns    = measure_tracepoint();
    const double overhead_ns = trace_ns - baseline_ns;

    printf("iterations:  %lu\n", N);
    printf("baseline:    %.3f ns/call  (empty loop with compiler fence)\n", baseline_ns);
    printf("tracepoint:  %.3f ns/call  (dormant, no active lttng session)\n", trace_ns);
    printf("overhead:    %.3f ns/call\n", overhead_ns);

    return 0;
}

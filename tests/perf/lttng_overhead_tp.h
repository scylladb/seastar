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

// LTTng-UST tracepoint provider for the dormant-overhead benchmark.
// Defines a single no-payload tracepoint to measure the pure probe-check cost.

#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER seastar_tp_bench

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "lttng_overhead_tp.h"

#if !defined(_LTTNG_OVERHEAD_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _LTTNG_OVERHEAD_TP_H

#include <lttng/tracepoint.h>

// A minimal tracepoint with no payload — isolates the probe-check overhead.
TRACEPOINT_EVENT(
    seastar_tp_bench,
    nop,
    TP_ARGS(),
    TP_FIELDS()
)

#endif // _LTTNG_OVERHEAD_TP_H

#include <lttng/tracepoint-event.h>

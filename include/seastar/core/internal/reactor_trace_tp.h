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

#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER seastar_reactor

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "seastar/core/internal/reactor_trace_tp.h"

#if !defined(_REACTOR_TRACE_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _REACTOR_TRACE_TP_H

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(
    seastar_reactor,
    run_tasks_start,
    TP_ARGS(),
    TP_FIELDS()
)

TRACEPOINT_EVENT(
    seastar_reactor,
    run_tasks_end,
    TP_ARGS(),
    TP_FIELDS()
)

TRACEPOINT_EVENT(
    seastar_reactor,
    task_start,
    TP_ARGS(uint32_t, sched_group_id, const char *, sched_group_name),
    TP_FIELDS(
        ctf_integer(uint32_t, sched_group_id, sched_group_id)
        ctf_string(sched_group_name, sched_group_name)
    )
)

TRACEPOINT_EVENT(
    seastar_reactor,
    task_end,
    TP_ARGS(uint32_t, sched_group_id, uint64_t, tasks_processed),
    TP_FIELDS(
        ctf_integer(uint32_t, sched_group_id, sched_group_id)
        ctf_integer(uint64_t, tasks_processed, tasks_processed)
    )
)

#endif

#include <lttng/tracepoint-event.h>

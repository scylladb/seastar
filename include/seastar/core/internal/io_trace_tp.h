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
 *
 * LTTng-UST tracepoint provider for Seastar IO subsystem.
 *
 * This header follows the LTTng-UST tracepoint provider convention
 * and must be included in exactly one compilation unit with
 * TRACEPOINT_CREATE_PROBES defined.
 */

#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER seastar_io

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "seastar/core/internal/io_trace_tp.h"

#if !defined(_SEASTAR_IO_TRACE_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _SEASTAR_IO_TRACE_TP_H

#include <lttng/tracepoint.h>

/*
 * io_queue_queued: an IO request has been enqueued into the fair queue.
 */
TRACEPOINT_EVENT(
    seastar_io,
    io_queue_queued,
    TP_ARGS(
        int,          fd,         /* file descriptor of the request */
        const void *, req_id,     /* unique request identifier (pointer) */
        int, direction,           /* 0 = read, 1 = write */
        unsigned int, pclass,     /* scheduling group / priority class id */
        uint64_t, offset,         /* file offset of the request */
        uint64_t, length          /* request size in bytes */
    ),
    TP_FIELDS(
        ctf_integer(int, fd, fd)
        ctf_integer_hex(unsigned long, req_id, (unsigned long)req_id)
        ctf_integer(int, direction, direction)
        ctf_integer(unsigned int, pclass, pclass)
        ctf_integer(uint64_t, offset, offset)
        ctf_integer(uint64_t, length, length)
    )
)

/*
 * io_queue_dispatched: a request has been dispatched to the IO backend.
 */
TRACEPOINT_EVENT(
    seastar_io,
    io_queue_dispatched,
    TP_ARGS(
        const void *, req_id        /* unique request identifier (pointer) */
    ),
    TP_FIELDS(
        ctf_integer_hex(unsigned long, req_id, (unsigned long)req_id)
    )
)

/*
 * io_queue_completed: a request has completed successfully.
 */
TRACEPOINT_EVENT(
    seastar_io,
    io_queue_completed,
    TP_ARGS(
        const void *, req_id        /* unique request identifier (pointer) */
    ),
    TP_FIELDS(
        ctf_integer_hex(unsigned long, req_id, (unsigned long)req_id)
    )
)

/*
 * io_queue_cancelled: a request was cancelled before dispatch.
 */
TRACEPOINT_EVENT(
    seastar_io,
    io_queue_cancelled,
    TP_ARGS(
        const void *, req_id        /* unique request identifier (pointer) */
    ),
    TP_FIELDS(
        ctf_integer_hex(unsigned long, req_id, (unsigned long)req_id)
    )
)

#endif /* _SEASTAR_IO_TRACE_TP_H */

#include <lttng/tracepoint-event.h>

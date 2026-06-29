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

#pragma once

#ifdef SEASTAR_HAVE_LTTNG_UST
#include <seastar/core/internal/io_trace_tp.h>
#define SEASTAR_IO_TRACE(...) tracepoint(seastar_io, __VA_ARGS__)
#else
#define SEASTAR_IO_TRACE(...) do {} while(0)
#endif

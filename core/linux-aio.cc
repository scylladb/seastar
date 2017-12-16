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
 * Copyright (C) 2017 ScyllaDB
 */

#include "linux-aio.hh"
#include <unistd.h>
#include <sys/syscall.h>

namespace seastar {

namespace internal {

int io_setup(int nr_events, ::aio_context_t* io_context) {
    return ::syscall(SYS_io_setup, nr_events, io_context);
}

int io_destroy(::aio_context_t io_context) {
   return ::syscall(SYS_io_destroy, io_context);
}

int io_submit(::aio_context_t io_context, long nr, ::iocb** iocbs) {
    return ::syscall(SYS_io_submit, io_context, nr, iocbs);
}

int io_cancel(::aio_context_t io_context, ::iocb* iocb, ::io_event* result) {
    return ::syscall(SYS_io_cancel, io_context, iocb, result);
}

int io_getevents(::aio_context_t io_context, long min_nr, long nr, ::io_event* events, const ::timespec* timeout) {
    return ::syscall(SYS_io_getevents, io_context, min_nr, nr, events, timeout);
}

}

}

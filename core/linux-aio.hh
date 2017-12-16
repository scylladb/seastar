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

#pragma once

#include <linux/aio_abi.h>
#include <linux/fs.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <cstdint>

namespace seastar {

namespace internal {

::iocb make_read_iocb(int fd, uint64_t offset, void* buffer, size_t len);
::iocb make_write_iocb(int fd, uint64_t offset, const void* buffer, size_t len);
::iocb make_readv_iocb(int fd, uint64_t offset, const ::iovec* iov, size_t niov);
::iocb make_writev_iocb(int fd, uint64_t offset, const ::iovec* iov, size_t niov);

void set_user_data(::iocb& iocb, void* data);
void* get_user_data(const ::iocb& iocb);
void set_nowait(::iocb& iocb, bool nowait);

void set_eventfd_notification(::iocb& iocb, int eventfd);

::iocb* get_iocb(const ::io_event& ioev);

int io_setup(int nr_events, ::aio_context_t* io_context);
int io_destroy(::aio_context_t io_context);
int io_submit(::aio_context_t io_context, long nr, ::iocb** iocbs);
int io_cancel(::aio_context_t io_context, ::iocb* iocb, ::io_event* result);
int io_getevents(::aio_context_t io_context, long min_nr, long nr, ::io_event* events, const ::timespec* timeout);

}

namespace internal {

inline
::iocb
make_read_iocb(int fd, uint64_t offset, void* buffer, size_t len) {
    ::iocb iocb{};
    iocb.aio_lio_opcode = IOCB_CMD_PREAD;
    iocb.aio_fildes = fd;
    iocb.aio_offset = offset;
    iocb.aio_buf = reinterpret_cast<uintptr_t>(buffer);
    iocb.aio_nbytes = len;
    return iocb;
}

inline
::iocb
make_write_iocb(int fd, uint64_t offset, const void* buffer, size_t len) {
    ::iocb iocb{};
    iocb.aio_lio_opcode = IOCB_CMD_PWRITE;
    iocb.aio_fildes = fd;
    iocb.aio_offset = offset;
    iocb.aio_buf = reinterpret_cast<uintptr_t>(buffer);
    iocb.aio_nbytes = len;
    return iocb;
}

inline
::iocb
make_readv_iocb(int fd, uint64_t offset, const ::iovec* iov, size_t niov) {
    ::iocb iocb{};
    iocb.aio_lio_opcode = IOCB_CMD_PREADV;
    iocb.aio_fildes = fd;
    iocb.aio_offset = offset;
    iocb.aio_buf = reinterpret_cast<uintptr_t>(iov);
    iocb.aio_nbytes = niov;
    return iocb;
}

inline
::iocb
make_writev_iocb(int fd, uint64_t offset, const ::iovec* iov, size_t niov) {
    ::iocb iocb{};
    iocb.aio_lio_opcode = IOCB_CMD_PWRITEV;
    iocb.aio_fildes = fd;
    iocb.aio_offset = offset;
    iocb.aio_buf = reinterpret_cast<uintptr_t>(iov);
    iocb.aio_nbytes = niov;
    return iocb;
}

inline
void
set_user_data(::iocb& iocb, void* data) {
    iocb.aio_data = reinterpret_cast<uintptr_t>(data);
}

inline
void*
get_user_data(const ::iocb& iocb) {
    return reinterpret_cast<void*>(uintptr_t(iocb.aio_data));
}

inline
void
set_eventfd_notification(::iocb& iocb, int eventfd) {
    iocb.aio_flags |= IOCB_FLAG_RESFD;
    iocb.aio_resfd = eventfd;
}

inline
::iocb*
get_iocb(const ::io_event& ev) {
    return reinterpret_cast<::iocb*>(uintptr_t(ev.obj));
}

inline
void
set_nowait(::iocb& iocb, bool nowait) {
#ifdef RWF_NOWAIT
    if (nowait) {
        iocb.aio_rw_flags |= RWF_NOWAIT;
    } else {
        iocb.aio_rw_flags &= ~RWF_NOWAIT;
    }
#endif
}

}


}


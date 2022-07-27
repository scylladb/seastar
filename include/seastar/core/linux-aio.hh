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

#include <endian.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <signal.h>
#include <cstdint>

namespace seastar {

namespace internal {

namespace linux_abi {

using aio_context_t = unsigned long;

enum class iocb_cmd : uint16_t {
    PREAD = 0,
    PWRITE = 1,
    FSYNC = 2,
    FDSYNC = 3,
    POLL = 5,
    NOOP = 6,
    PREADV = 7,
    PWRITEV = 8,
};

struct io_event {
    uint64_t data;
    uint64_t obj;
    int64_t res;
    int64_t res2;
};

constexpr int IOCB_FLAG_RESFD = 1;

struct iocb {
        uint64_t   aio_data;

#if __BYTE_ORDER == __LITTLE_ENDIAN
        uint32_t   aio_key;
        int32_t aio_rw_flags;
#elif __BYTE_ORDER == __BIG_ENDIAN
        int32_t aio_rw_flags;
        uint32_t   aio_key;
#else
#error bad byteorder
#endif

        iocb_cmd   aio_lio_opcode;
        int16_t   aio_reqprio;
        uint32_t   aio_fildes;

        uint64_t   aio_buf;
        uint64_t   aio_nbytes;
        int64_t   aio_offset;

        uint64_t   aio_reserved2;

        uint32_t   aio_flags;

        uint32_t   aio_resfd;
};

struct aio_sigset {
    const sigset_t *sigmask;
    size_t sigsetsize;
};

}

linux_abi::iocb make_read_iocb(int fd, uint64_t offset, void* buffer, size_t len);
linux_abi::iocb make_write_iocb(int fd, uint64_t offset, const void* buffer, size_t len);
linux_abi::iocb make_readv_iocb(int fd, uint64_t offset, const ::iovec* iov, size_t niov);
linux_abi::iocb make_writev_iocb(int fd, uint64_t offset, const ::iovec* iov, size_t niov);
linux_abi::iocb make_poll_iocb(int fd, uint32_t events);

void set_user_data(linux_abi::iocb& iocb, void* data);
void set_nowait(linux_abi::iocb& iocb, bool nowait);

void set_eventfd_notification(linux_abi::iocb& iocb, int eventfd);

linux_abi::iocb* get_iocb(const linux_abi::io_event& ioev);

int io_setup(int nr_events, linux_abi::aio_context_t* io_context);
int io_destroy(linux_abi::aio_context_t io_context) noexcept;
int io_submit(linux_abi::aio_context_t io_context, long nr, linux_abi::iocb** iocbs);
int io_cancel(linux_abi::aio_context_t io_context, linux_abi::iocb* iocb, linux_abi::io_event* result);
int io_getevents(linux_abi::aio_context_t io_context, long min_nr, long nr, linux_abi::io_event* events, const ::timespec* timeout,
        bool force_syscall = false);
int io_pgetevents(linux_abi::aio_context_t io_context, long min_nr, long nr, linux_abi::io_event* events, const ::timespec* timeout, const sigset_t* sigmask,
        bool force_syscall = false);

void setup_aio_context(size_t nr, linux_abi::aio_context_t* io_context);

}

extern bool aio_nowait_supported;

namespace internal {

inline
linux_abi::iocb
make_read_iocb(int fd, uint64_t offset, void* buffer, size_t len) {
    linux_abi::iocb iocb{};
    iocb.aio_lio_opcode = linux_abi::iocb_cmd::PREAD;
    iocb.aio_fildes = fd;
    iocb.aio_offset = offset;
    iocb.aio_buf = reinterpret_cast<uintptr_t>(buffer);
    iocb.aio_nbytes = len;
    return iocb;
}

inline
linux_abi::iocb
make_write_iocb(int fd, uint64_t offset, const void* buffer, size_t len) {
    linux_abi::iocb iocb{};
    iocb.aio_lio_opcode = linux_abi::iocb_cmd::PWRITE;
    iocb.aio_fildes = fd;
    iocb.aio_offset = offset;
    iocb.aio_buf = reinterpret_cast<uintptr_t>(buffer);
    iocb.aio_nbytes = len;
    return iocb;
}

inline
linux_abi::iocb
make_readv_iocb(int fd, uint64_t offset, const ::iovec* iov, size_t niov) {
    linux_abi::iocb iocb{};
    iocb.aio_lio_opcode = linux_abi::iocb_cmd::PREADV;
    iocb.aio_fildes = fd;
    iocb.aio_offset = offset;
    iocb.aio_buf = reinterpret_cast<uintptr_t>(iov);
    iocb.aio_nbytes = niov;
    return iocb;
}

inline
linux_abi::iocb
make_writev_iocb(int fd, uint64_t offset, const ::iovec* iov, size_t niov) {
    linux_abi::iocb iocb{};
    iocb.aio_lio_opcode = linux_abi::iocb_cmd::PWRITEV;
    iocb.aio_fildes = fd;
    iocb.aio_offset = offset;
    iocb.aio_buf = reinterpret_cast<uintptr_t>(iov);
    iocb.aio_nbytes = niov;
    return iocb;
}

inline
linux_abi::iocb
make_poll_iocb(int fd, uint32_t events) {
    linux_abi::iocb iocb{};
    iocb.aio_lio_opcode = linux_abi::iocb_cmd::POLL;
    iocb.aio_fildes = fd;
    iocb.aio_buf = events;
    return iocb;
}

inline
linux_abi::iocb
make_fdsync_iocb(int fd) {
    linux_abi::iocb iocb{};
    iocb.aio_lio_opcode = linux_abi::iocb_cmd::FDSYNC;
    iocb.aio_fildes = fd;
    return iocb;
}

inline
void
set_user_data(linux_abi::iocb& iocb, void* data) {
    iocb.aio_data = reinterpret_cast<uintptr_t>(data);
}

template <typename T>
inline T* get_user_data(const linux_abi::iocb& iocb) noexcept {
    return reinterpret_cast<T*>(uintptr_t(iocb.aio_data));
}

template <typename T>
inline T* get_user_data(const linux_abi::io_event& ev) noexcept {
    return reinterpret_cast<T*>(uintptr_t(ev.data));
}

inline
void
set_eventfd_notification(linux_abi::iocb& iocb, int eventfd) {
    iocb.aio_flags |= linux_abi::IOCB_FLAG_RESFD;
    iocb.aio_resfd = eventfd;
}

inline
linux_abi::iocb*
get_iocb(const linux_abi::io_event& ev) {
    return reinterpret_cast<linux_abi::iocb*>(uintptr_t(ev.obj));
}

inline
void
set_nowait(linux_abi::iocb& iocb, bool nowait) {
#ifdef RWF_NOWAIT
    if (aio_nowait_supported) {
        if (nowait) {
            iocb.aio_rw_flags |= RWF_NOWAIT;
        } else {
            iocb.aio_rw_flags &= ~RWF_NOWAIT;
        }
    }
#endif
}

}


}


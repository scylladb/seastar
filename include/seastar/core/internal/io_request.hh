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
 * Copyright 2020 ScyllaDB
 */

#pragma once

#include <seastar/core/sstring.hh>
#include <seastar/core/on_internal_error.hh>
#include <seastar/util/assert.hh>
#include <cstdint>
#include <vector>
#include <sys/types.h>
#include <sys/socket.h>

namespace seastar {
extern logger io_log;

namespace internal {

class io_request {
public:
    enum class operation : char { read, readv, write, writev, fdatasync, recv, recvmsg, send, sendmsg, accept, connect, poll_add, poll_remove, cancel };
private:
    // the upper layers give us void pointers, but storing void pointers here is just
    // dangerous. The constructors seem to be happy to convert other pointers to void*,
    // even if they are marked as explicit, and then you end up losing approximately 3 hours
    // and 15 minutes (hypothetically, of course), trying to chase the weirdest bug.
    // Let's store a char* for safety, and cast it back to void* in the accessor.
    struct read_op {
        operation op;
        bool nowait_works;
        int fd;
        uint64_t pos;
        char* addr;
        size_t size;
    };
    struct readv_op {
        operation op;
        bool nowait_works;
        int fd;
        uint64_t pos;
        ::iovec* iovec;
        size_t iov_len;
    };
    struct recv_op {
        operation op;
        int fd;
        char* addr;
        size_t size;
        int flags;
    };
    struct recvmsg_op {
        operation op;
        int fd;
        ::msghdr* msghdr;
        int flags;
    };
    using send_op = recv_op;
    using sendmsg_op = recvmsg_op;
    using write_op = read_op;
    using writev_op = readv_op;
    struct fdatasync_op {
        operation op;
        int fd;
    };
    struct accept_op {
        operation op;
        int fd;
        ::sockaddr* sockaddr;
        socklen_t* socklen_ptr;
        int flags;
    };
    struct connect_op {
        operation op;
        int fd;
        ::sockaddr* sockaddr;
        socklen_t socklen;
    };
    struct poll_add_op {
        operation op;
        int fd;
        int events;
    };
    struct poll_remove_op {
        operation op;
        int fd;
        char* addr;
    };
    struct cancel_op {
        operation op;
        int fd;
        char* addr;
    };

    union {
        read_op _read;
        readv_op _readv;
        recv_op _recv;
        recvmsg_op _recvmsg;
        send_op _send;
        sendmsg_op _sendmsg;
        write_op _write;
        writev_op _writev;
        fdatasync_op _fdatasync;
        accept_op _accept;
        connect_op _connect;
        poll_add_op _poll_add;
        poll_remove_op _poll_remove;
        cancel_op _cancel;
    };

public:
    static io_request make_read(int fd, uint64_t pos, void* address, size_t size, bool nowait_works) {
        io_request req;
        req._read = {
          .op = operation::read,
          .nowait_works = nowait_works,
          .fd = fd,
          .pos = pos,
          .addr = reinterpret_cast<char*>(address),
          .size = size,
        };
        return req;
    }

    static io_request make_readv(int fd, uint64_t pos, std::vector<iovec>& iov, bool nowait_works) {
        io_request req;
        req._readv = {
          .op = operation::readv,
          .nowait_works = nowait_works,
          .fd = fd,
          .pos = pos,
          .iovec = iov.data(),
          .iov_len = iov.size(),
        };
        return req;
    }

    static io_request make_recv(int fd, void* address, size_t size, int flags) {
        io_request req;
        req._recv = {
          .op = operation::recv,
          .fd = fd,
          .addr = reinterpret_cast<char*>(address),
          .size = size,
          .flags = flags,
        };
        return req;
    }

    static io_request make_recvmsg(int fd, ::msghdr* msg, int flags) {
        io_request req;
        req._recvmsg = {
          .op = operation::recvmsg,
          .fd = fd,
          .msghdr = msg,
          .flags = flags,
        };
        return req;
    }

    static io_request make_send(int fd, const void* address, size_t size, int flags) {
        io_request req;
        req._send = {
          .op = operation::send,
          .fd = fd,
          .addr = const_cast<char*>(reinterpret_cast<const char*>(address)),
          .size = size,
          .flags = flags,
        };
        return req;
    }

    static io_request make_sendmsg(int fd, ::msghdr* msg, int flags) {
        io_request req;
        req._sendmsg = {
          .op = operation::sendmsg,
          .fd = fd,
          .msghdr = msg,
          .flags = flags,
        };
        return req;
    }

    static io_request make_write(int fd, uint64_t pos, const void* address, size_t size, bool nowait_works) {
        io_request req;
        req._write = {
          .op = operation::write,
          .nowait_works = nowait_works,
          .fd = fd,
          .pos = pos,
          .addr = const_cast<char*>(reinterpret_cast<const char*>(address)),
          .size = size,
        };
        return req;
    }

    static io_request make_writev(int fd, uint64_t pos, std::vector<iovec>& iov, bool nowait_works) {
        io_request req;
        req._writev = {
          .op = operation::writev,
          .nowait_works = nowait_works,
          .fd = fd,
          .pos = pos,
          .iovec = iov.data(),
          .iov_len = iov.size(),
        };
        return req;
    }

    static io_request make_fdatasync(int fd) {
        io_request req;
        req._fdatasync = {
          .op = operation::fdatasync,
          .fd = fd,
        };
        return req;
    }

    static io_request make_accept(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags) {
        io_request req;
        req._accept = {
          .op = operation::accept,
          .fd = fd,
          .sockaddr = addr,
          .socklen_ptr = addrlen,
          .flags = flags,
        };
        return req;
    }

    static io_request make_connect(int fd, struct sockaddr* addr, socklen_t addrlen) {
        io_request req;
        req._connect = {
          .op = operation::connect,
          .fd = fd,
          .sockaddr = addr,
          .socklen = addrlen,
        };
        return req;
    }

    static io_request make_poll_add(int fd, int events) {
        io_request req;
        req._poll_add = {
          .op = operation::poll_add,
          .fd = fd,
          .events = events,
        };
        return req;
    }

    static io_request make_poll_remove(int fd, void *addr) {
        io_request req;
        req._poll_remove = {
          .op = operation::poll_remove,
          .fd = fd,
          .addr = reinterpret_cast<char*>(addr),
        };
        return req;
    }

    static io_request make_cancel(int fd, void *addr) {
        io_request req;
        req._cancel = {
          .op = operation::cancel,
          .fd = fd,
          .addr = reinterpret_cast<char*>(addr),
        };
        return req;
    }

    bool is_read() const {
        switch (opcode()) {
        case operation::read:
        case operation::readv:
        case operation::recvmsg:
        case operation::recv:
            return true;
        default:
            return false;
        }
    }

    bool is_write() const {
        switch (opcode()) {
        case operation::write:
        case operation::writev:
        case operation::send:
        case operation::sendmsg:
            return true;
        default:
            return false;
        }
    }

    sstring opname() const;

    // All operation variants are tagged unions with an operation as
    // the first member, which we read through the _read union member
    // (chosen arbitrarily) which is allowed by the common-initial-subsequence rule.
    operation opcode() const {
        return _read.op;
    }

    template <operation Op>
    auto& as() const {
        if constexpr (Op == operation::read) {
            return _read;
        }
        if constexpr (Op == operation::readv) {
            return _readv;
        }
        if constexpr (Op == operation::recv) {
            return _recv;
        }
        if constexpr (Op == operation::recvmsg) {
            return _recvmsg;
        }
        if constexpr (Op == operation::send) {
            return _send;
        }
        if constexpr (Op == operation::sendmsg) {
            return _sendmsg;
        }
        if constexpr (Op == operation::write) {
            return _write;
        }
        if constexpr (Op == operation::writev) {
            return _writev;
        }
        if constexpr (Op == operation::fdatasync) {
            return _fdatasync;
        }
        if constexpr (Op == operation::accept) {
            return _accept;
        }
        if constexpr (Op == operation::connect) {
            return _connect;
        }
        if constexpr (Op == operation::poll_add) {
            return _poll_add;
        }
        if constexpr (Op == operation::poll_remove) {
            return _poll_remove;
        }
        if constexpr (Op == operation::cancel) {
            return _cancel;
        }
    }

    struct part;
    std::vector<part> split(size_t max_length);

private:
    io_request sub_req_buffer(size_t pos, size_t len) const {
        io_request sub_req;
        // read_op and write_op share the same layout, so we don't handle
        // them separately
        auto& op = _read;
        auto& sub_op = sub_req._read;
        sub_op = {
          .op = op.op,
          .nowait_works = op.nowait_works,
          .fd = op.fd,
          .pos = op.pos + pos,
          .addr = op.addr + pos,
          .size = len,
        };
        return sub_req;
    }
    std::vector<part> split_buffer(size_t max_length);

    io_request sub_req_iovec(size_t pos, std::vector<iovec>& iov) const {
        io_request sub_req;
        // readv_op and writev_op share the same layout, so we don't handle
        // them separately
        auto& op = _readv;
        auto& sub_op = sub_req._readv;
        sub_op = {
          .op = op.op,
          .nowait_works = op.nowait_works,
          .fd = op.fd,
          .pos = op.pos + pos,
          .iovec = iov.data(),
          .iov_len = iov.size(),
        };
        return sub_req;
    }
    std::vector<part> split_iovec(size_t max_length);
};

struct io_request::part {
    io_request req;
    size_t size;
    std::vector<::iovec> iovecs;
};

// Helper pair of IO direction and length
struct io_direction_and_length {
    size_t _directed_length; // bit 0 is R/W flag

public:
    size_t length() const noexcept { return _directed_length >> 1; }
    int rw_idx() const noexcept { return _directed_length & 0x1; }
    static constexpr int read_idx = 1;
    static constexpr int write_idx = 0;

    io_direction_and_length(int idx, size_t val) noexcept
            : _directed_length((val << 1) | idx)
    {
        SEASTAR_ASSERT(idx == read_idx || idx == write_idx);
    }
};

}
}

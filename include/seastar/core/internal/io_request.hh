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
#include <seastar/core/linux-aio.hh>
#include <seastar/core/internal/io_desc.hh>
#include <seastar/core/on_internal_error.hh>
#include <sys/types.h>
#include <sys/socket.h>

namespace seastar {
extern logger io_log;

namespace internal {

class io_request {
public:
    enum class operation { read, readv, write, writev, fdatasync, recv, recvmsg, send, sendmsg, accept, connect, poll_add, poll_remove, cancel };
private:
    operation _op;
    // the upper layers give us void pointers, but storing void pointers here is just
    // dangerous. The constructors seem to be happy to convert other pointers to void*,
    // even if they are marked as explicit, and then you end up losing approximately 3 hours
    // and 15 minutes (hypothetically, of course), trying to chase the weirdest bug.
    // Let's store a char* for safety, and cast it back to void* in the accessor.
    struct read_op {
        int fd;
        uint64_t pos;
        char* addr;
        size_t size;
        bool nowait_works;
    };
    struct readv_op {
        int fd;
        uint64_t pos;
        ::iovec* iovec;
        size_t iov_len;
        bool nowait_works;
    };
    struct recv_op {
        int fd;
        char* addr;
        size_t size;
        int flags;
    };
    struct recvmsg_op {
        int fd;
        ::msghdr* msghdr;
        int flags;
    };
    using send_op = recv_op;
    using sendmsg_op = recvmsg_op;
    using write_op = read_op;
    using writev_op = readv_op;
    struct fdatasync_op {
        int fd;
    };
    struct accept_op {
        int fd;
        ::sockaddr* sockaddr;
        socklen_t* socklen_ptr;
        int flags;
    };
    struct connect_op {
        int fd;
        ::sockaddr* sockaddr;
        socklen_t socklen;
    };
    struct poll_add_op {
        int fd;
        int events;
    };
    struct poll_remove_op {
        int fd;
        char* addr;
    };
    struct cancel_op {
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

    template<operation Op>
    struct operation_constant {};
    io_request(operation_constant<operation::read>, int fd, uint64_t pos, void* address, size_t size, bool nowait_works)
        : _op(operation::read),
          _read {
            .fd = fd,
            .pos = pos,
            .addr = reinterpret_cast<char*>(address),
            .size = size,
            .nowait_works = nowait_works,
          } {}
    io_request(operation_constant<operation::readv>, int fd, uint64_t pos, std::vector<iovec>& iov, bool nowait_works)
        : _op(operation::readv),
          _readv {
            .fd = fd,
            .pos = pos,
            .iovec = iov.data(),
            .iov_len = iov.size(),
            .nowait_works = nowait_works,
          } {}
    io_request(operation_constant<operation::recv>, int fd, void* address, size_t size, int flags)
        : _op(operation::recv),
          _recv {
            .fd = fd,
            .addr = reinterpret_cast<char*>(address),
            .size = size,
            .flags = flags,
          } {}
    io_request(operation_constant<operation::recvmsg>, int fd, ::msghdr* msg, int flags)
        : _op(operation::recvmsg),
          _recvmsg {
            .fd = fd,
            .msghdr = msg,
            .flags = flags,
          } {}
    io_request(operation_constant<operation::send>, int fd, const void* address, size_t size, int flags)
        : _op(operation::send),
          _send {
            .fd = fd,
            .addr = const_cast<char*>(reinterpret_cast<const char*>(address)),
            .size = size,
            .flags = flags,
          } {}
    io_request(operation_constant<operation::sendmsg>, int fd, ::msghdr* msg, int flags)
        : _op(operation::sendmsg),
          _sendmsg{
            .fd = fd,
            .msghdr = msg,
            .flags = flags,
          } {}
    io_request(operation_constant<operation::write>, int fd, uint64_t pos, const void* address, size_t size, bool nowait_works)
        : _op(operation::write),
          _write {
            .fd = fd,
            .pos = pos,
            .addr = const_cast<char*>(reinterpret_cast<const char*>(address)),
            .size = size,
            .nowait_works = nowait_works,
          } {}
    io_request(operation_constant<operation::writev>, int fd, uint64_t pos, std::vector<iovec>& iov, bool nowait_works)
        : _op(operation::writev),
          _writev{
            .fd = fd,
            .pos = pos,
            .iovec = iov.data(),
            .iov_len = iov.size(),
            .nowait_works = nowait_works,
          } {}
    io_request(operation_constant<operation::fdatasync>, int fd)
        : _op(operation::fdatasync),
          _fdatasync{
            .fd = fd,
          } {}
    io_request(operation_constant<operation::accept>, int fd, struct sockaddr* addr, socklen_t* addrlen, int flags)
        : _op(operation::accept),
          _accept {
            .fd = fd,
            .sockaddr = addr,
            .socklen_ptr = addrlen,
            .flags = flags,
          } {}
    io_request(operation_constant<operation::connect>, int fd, struct sockaddr* addr, socklen_t addrlen)
        : _op(operation::connect),
          _connect {
            .fd = fd,
            .sockaddr = addr,
            .socklen = addrlen,
          } {}
    io_request(operation_constant<operation::poll_add>, int fd, int events)
        : _op(operation::poll_add),
          _poll_add {
            .fd = fd,
            .events = events,
          } {}
    io_request(operation_constant<operation::poll_remove>, int fd, void *addr)
        : _op(operation::poll_remove),
          _poll_remove {
          .fd = fd,
          .addr = reinterpret_cast<char*>(addr),
          } {}
    io_request(operation_constant<operation::cancel>, int fd, void *addr)
        : _op(operation::cancel),
          _cancel {
            .fd = fd,
            .addr = reinterpret_cast<char*>(addr),
          } {}

public:
    static io_request make_read(int fd, uint64_t pos, void* address, size_t size, bool nowait_works) {
        return io_request(operation_constant<operation::read>{}, fd, pos, address, size, nowait_works);
    }
    static io_request make_readv(int fd, uint64_t pos, std::vector<iovec>& iov, bool nowait_works) {
        return io_request(operation_constant<operation::readv>{}, fd, pos, iov, nowait_works);
    }
    static io_request make_recv(int fd, void* address, size_t size, int flags) {
        return io_request(operation_constant<operation::recv>{}, fd, address, size, flags);
    }
    static io_request make_recvmsg(int fd, ::msghdr* msg, int flags) {
        return io_request(operation_constant<operation::recvmsg>{}, fd, msg, flags);
    }
    static io_request make_send(int fd, const void* address, size_t size, int flags) {
        return io_request(operation_constant<operation::send>{}, fd, address, size, flags);
    }
    static io_request make_sendmsg(int fd, ::msghdr* msg, int flags) {
        return io_request(operation_constant<operation::sendmsg>{}, fd, msg, flags);
    }
    static io_request make_write(int fd, uint64_t pos, const void* address, size_t size, bool nowait_works) {
        return io_request(operation_constant<operation::write>{}, fd, pos, address, size, nowait_works);
    }
    static io_request make_writev(int fd, uint64_t pos, std::vector<iovec>& iov, bool nowait_works) {
        return io_request(operation_constant<operation::writev>{}, fd, pos, iov, nowait_works);
    }
    static io_request make_fdatasync(int fd) {
        return io_request(operation_constant<operation::fdatasync>{}, fd);
    }
    static io_request make_accept(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags) {
        return io_request(operation_constant<operation::accept>{}, fd, addr, addrlen, flags);
    }
    static io_request make_connect(int fd, struct sockaddr* addr, socklen_t addrlen) {
        return io_request(operation_constant<operation::connect>{}, fd, addr, addrlen);
    }
    static io_request make_poll_add(int fd, int events) {
        return io_request(operation_constant<operation::poll_add>{}, fd, events);
    }
    static io_request make_poll_remove(int fd, void *addr) {
        return io_request(operation_constant<operation::poll_remove>{}, fd, addr);
    }
    static io_request make_cancel(int fd, void *addr) {
        return io_request(operation_constant<operation::cancel>{}, fd, addr);
    }

    bool is_read() const {
        switch (_op) {
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
        switch (_op) {
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

    operation opcode() const {
        return _op;
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
        if (_op == operation::read) {
            return make_read(_read.fd, _read.pos + pos, _read.addr + pos, _read.size, _read.nowait_works);
        } else {
            return make_write(_write.fd, _write.pos + pos, _write.addr + pos, _write.size, _write.nowait_works);
        }
    }
    std::vector<part> split_buffer(size_t max_length);

    io_request sub_req_iovec(size_t pos, std::vector<iovec>& iov) const {
        if (_op == operation::readv) {
            return make_readv(_readv.fd, _readv.pos + pos, iov, _readv.nowait_works);
        } else {
            return make_writev(_writev.fd, _writev.pos + pos, iov, _writev.nowait_works);
        }
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
        assert(idx == read_idx || idx == write_idx);
    }
};

}
}

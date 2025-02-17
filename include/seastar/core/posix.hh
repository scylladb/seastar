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
 * Copyright (C) 2016 ScyllaDB
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>
#include <utility>
#include <system_error>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <set>
#include <optional>
#endif
#include "abort_on_ebadf.hh"
#include <seastar/core/sstring.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

/// \file
/// \defgroup posix-support POSIX Support
///
/// Mostly-internal APIs to provide C++ glue for the underlying POSIX platform;
/// but can be used by the application when they don't block.
///
/// \addtogroup posix-support
/// @{

inline void throw_system_error_on(bool condition, const char* what_arg = "");

template <typename T>
inline void throw_kernel_error(T r);

template <typename T>
inline void throw_pthread_error(T r);

struct mmap_deleter {
    size_t _size;
    void operator()(void* ptr) const;
};

using mmap_area = std::unique_ptr<char[], mmap_deleter>;

mmap_area mmap_anonymous(void* addr, size_t length, int prot, int flags);

class file_desc {
    int _fd;
public:
    file_desc() = delete;
    file_desc(const file_desc&) = delete;
    file_desc(file_desc&& x) noexcept : _fd(x._fd) { x._fd = -1; }
    ~file_desc() { if (_fd != -1) { ::close(_fd); } }
    void operator=(const file_desc&) = delete;
    file_desc& operator=(file_desc&& x) {
        if (this != &x) {
            std::swap(_fd, x._fd);
            if (x._fd != -1) {
                x.close();
            }
        }
        return *this;
    }
    void close() {
        SEASTAR_ASSERT(_fd != -1);
        auto r = ::close(_fd);
        throw_system_error_on(r == -1, "close");
        _fd = -1;
    }
    int get() const { return _fd; }

    sstring fdinfo() const noexcept;

    static file_desc from_fd(int fd) {
        return file_desc(fd);
    }

    static file_desc open(sstring name, int flags, mode_t mode = 0) {
        int fd = ::open(name.c_str(), flags, mode);
        throw_system_error_on(fd == -1, "open");
        return file_desc(fd);
    }
    static file_desc socket(int family, int type, int protocol = 0) {
        int fd = ::socket(family, type, protocol);
        throw_system_error_on(fd == -1, "socket");
        return file_desc(fd);
    }
    static file_desc eventfd(unsigned initval, int flags) {
        int fd = ::eventfd(initval, flags);
        throw_system_error_on(fd == -1, "eventfd");
        return file_desc(fd);
    }
    static file_desc epoll_create(int flags = 0) {
        int fd = ::epoll_create1(flags);
        throw_system_error_on(fd == -1, "epoll_create1");
        return file_desc(fd);
    }
    static file_desc timerfd_create(int clockid, int flags) {
        int fd = ::timerfd_create(clockid, flags);
        throw_system_error_on(fd == -1, "timerfd_create");
        return file_desc(fd);
    }
    static file_desc temporary(sstring directory);
    file_desc dup() const {
        int fd = ::dup(get());
        throw_system_error_on(fd == -1, "dup");
        return file_desc(fd);
    }
    file_desc accept(socket_address& sa, int flags = 0) {
        auto ret = ::accept4(_fd, &sa.as_posix_sockaddr(), &sa.addr_length, flags);
        throw_system_error_on(ret == -1, "accept4");
        return file_desc(ret);
    }
    static file_desc inotify_init(int flags);
    // return nullopt if no connection is availbale to be accepted
    std::optional<file_desc> try_accept(socket_address& sa, int flags = 0) {
        auto ret = ::accept4(_fd, &sa.as_posix_sockaddr(), &sa.addr_length, flags);
        if (ret == -1 && errno == EAGAIN) {
            return {};
        }
        throw_system_error_on(ret == -1, "accept4");
        return file_desc(ret);
    }
    void shutdown(int how) {
        auto ret = ::shutdown(_fd, how);
        if (ret == -1 && errno != ENOTCONN) {
            throw_system_error_on(ret == -1, "shutdown");
        }
    }
    void truncate(size_t size) {
        auto ret = ::ftruncate(_fd, size);
        throw_system_error_on(ret, "ftruncate");
    }
    int ioctl(int request) {
        return ioctl(request, 0);
    }
    int ioctl(int request, int value) {
        int r = ::ioctl(_fd, request, value);
        throw_system_error_on(r == -1, "ioctl");
        return r;
    }
    int ioctl(int request, unsigned int value) {
        int r = ::ioctl(_fd, request, value);
        throw_system_error_on(r == -1, "ioctl");
        return r;
    }
    template <class X>
    int ioctl(int request, X& data) {
        int r = ::ioctl(_fd, request, &data);
        throw_system_error_on(r == -1, "ioctl");
        return r;
    }
    template <class X>
    int ioctl(int request, X&& data) {
        int r = ::ioctl(_fd, request, &data);
        throw_system_error_on(r == -1, "ioctl");
        return r;
    }
    template <class X>
    int setsockopt(int level, int optname, X&& data) {
        int r = ::setsockopt(_fd, level, optname, &data, sizeof(data));
        throw_system_error_on(r == -1, "setsockopt");
        return r;
    }
    int setsockopt(int level, int optname, const char* data) {
        int r = ::setsockopt(_fd, level, optname, data, strlen(data) + 1);
        throw_system_error_on(r == -1, "setsockopt");
        return r;
    }
    int setsockopt(int level, int optname, const void* data, socklen_t len) {
        int r = ::setsockopt(_fd, level, optname, data, len);
        throw_system_error_on(r == -1, "setsockopt");
        return r;
    }
    template <typename Data>
    Data getsockopt(int level, int optname) {
        Data data;
        socklen_t len = sizeof(data);
        memset(&data, 0, len);
        int r = ::getsockopt(_fd, level, optname, &data, &len);
        throw_system_error_on(r == -1, "getsockopt");
        return data;
    }
    int getsockopt(int level, int optname, char* data, socklen_t len) {
        int r = ::getsockopt(_fd, level, optname, data, &len);
        throw_system_error_on(r == -1, "getsockopt");
        return r;
    }
    size_t size() {
        struct stat buf;
        auto r = ::fstat(_fd, &buf);
        throw_system_error_on(r == -1, "fstat");
        return buf.st_size;
    }
    std::optional<size_t> read(void* buffer, size_t len) {
        auto r = ::read(_fd, buffer, len);
        if (r == -1 && errno == EAGAIN) {
            return {};
        }
        throw_system_error_on(r == -1, "read");
        return { size_t(r) };
    }
    std::optional<ssize_t> recv(void* buffer, size_t len, int flags) {
        auto r = ::recv(_fd, buffer, len, flags);
        if (r == -1 && errno == EAGAIN) {
            return {};
        }
        throw_system_error_on(r == -1, "recv");
        return { ssize_t(r) };
    }
    std::optional<size_t> recvmsg(msghdr* mh, int flags) {
        auto r = ::recvmsg(_fd, mh, flags);
        if (r == -1 && errno == EAGAIN) {
            return {};
        }
        throw_system_error_on(r == -1, "recvmsg");
        return { size_t(r) };
    }
    std::optional<size_t> send(const void* buffer, size_t len, int flags) {
        auto r = ::send(_fd, buffer, len, flags);
        if (r == -1 && errno == EAGAIN) {
            return {};
        }
        throw_system_error_on(r == -1, "send");
        return { size_t(r) };
    }
    std::optional<size_t> sendto(socket_address& addr, const void* buf, size_t len, int flags) {
        auto r = ::sendto(_fd, buf, len, flags, &addr.u.sa, addr.length());
        if (r == -1 && errno == EAGAIN) {
            return {};
        }
        throw_system_error_on(r == -1, "sendto");
        return { size_t(r) };
    }
    std::optional<size_t> sendmsg(const msghdr* msg, int flags) {
        auto r = ::sendmsg(_fd, msg, flags);
        if (r == -1 && errno == EAGAIN) {
            return {};
        }
        throw_system_error_on(r == -1, "sendmsg");
        return { size_t(r) };
    }
    void bind(sockaddr& sa, socklen_t sl) {
        auto r = ::bind(_fd, &sa, sl);
        throw_system_error_on(r == -1, "bind");
    }
    void connect(sockaddr& sa, socklen_t sl) {
        auto r = ::connect(_fd, &sa, sl);
        if (r == -1 && errno == EINPROGRESS) {
            return;
        }
        throw_system_error_on(r == -1, "connect");
    }
    socket_address get_address() {
        socket_address addr;
        auto r = ::getsockname(_fd, &addr.u.sa, &addr.addr_length);
        throw_system_error_on(r == -1, "getsockname");
        return addr;
    }
    socket_address get_remote_address() {
        socket_address addr;
        auto r = ::getpeername(_fd, &addr.u.sa, &addr.addr_length);
        throw_system_error_on(r == -1, "getpeername");
        return addr;
    }
    void listen(int backlog) {
        auto fd = ::listen(_fd, backlog);
        throw_system_error_on(fd == -1, "listen");
    }
    std::optional<size_t> write(const void* buf, size_t len) {
        auto r = ::write(_fd, buf, len);
        if (r == -1 && errno == EAGAIN) {
            return {};
        }
        throw_system_error_on(r == -1, "write");
        return { size_t(r) };
    }
    std::optional<size_t> writev(const iovec *iov, int iovcnt) {
        auto r = ::writev(_fd, iov, iovcnt);
        if (r == -1 && errno == EAGAIN) {
            return {};
        }
        throw_system_error_on(r == -1, "writev");
        return { size_t(r) };
    }
    size_t pread(void* buf, size_t len, off_t off) {
        auto r = ::pread(_fd, buf, len, off);
        throw_system_error_on(r == -1, "pread");
        return size_t(r);
    }
    void timerfd_settime(int flags, const itimerspec& its) {
        auto r = ::timerfd_settime(_fd, flags, &its, NULL);
        throw_system_error_on(r == -1, "timerfd_settime");
    }

    mmap_area map(size_t size, unsigned prot, unsigned flags, size_t offset,
            void* addr = nullptr) {
        void *x = mmap(addr, size, prot, flags, _fd, offset);
        throw_system_error_on(x == MAP_FAILED, "mmap");
        return mmap_area(static_cast<char*>(x), mmap_deleter{size});
    }

    mmap_area map_shared_rw(size_t size, size_t offset) {
        return map(size, PROT_READ | PROT_WRITE, MAP_SHARED, offset);
    }

    mmap_area map_shared_ro(size_t size, size_t offset) {
        return map(size, PROT_READ, MAP_SHARED, offset);
    }

    mmap_area map_private_rw(size_t size, size_t offset) {
        return map(size, PROT_READ | PROT_WRITE, MAP_PRIVATE, offset);
    }

    mmap_area map_private_ro(size_t size, size_t offset) {
        return map(size, PROT_READ, MAP_PRIVATE, offset);
    }

    void spawn_actions_add_close(posix_spawn_file_actions_t* actions) {
        auto r = ::posix_spawn_file_actions_addclose(actions, _fd);
        throw_pthread_error(r);
    }

    void spawn_actions_add_dup2(posix_spawn_file_actions_t* actions, int newfd) {
        auto r = ::posix_spawn_file_actions_adddup2(actions, _fd, newfd);
        throw_pthread_error(r);
    }
private:
    file_desc(int fd) : _fd(fd) {}
 };


namespace posix {

constexpr unsigned rcv_shutdown = 0x1;
constexpr unsigned snd_shutdown = 0x2;
inline constexpr unsigned shutdown_mask(int how) { return how + 1; }

/// Converts a duration value to a `timespec`
///
/// \param d a duration value to convert to the POSIX `timespec` format
/// \return `d` as a `timespec` value
template <typename Rep, typename Period>
struct timespec
to_timespec(std::chrono::duration<Rep, Period> d) {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(d).count();
    struct timespec ts {};
    ts.tv_sec = ns / 1000000000;
    ts.tv_nsec = ns % 1000000000;
    return ts;
}

/// Converts a relative start time and an interval to an `itimerspec`
///
/// \param base First expiration of the timer, relative to the current time
/// \param interval period for re-arming the timer
/// \return `base` and `interval` converted to an `itimerspec`
template <typename Rep1, typename Period1, typename Rep2, typename Period2>
struct itimerspec
to_relative_itimerspec(std::chrono::duration<Rep1, Period1> base, std::chrono::duration<Rep2, Period2> interval) {
    struct itimerspec its {};
    its.it_interval = to_timespec(interval);
    its.it_value = to_timespec(base);
    return its;
}


/// Converts a time_point and a duration to an `itimerspec`
///
/// \param base  base time for the timer; must use the same clock as the timer
/// \param interval period for re-arming the timer
/// \return `base` and `interval` converted to an `itimerspec`
template <typename Clock, class Duration, class Rep, class Period>
struct itimerspec
to_absolute_itimerspec(std::chrono::time_point<Clock, Duration> base, std::chrono::duration<Rep, Period> interval) {
    return to_relative_itimerspec(base.time_since_epoch(), interval);
}

}

class posix_thread {
public:
    class attr;
private:
    // must allocate, since this class is moveable
    std::unique_ptr<std::function<void ()>> _func;
    pthread_t _pthread;
    bool _valid = true;
    mmap_area _stack;
private:
    static void* start_routine(void* arg) noexcept;
public:
    posix_thread(std::function<void ()> func);
    posix_thread(attr a, std::function<void ()> func);
    posix_thread(posix_thread&& x);
    ~posix_thread();
    void join();
public:
    class attr {
    public:
        struct stack_size { size_t size = 0; };
        attr() = default;
        template <typename... A>
        attr(A... a) {
            set(std::forward<A>(a)...);
        }
        void set() {}
        template <typename A, typename... Rest>
        void set(A a, Rest... rest) {
            set(std::forward<A>(a));
            set(std::forward<Rest>(rest)...);
        }
        void set(stack_size ss) { _stack_size = ss; }
        void set(cpu_set_t affinity) { _affinity = affinity; }
    private:
        stack_size _stack_size;
        std::optional<cpu_set_t> _affinity;
        friend class posix_thread;
    };
};


inline
void throw_system_error_on(bool condition, const char* what_arg) {
    if (condition) {
        if ((errno == EBADF || errno == ENOTSOCK) && is_abort_on_ebadf_enabled()) {
            abort();
        }
        throw std::system_error(errno, std::system_category(), what_arg);
    }
}

template <typename T>
inline
void throw_kernel_error(T r) {
    static_assert(std::is_signed_v<T>, "kernel error variables must be signed");
    if (r < 0) {
        auto ec = -r;
        if ((ec == EBADF || ec == ENOTSOCK) && is_abort_on_ebadf_enabled()) {
            abort();
        }
        throw std::system_error(-r, std::system_category());
    }
}

template <typename T>
inline
void throw_pthread_error(T r) {
    if (r != 0) {
        throw std::system_error(r, std::system_category());
    }
}

inline
sigset_t make_sigset_mask(int signo) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signo);
    return set;
}

inline
sigset_t make_full_sigset_mask() {
    sigset_t set;
    sigfillset(&set);
    return set;
}

inline
sigset_t make_empty_sigset_mask() {
    sigset_t set;
    sigemptyset(&set);
    return set;
}

inline
void pin_this_thread(unsigned cpu_id) {
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(cpu_id, &cs);
    auto r = pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    SEASTAR_ASSERT(r == 0);
    (void)r;
}

std::set<unsigned> get_current_cpuset();

/// @}

SEASTAR_MODULE_EXPORT_END
}

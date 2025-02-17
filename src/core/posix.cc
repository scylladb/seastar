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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <memory>
#include <set>
#include <vector>
#include <functional>
#include <fmt/format.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/inotify.h>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/posix.hh>
#include <seastar/core/align.hh>
#include <seastar/util/critical_alloc_section.hh>
#endif
#include <seastar/util/assert.hh>

namespace seastar {

file_desc
file_desc::temporary(sstring directory) {
    // FIXME: add O_TMPFILE support one day
    directory += "/XXXXXX";
    std::vector<char> templat(directory.c_str(), directory.c_str() + directory.size() + 1);
    int fd = ::mkstemp(templat.data());
    throw_system_error_on(fd == -1);
    int r = ::unlink(templat.data());
    throw_system_error_on(r == -1); // leaks created file, but what can we do?
    return file_desc(fd);
}

file_desc
file_desc::inotify_init(int flags) {
    int fd = ::inotify_init1(flags);
    throw_system_error_on(fd == -1, "could not create inotify instance");
    return file_desc(fd);
}

sstring file_desc::fdinfo() const noexcept {
    memory::scoped_critical_alloc_section _;
    auto path = fmt::format("/proc/self/fd/{}", _fd);
    temporary_buffer<char> buf(64);
    auto ret = ::readlink(path.c_str(), buf.get_write(), buf.size());
    if (ret > 0) {
        return sstring(buf.get(), ret);
    } else {
        return fmt::format("error({})", errno);
    }
}

void mmap_deleter::operator()(void* ptr) const {
    ::munmap(ptr, _size);
}

mmap_area mmap_anonymous(void* addr, size_t length, int prot, int flags) {
    auto ret = ::mmap(addr, length, prot, flags | MAP_ANONYMOUS, -1, 0);
    throw_system_error_on(ret == MAP_FAILED);
    return mmap_area(reinterpret_cast<char*>(ret), mmap_deleter{length});
}

void* posix_thread::start_routine(void* arg) noexcept {
    auto pfunc = reinterpret_cast<std::function<void ()>*>(arg);
    (*pfunc)();
    return nullptr;
}

posix_thread::posix_thread(std::function<void ()> func)
    : posix_thread(attr{}, std::move(func)) {
}

posix_thread::posix_thread(attr a, std::function<void ()> func)
    : _func(std::make_unique<std::function<void ()>>(std::move(func))) {
    pthread_attr_t pa;
    auto r = pthread_attr_init(&pa);
    if (r) {
        throw std::system_error(r, std::system_category());
    }

#ifndef SEASTAR_ASAN_ENABLED
    auto stack_size = a._stack_size.size;
    if (!stack_size) {
        stack_size = 2 << 20;
    }
    // allocate guard area as well
    _stack = mmap_anonymous(nullptr, stack_size + (4 << 20),
            PROT_NONE, MAP_PRIVATE | MAP_NORESERVE);
    auto stack_start = align_up(_stack.get() + 1, 2 << 20);
    mmap_area real_stack = mmap_anonymous(stack_start, stack_size,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED | MAP_STACK);
    real_stack.release(); // protected by @_stack
    ::madvise(stack_start, stack_size, MADV_HUGEPAGE);
    r = pthread_attr_setstack(&pa, stack_start, stack_size);
    if (r) {
        throw std::system_error(r, std::system_category());
    }
#endif

#ifdef SEASTAR_PTHREAD_ATTR_SETAFFINITY_NP
    if (a._affinity) {
        auto& cpuset = *a._affinity;
        pthread_attr_setaffinity_np(&pa, sizeof(cpuset), &cpuset);
    }
#endif

    r = pthread_create(&_pthread, &pa,
                &posix_thread::start_routine, _func.get());
    if (r) {
        throw std::system_error(r, std::system_category());
    }

#ifndef SEASTAR_PTHREAD_ATTR_SETAFFINITY_NP
    if (a._affinity) {
        auto& cpuset = *a._affinity;
        pthread_setaffinity_np(_pthread, sizeof(cpuset), &cpuset);
    }
#endif
}

posix_thread::posix_thread(posix_thread&& x)
    : _func(std::move(x._func)), _pthread(x._pthread), _valid(x._valid)
    , _stack(std::move(x._stack)) {
    x._valid = false;
}

posix_thread::~posix_thread() {
    SEASTAR_ASSERT(!_valid);
}

void posix_thread::join() {
    SEASTAR_ASSERT(_valid);
    pthread_join(_pthread, NULL);
    _valid = false;
}

std::set<unsigned> get_current_cpuset() {
    cpu_set_t cs;
    auto r = pthread_getaffinity_np(pthread_self(), sizeof(cs), &cs);
    SEASTAR_ASSERT(r == 0);
    std::set<unsigned> ret;
    unsigned nr = CPU_COUNT(&cs);
    for (int cpu = 0; cpu < CPU_SETSIZE && ret.size() < nr; cpu++) {
        if (CPU_ISSET(cpu, &cs)) {
            ret.insert(cpu);
        }
    }
    return ret;
}

}


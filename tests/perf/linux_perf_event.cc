/*
 * Copyright (C) 2021-present ScyllaDB
 */

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
 * This file was copied from Scylla (https://github.com/scylladb/scylla)
 */

#include <seastar/testing/linux_perf_event.hh>
#include <seastar/util/assert.hh>

#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/ioctl.h>
#include <asm/unistd.h>

linux_perf_event::linux_perf_event(const struct ::perf_event_attr& attr, pid_t pid, int cpu, int group_fd, unsigned long flags) {
    int ret = syscall(__NR_perf_event_open, &attr, pid, cpu, group_fd, flags);
    if (ret != -1) {
        _fd = ret; // ignore failures, can happen in constrained environments such as containers
    }
}

linux_perf_event::~linux_perf_event() {
    if (_fd != -1) {
        ::close(_fd);
    }
}

linux_perf_event&
linux_perf_event::operator=(linux_perf_event&& x) noexcept {
    if (this != &x) {
        if (_fd != -1) {
            ::close(_fd);
        }
        _fd = std::exchange(x._fd, -1);
    }
    return *this;
}

uint64_t
linux_perf_event::read() {
    if (_fd == -1) {
        return 0;
    }
    uint64_t ret;
    auto res = ::read(_fd, &ret, sizeof(ret));
    SEASTAR_ASSERT(res == sizeof(ret) && "read(2) failed on perf_event fd");
    return ret;
}

void
linux_perf_event::enable() {
    if (_fd == -1) {
        return;
    }
    ::ioctl(_fd, PERF_EVENT_IOC_ENABLE, 0);
}

void
linux_perf_event::disable() {
    if (_fd == -1) {
        return;
    }
    ::ioctl(_fd, PERF_EVENT_IOC_DISABLE, 0);
}

static linux_perf_event
make_linux_perf_event(unsigned config, pid_t pid = 0, int cpu = -1, int group_fd = -1, unsigned long flags = 0) {
    return linux_perf_event(perf_event_attr{
            .type = PERF_TYPE_HARDWARE,
            .size = sizeof(struct perf_event_attr),
            .config = config,
            .disabled = 1,
            .exclude_kernel = 1,
            .exclude_hv = 1,
#if defined(__x86_64__)
            // exclude_idle is not supported on all architectures (e.g. aarch64)
            // so enable it selectively only on architectures that support it.
            .exclude_idle = 1,
#endif
            }, pid, cpu, group_fd, flags);
}

linux_perf_event
linux_perf_event::user_instructions_retired() {
    return make_linux_perf_event(PERF_COUNT_HW_INSTRUCTIONS);
}

linux_perf_event
linux_perf_event::user_cpu_cycles_retired() {
    return make_linux_perf_event(PERF_COUNT_HW_CPU_CYCLES);
}

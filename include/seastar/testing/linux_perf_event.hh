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

#pragma once


#include <cstdint>
#include <utility>
#include <unistd.h>

struct perf_event_attr; // from <linux/perf_event.h>

class linux_perf_event {
    int _fd = -1;
public:
    linux_perf_event(const struct ::perf_event_attr& attr, pid_t pid, int cpu, int group_fd, unsigned long flags);
    linux_perf_event(linux_perf_event&& x) noexcept : _fd(std::exchange(x._fd, -1)) {}
    linux_perf_event& operator=(linux_perf_event&& x) noexcept;
    ~linux_perf_event();
    uint64_t read();
    void enable();
    void disable();
public:
    static linux_perf_event user_instructions_retired();
};


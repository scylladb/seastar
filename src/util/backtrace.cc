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
 * Copyright 2017 ScyllaDB
 */
#include <seastar/util/backtrace.hh>

#include <link.h>
#include <sys/types.h>
#include <unistd.h>

#include <errno.h>
#include <string.h>

#include <seastar/core/print.hh>


namespace seastar {

static int dl_iterate_phdr_callback(struct dl_phdr_info *info, size_t size, void *data)
{
    std::size_t total_size{0};
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto hdr = info->dlpi_phdr[i];

        // Only account loadable, executable (text) segments
        if (hdr.p_type == PT_LOAD && (hdr.p_flags & PF_X) == PF_X) {
            total_size += hdr.p_memsz;
        }
    }

    reinterpret_cast<std::vector<shared_object>*>(data)->push_back({info->dlpi_name, info->dlpi_addr, info->dlpi_addr + total_size});

    return 0;
}

static std::vector<shared_object> enumerate_shared_objects() {
    std::vector<shared_object> shared_objects;
    dl_iterate_phdr(dl_iterate_phdr_callback, &shared_objects);

    return shared_objects;
}

static const std::vector<shared_object> shared_objects{enumerate_shared_objects()};
static const shared_object uknown_shared_object{"", 0, std::numeric_limits<uintptr_t>::max()};

bool operator==(const frame& a, const frame& b) {
    return a.so == b.so && a.addr == b.addr;
}

frame decorate(uintptr_t addr) {
    // If the shared-objects are not enumerated yet, or the enumeration
    // failed return the addr as-is with a dummy shared-object.
    if (shared_objects.empty()) {
        return {&uknown_shared_object, addr};
    }

    auto it = std::find_if(shared_objects.begin(), shared_objects.end(), [&] (const shared_object& so) {
        return addr >= so.begin && addr < so.end;
    });

    // Unidentified addresses are assumed to originate from the executable.
    auto& so = it == shared_objects.end() ? shared_objects.front() : *it;
    return {&so, addr - so.begin};
}

saved_backtrace current_backtrace() noexcept {
    saved_backtrace::vector_type v;
    backtrace([&] (frame f) {
        if (v.size() < v.capacity()) {
            v.emplace_back(std::move(f));
        }
    });
    return saved_backtrace(std::move(v), current_scheduling_group());
}

size_t saved_backtrace::hash() const {
    size_t h = 0;
    for (auto f : _frames) {
        h = ((h << 5) - h) ^ (f.so->begin + f.addr);
    }
    return h;
}

std::ostream& operator<<(std::ostream& out, const saved_backtrace& b) {
    for (auto f : b._frames) {
        out << "  ";
        if (!f.so->name.empty()) {
            out << f.so->name << "+";
        }
        out << format("0x{:x}", f.addr) << "\n";
    }
    return out;
}


} // namespace seastar

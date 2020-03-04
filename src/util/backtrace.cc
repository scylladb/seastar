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
#include <seastar/util/variant_utils.hh>

#include <link.h>
#include <sys/types.h>
#include <unistd.h>

#include <errno.h>
#include <string.h>

#include <seastar/core/print.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/reactor.hh>


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

simple_backtrace current_backtrace_tasklocal() noexcept {
    simple_backtrace::vector_type v;
    backtrace([&] (frame f) {
        if (v.size() < v.capacity()) {
            v.emplace_back(std::move(f));
        }
    });
    return simple_backtrace(std::move(v));
}

size_t simple_backtrace::hash() const {
    size_t h = 0;
    for (auto f : _frames) {
        h = ((h << 5) - h) ^ (f.so->begin + f.addr);
    }
    return h;
}

size_t tasktrace::hash() const {
    size_t hash = 0;
    for (auto&& sb : _prev) {
        hash *= 31;
        std::visit(make_visitor([&] (const shared_backtrace& sb) {
            hash ^= sb->hash();
        }, [&] (const task_entry& f) {
            hash ^= f.hash();
        }), sb);
    }
    return hash;
}

std::ostream& operator<<(std::ostream& out, const frame& f) {
    if (!f.so->name.empty()) {
        out << f.so->name << "+";
    }
    out << format("0x{:x}", f.addr);
    return out;
}

std::ostream& operator<<(std::ostream& out, const simple_backtrace& b) {
    for (auto f : b._frames) {
        out << "   " << f << "\n";
    }
    return out;
}

std::ostream& operator<<(std::ostream& out, const tasktrace& b) {
    out << b._main;
    for (auto&& e : b._prev) {
        out << "   --------\n";
        std::visit(make_visitor([&] (const shared_backtrace& sb) {
            out << sb;
        }, [&] (const task_entry& f) {
            out << "   " << f << "\n";
        }), e);
    }
    return out;
}

std::ostream& operator<<(std::ostream& out, const task_entry& e) {
    return out << seastar::pretty_type_name(*e._task_type);
}

tasktrace current_tasktrace() noexcept {
    auto main = current_backtrace_tasklocal();

    tasktrace::vector_type prev;
    if (local_engine && g_current_context) {
        task* tsk = nullptr;

        thread_context* thread = thread_impl::get();
        if (thread) {
            tsk = thread->waiting_task();
        } else {
            tsk = local_engine->current_task();
        }

        while (tsk && prev.size() < prev.max_size()) {
            shared_backtrace bt = tsk->get_backtrace();
            if (bt) {
                prev.push_back(bt);
            } else {
                const std::type_info& ti = typeid(*tsk);
                prev.push_back(task_entry(ti));
            }
            tsk = tsk->waiting_task();
        }
    }

    return tasktrace(std::move(main), std::move(prev), current_scheduling_group());
}

saved_backtrace current_backtrace() noexcept {
    return current_tasktrace();
}

tasktrace::tasktrace(simple_backtrace main, tasktrace::vector_type prev, scheduling_group sg)
    : _main(std::move(main))
    , _prev(std::move(prev))
    , _sg(sg)
{ }

bool tasktrace::operator==(const tasktrace& o) const {
    return _main == o._main && _prev == o._prev;
}

tasktrace::~tasktrace() {}

} // namespace seastar

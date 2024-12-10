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
#ifdef SEASTAR_MODULE
module;
#endif

#include <link.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <variant>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <fmt/ostream.h>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/util/backtrace.hh>
#include <seastar/core/print.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/reactor.hh>
#endif

namespace seastar {

static int dl_iterate_phdr_callback(struct dl_phdr_info *info, size_t size, void *data)
{
    std::size_t total_size{0};
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto hdr = info->dlpi_phdr[i];

        // Only account loadable segments
        if (hdr.p_type == PT_LOAD) {
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

bool operator==(const frame& a, const frame& b) noexcept {
    return a.so == b.so && a.addr == b.addr;
}

frame decorate(uintptr_t addr) noexcept {
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

size_t simple_backtrace::calculate_hash() const noexcept {
    size_t h = 0;
    for (auto f : _frames) {
        h = ((h << 5) - h) ^ (f.so->begin + f.addr);
    }
    return h;
}

std::ostream& operator<<(std::ostream& out, const frame& f) {
    fmt::print(out, "{}", f);
    return out;
}

std::ostream& operator<<(std::ostream& out, const simple_backtrace& b) {
    fmt::print(out, "{}", b);
    return out;
}

std::ostream& operator<<(std::ostream& out, const tasktrace& b) {
    fmt::print(out, "{}", b);
    return out;
}

std::ostream& operator<<(std::ostream& out, const task_entry& e) {
    fmt::print(out, "{}", e);
    return out;
}

tasktrace current_tasktrace() noexcept {
    auto main = current_backtrace_tasklocal();

    tasktrace::vector_type prev;
    size_t hash = 0;
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
            hash *= 31;
            if (bt) {
                hash ^= bt->hash();
                prev.push_back(bt);
            } else {
                const std::type_info& ti = typeid(*tsk);
                prev.push_back(task_entry(ti));
                hash ^= ti.hash_code();
            }
            tsk = tsk->waiting_task();
        }
    }

    return tasktrace(std::move(main), std::move(prev), hash, current_scheduling_group());
}

saved_backtrace current_backtrace() noexcept {
    return current_tasktrace();
}

tasktrace::tasktrace(simple_backtrace main, tasktrace::vector_type prev, size_t prev_hash, scheduling_group sg) noexcept
    : _main(std::move(main))
    , _prev(std::move(prev))
    , _sg(sg)
    , _hash(_main.hash() * 31 ^ prev_hash)
{ }

bool tasktrace::operator==(const tasktrace& o) const noexcept {
    return _hash == o._hash && _main == o._main && _prev == o._prev;
}

tasktrace::~tasktrace() {}

} // namespace seastar

namespace fmt {

auto formatter<seastar::frame>::format(const seastar::frame& f, format_context& ctx) const
    -> decltype(ctx.out()) {
    auto out = ctx.out();
    if (!f.so->name.empty()) {
        out = fmt::format_to(out, "{}+", f.so->name);
    }
    return fmt::format_to(out, "0x{:x}", f.addr);
}

auto formatter<seastar::simple_backtrace>::format(const seastar::simple_backtrace& b, format_context& ctx) const
    -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "{}", fmt::join(b._frames, " "));
}

auto formatter<seastar::tasktrace>::format(const seastar::tasktrace& b, format_context& ctx) const
    -> decltype(ctx.out()) {
    auto out = ctx.out();
    out = fmt::format_to(out, "{}", b._main);
    for (auto&& e : b._prev) {
        out = fmt::format_to(out,  "\n   --------");
        out = std::visit(seastar::make_visitor(
            [&] (const seastar::shared_backtrace& sb) {
                return fmt::format_to(out,  "\n{}", sb);
            },
            [&] (const seastar::task_entry& f) {
                return fmt::format_to(out,  "\n   {}", f);
            }), e);
    }
    return out;
}

auto formatter<seastar::task_entry>::format(const seastar::task_entry& e, format_context& ctx) const
    -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "{}", seastar::pretty_type_name(*e._task_type));
}

}

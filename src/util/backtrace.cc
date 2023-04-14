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
    if (!f.so->name.empty()) {
        out << f.so->name << "+";
    }
    out << format("0x{:x}", f.addr);
    return out;
}

std::ostream& operator<<(std::ostream& out, const simple_backtrace& b) {
    char delim[2] = {'\0', '\0'};
    for (auto f : b._frames) {
        out << delim << f;
        delim[0] = b.delimeter();
    }
    return out;
}

std::ostream& operator<<(std::ostream& out, const tasktrace& b) {
    out << b._main;
    for (auto&& e : b._prev) {
        out << "\n   --------";
        std::visit(make_visitor([&] (const shared_backtrace& sb) {
            out << '\n' << sb;
        }, [&] (const task_entry& f) {
            out << "\n   " << f;
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

thread_local tracer g_tracer;

struct tracer::impl {
    std::optional<future<>> _loop;
    server_socket _ss;
    std::optional<connected_socket> _conn;
    tracer* _parent;
    condition_variable _cv;
    bool _stopping = false;

    size_t queued() {
        auto t = _parent->_tail; 
        auto h = _parent->_head; 
        if (h > t) {
            return h - t;
        } else {
            return _parent->_buf.size() - (t - h);
        }
    }

    impl(tracer* p) : _parent(p) {}

    future<> handle() {
        auto out = _conn->output(_parent->_buf.size() * sizeof(_parent->_buf[0]));
        while (true) {
            co_await _cv.when([&] {return queued() >= 1024 || _stopping;});
            if (_stopping) {
                break;
            }
            auto t = _parent->_tail;
            auto h = _parent->_head;
            if (h > t) {
                auto base = t;
                auto size = h - t;
                co_await out.write((char*)(&_parent->_buf[base]), sizeof(_parent->_buf[0]) * size);
            } else {
                auto base = t;
                auto size = _parent->_buf.size() - t;
                co_await out.write((char*)(&_parent->_buf[base]), sizeof(_parent->_buf[0]) * size);
                base = 0;
                size = h;
                co_await out.write((char*)(&_parent->_buf[base]), sizeof(_parent->_buf[0]) * size);
            }
            if (h == t) {
                std::cerr << "OVERFULL\n";
            }
            _parent->_tail = (h > 0) ? (h - 1) : (_parent->_buf.size() - 1);
        }
    }

    void commit() {
        _cv.signal();
    }

    future<> run_loop() {
        listen_options lo;
        lo.reuse_address = true;
        auto addr = socket_address(uint16_t(12345));
        _ss = seastar::listen(addr, lo);
        while (true) {
            accept_result ar = co_await _ss.accept();
            _conn = std::move(ar.connection);
            try {
                co_await handle();
            } catch(...) { 
            }
            _conn->shutdown_input();
            _conn->shutdown_output();
            _conn = {};
        }
    }

    void start() {
        _loop = run_loop();
    }

    future<> stop() {
        _stopping = true;
        try {
            _ss.abort_accept();
            if (_conn) {
                _conn->shutdown_input();
                _conn->shutdown_output();
            }
            _cv.signal();
        } catch (...) {
        }
        return std::move(*std::exchange(_loop, std::nullopt));
    }
};

tracer::tracer() : _buf(buffer_size), _impl(new impl(this)) {
}

void tracer::start() {
    _impl->start();
}

future<> tracer::stop() {
    return _impl->stop();
}

void tracer::commit() {
    _impl->commit();
}

} // namespace seastar

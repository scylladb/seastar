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
 * Copyright (C) 2022 Kefu Chai ( tchaikov@gmail.com )
 */

#ifdef SEASTAR_MODULE
module;
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>
#include <utility>
module seastar;
#else
#include <seastar/core/fstream.hh>
#include <seastar/core/internal/buffer_allocator.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/polymorphic_temporary_buffer.hh>
#include <seastar/core/reactor.hh>
#include <seastar/util/process.hh>
#endif
#include <seastar/util/assert.hh>

namespace seastar::experimental {

namespace {
class pipe_data_source_impl final : public data_source_impl {
    static constexpr std::size_t buffer_size = 8192;
    struct buffer_allocator : public internal::buffer_allocator {
        temporary_buffer<char> allocate_buffer() override {
            return make_temporary_buffer<char>(memory::malloc_allocator, buffer_size);
        }
    };
    pollable_fd _fd;
    buffer_allocator _ba;
public:
    explicit pipe_data_source_impl(pollable_fd fd)
        : _fd(std::move(fd)) {}
    static auto from_fd(file_desc&& fd) {
        return std::make_unique<pipe_data_source_impl>(pollable_fd(std::move(fd)));
    }
    future<temporary_buffer<char>> get() override {
        return _fd.read_some(&_ba);
    }
    future<> close() override {
        _fd.close();
        return make_ready_future();
    }
};

class pipe_data_sink_impl final : public data_sink_impl {
    file_desc _fd;
    io_queue& _io_queue;
    const size_t _buffer_size;
public:
    explicit pipe_data_sink_impl(file_desc&& fd)
        : _fd(std::move(fd))
        , _io_queue(engine().get_io_queue(0))
        , _buffer_size(file_input_stream_options{}.buffer_size) {}
    static auto from_fd(file_desc&& fd) {
        return std::make_unique<pipe_data_sink_impl>(std::move(fd));
    }
    using data_sink_impl::put;
    future<> put(temporary_buffer<char> buf) override {
        size_t buf_size = buf.size();
        auto req = internal::io_request::make_write(_fd.get(), 0, buf.get(), buf_size, false);
        return _io_queue.submit_io_write(internal::priority_class(internal::maybe_priority_class_ref()), buf_size, std::move(req), nullptr).then(
            [this, buf = std::move(buf), buf_size] (size_t written) mutable {
                if (written < buf_size) {
                    buf.trim_front(written);
                    return put(std::move(buf));
                }
                return make_ready_future();
            });
    }
    future<> put(net::packet data) override {
        return do_with(data.release(), [this] (std::vector<temporary_buffer<char>>& bufs) {
            return do_for_each(bufs, [this] (temporary_buffer<char>& buf) {
                return put(buf.share());
            });
        });
    }
    future<> close() override {
        _fd.close();
        return make_ready_future();
    }
    size_t buffer_size() const noexcept override {
        return _buffer_size;
    }
};
}

process::process(create_tag, pid_t pid, file_desc&& cin, file_desc&& cout, file_desc&& cerr)
    : _pid(pid)
    , _stdin(std::move(cin))
    , _stdout(std::move(cout))
    , _stderr(std::move(cerr)) {}

future<process::wait_status> process::wait() {
    return engine().waitpid(_pid).then([] (int wstatus) -> wait_status {
        if (WIFEXITED(wstatus)) {
            return wait_exited{WEXITSTATUS(wstatus)};
        } else {
            SEASTAR_ASSERT(WIFSIGNALED(wstatus));
            return wait_signaled{WTERMSIG(wstatus)};
        }
    });
}

void process::terminate() {
    engine().kill(_pid, SIGTERM);
}

void process::kill() {
    engine().kill(_pid, SIGKILL);
}

future<process> process::spawn(const std::filesystem::path& pathname,
                               spawn_parameters params) {
    SEASTAR_ASSERT(!params.argv.empty());
    return engine().spawn(pathname.native(), std::move(params.argv), std::move(params.env)).then_unpack(
            [] (pid_t pid, file_desc stdin_pipe, file_desc stdout_pipe, file_desc stderr_pipe) {
        return make_ready_future<process>(create_tag{}, pid, std::move(stdin_pipe), std::move(stdout_pipe), std::move(stderr_pipe));
    });
}

future<process> process::spawn(const std::filesystem::path& pathname) {
    return spawn(pathname, {{pathname.native()}, {}});
}

output_stream<char> process::cin() {
    return output_stream<char>(data_sink(pipe_data_sink_impl::from_fd(std::move(_stdin))));
}

input_stream<char> process::cout() {
    return input_stream<char>(data_source(pipe_data_source_impl::from_fd(std::move(_stdout))));
}

input_stream<char> process::cerr() {
    return input_stream<char>(data_source(pipe_data_source_impl::from_fd(std::move(_stderr))));
}

}

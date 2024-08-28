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
 * Copyright (C) 2024 ScyllaDB
 */
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/fstream.hh>
#include <seastar/util/trace.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/internal/io_trace.hh>

using namespace seastar;

size_t event_body_size(internal::trace_event ev) {
    switch (ev) {
    case internal::trace_event::TICK:
        return 0;
    case internal::trace_event::IO_POLL:
        return internal::event_tracer<internal::trace_event::IO_POLL>::size();
    case internal::trace_event::IO_QUEUE:
        return internal::event_tracer<internal::trace_event::IO_QUEUE>::size();
    case internal::trace_event::IO_DISPATCH:
        return internal::event_tracer<internal::trace_event::IO_DISPATCH>::size();
    case internal::trace_event::IO_COMPLETE:
        return internal::event_tracer<internal::trace_event::IO_COMPLETE>::size();
    case internal::trace_event::BUFFER_HEAD:
    case internal::trace_event::OPENING:
    case internal::trace_event::T800:
        break;
    }
    fmt::print("Invalid event met ({})\n", static_cast<int>(ev));
    throw std::runtime_error("invalid file content");
    return 0;
}

template <internal::trace_event Ev>
std::string parse(temporary_buffer<char> body);

template<>
std::string parse<internal::trace_event::TICK>(temporary_buffer<char> body) {
    return "TICK";
}

template<>
std::string parse<internal::trace_event::IO_POLL>(temporary_buffer<char> body) {
    return "IO_POLL";
}

template<>
std::string parse<internal::trace_event::IO_QUEUE>(temporary_buffer<char> body) {
    const char* b = body.get();
    auto rw = read_le<uint8_t>(b + 0);
    auto cid = read_le<uint8_t>(b + 1);
    auto len = read_le<uint16_t>(b + 2);
    auto rq = read_le<uint32_t>(b + 4);
    return format("IO Q {:04x} {} class {} {}", rq, rw == 0 ? "w" : "r", cid, len << 9);
}

template<>
std::string parse<internal::trace_event::IO_DISPATCH>(temporary_buffer<char> body) {
    const char* b = body.get();
    auto rq = read_le<uint32_t>(b);
    return format("IO D {:04x}", rq);
}

template<>
std::string parse<internal::trace_event::IO_COMPLETE>(temporary_buffer<char> body) {
    const char* b = body.get();
    auto rq = read_le<uint32_t>(b);
    return format("IO C {:04x}", rq);
}

std::string parse_event(internal::trace_event ev, temporary_buffer<char> body) {
    switch (ev) {
    case internal::trace_event::TICK:
        return parse<internal::trace_event::TICK>(std::move(body));
    case internal::trace_event::IO_POLL:
        return parse<internal::trace_event::IO_POLL>(std::move(body));
    case internal::trace_event::IO_QUEUE:
        return parse<internal::trace_event::IO_QUEUE>(std::move(body));
    case internal::trace_event::IO_DISPATCH:
        return parse<internal::trace_event::IO_DISPATCH>(std::move(body));
    case internal::trace_event::IO_COMPLETE:
        return parse<internal::trace_event::IO_COMPLETE>(std::move(body));
    case internal::trace_event::BUFFER_HEAD:
    case internal::trace_event::OPENING:
    case internal::trace_event::T800:
        break;
    }
    return "";
}

std::string format_ts(uint64_t timestamp) {
    // timestamps are in usec
    return format("{:03d}.{:06d}", timestamp/1000000, timestamp%1000000);
}

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    app_template app;
    auto opt_add = app.add_options();
    opt_add
        ("file", bpo::value<sstring>()->default_value("trace.0.bin"), "file to decode")
    ;

    return app.run(ac, av, [&] {
        return seastar::async([&] {
            auto& opts = app.configuration();
            auto& fname = opts["file"].as<sstring>();
            auto f = open_file_dma(fname, open_flags::ro).get();
            auto in = make_file_input_stream(std::move(f));
            size_t consumed = 0;
            uint64_t timestamp = 0;

            while (true) {
                auto tb = in.read_exactly(sizeof(uint8_t)).get();
                if (!tb) {
                    break;
                }
                const char* buf = tb.get();
                auto ev = static_cast<internal::trace_event>(*(uint8_t*)buf);

                if (ev == internal::trace_event::OPENING) {
                    tb = in.read_exactly(sizeof(uint8_t) + sizeof(uint16_t)).get();
                    if (!tb) {
                        fmt::print("corrupted buffer -- no opening body\n");
                        break;
                    }
                    buf = tb.get();
                    auto shard = read_le<uint8_t>(buf);
                    auto size = read_le<uint16_t>(buf + 1);

                    tb = in.read_exactly(size).get();
                    if (!tb) {
                        fmt::print("corrupted buffer -- no opening payload (size={})\n", size);
                        break;
                    }
                    buf = tb.get();
                    fmt::print("{:08x}:- OPENING: shard={} text={}\n", consumed, shard, std::string(buf, size));
                    consumed += sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint16_t) + size;
                    continue;
                }

                if (ev == internal::trace_event::BUFFER_HEAD) {
                    tb = in.read_exactly(2 * sizeof(uint32_t)).get();
                    if (!tb) {
                        fmt::print("corrupted buffer -- no buffer-head body\n");
                        break;
                    }
                    buf = tb.get();
                    auto ts = read_le<uint32_t>(buf);
                    timestamp += ts;
                    auto skip = read_le<uint32_t>(buf + sizeof(uint32_t));
                    fmt::print("{:08x}:{} --- buffer (skipped {}) ---\n", consumed, format_ts(timestamp), skip);
                    consumed = sizeof(uint8_t) + 2 * sizeof(uint32_t);
                    continue;
                }

                if (ev == internal::trace_event::T800) {
                    auto rem = internal::tracer::buffer_size - consumed - sizeof(uint8_t);
                    fmt::print("{:08x}:- REM -- skip {} bytes\n", consumed, rem);
                    if (rem > 0) {
                        in.read_exactly(rem).get();
                    }
                    consumed = 0;
                    continue;
                }

                tb = in.read_exactly(sizeof(uint16_t)).get();
                if (!tb) {
                    fmt::print("corrupted buffer -- no timestamp\n");
                    break;
                }
                buf = tb.get();
                auto ts = read_le<uint16_t>(buf);
                timestamp += ts;

                auto sz = event_body_size(ev);
                if (sz > 0) {
                    tb = in.read_exactly(sz).get();
                    if (!tb) {
                        fmt::print("corrupted file -- no event body\n");
                    }
                } else {
                    tb = temporary_buffer<char>();
                }
                fmt::print("{:08x}:{} {}\n", consumed, format_ts(timestamp), parse_event(ev, std::move(tb)));
                consumed += sz + sizeof(uint8_t) + sizeof(uint16_t);
            }
        });
    });
}

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

#include "../../core/reactor.hh"
#include "../../core/fstream.hh"
#include "../../core/file.hh"
#include "../../core/app-template.hh"

using namespace std::chrono_literals;

int main(int ac, char** av) {
    app_template at;
    namespace bpo = boost::program_options;
    at.add_options()
            ("concurrency", bpo::value<unsigned>()->default_value(1), "Write operations to issue in parallel")
            ("buffer-size", bpo::value<size_t>()->default_value(4096), "Write buffer size")
            ("total-ops", bpo::value<unsigned>()->default_value(100000), "Total write operations to issue")
            ("sloppy-size", bpo::value<bool>()->default_value(false), "Enable the sloppy-size optimization")
            ;
    return at.run(ac, av, [&at] {
        auto concurrency = at.configuration()["concurrency"].as<unsigned>();
        auto buffer_size = at.configuration()["buffer-size"].as<size_t>();
        auto total_ops = at.configuration()["total-ops"].as<unsigned>();
        auto sloppy_size = at.configuration()["sloppy-size"].as<bool>();
        file_open_options foo;
        foo.sloppy_size = sloppy_size;
        return open_file_dma(
                "testfile.tmp", open_flags::wo | open_flags::create | open_flags::exclusive,
                foo).then([=] (file f) {
            file_output_stream_options foso;
            foso.buffer_size = buffer_size;
            foso.preallocation_size = 32 << 20;
            foso.write_behind = concurrency;
            auto os = make_file_output_stream(f, foso);
            return do_with(std::move(os), std::move(f), unsigned(0), [=] (output_stream<char>& os, file& f, unsigned& completed) {
                auto start = std::chrono::steady_clock::now();
                return repeat([=, &os, &completed] {
                    if (completed == total_ops) {
                        return make_ready_future<stop_iteration>(stop_iteration::yes);
                    }
                    char buf[buffer_size];
                    memset(buf, 0, buffer_size);
                    return os.write(buf, buffer_size).then([&completed] {
                        ++completed;
                        return stop_iteration::no;
                    });
                }).then([=, &os] {
                    auto end = std::chrono::steady_clock::now();
                    using fseconds = std::chrono::duration<float, std::ratio<1, 1>>;
                    auto iops = total_ops / std::chrono::duration_cast<fseconds>(end - start).count();
                    print("%10s %10s %10s %12s\n", "bufsize", "ops", "iodepth", "IOPS");
                    print("%10d %10d %10d %12.0f\n", buffer_size, total_ops, concurrency, iops);
                    return os.flush();
                }).then([&os] {
                    return os.close();
                });
            });
        });
    });
}

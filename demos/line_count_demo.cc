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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

// Demonstration of file_input_stream.  Don't expect stellar performance
// since no read-ahead or caching is done yet.

#include <seastar/core/fstream.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/reactor.hh>
#include <algorithm>
#include <iostream>

using namespace seastar;

struct reader {
public:
    reader(file f)
            : is(make_file_input_stream(std::move(f), file_input_stream_options{1 << 16, 1})) {
    }

    input_stream<char> is;
    size_t count = 0;

    // for input_stream::consume():
    using unconsumed_remainder = compat::optional<temporary_buffer<char>>;
    future<unconsumed_remainder> operator()(temporary_buffer<char> data) {
        if (data.empty()) {
            return make_ready_future<unconsumed_remainder>(std::move(data));
        } else {
            count += std::count(data.begin(), data.end(), '\n');
            // FIXME: last line without \n?
            return make_ready_future<unconsumed_remainder>();
        }
    }
};

int main(int ac, char** av) {
    app_template app;
    namespace bpo = boost::program_options;
    app.add_positional_options({
        { "file", bpo::value<std::string>(), "File to process", 1 },
    });
    return app.run(ac, av, [&app] {
        auto fname = app.configuration()["file"].as<std::string>();
        return open_file_dma(fname, open_flags::ro).then([] (file f) {
            auto r = make_shared<reader>(std::move(f));
            return r->is.consume(*r).then([r] {
               fmt::print("{:d} lines\n", r->count);
               return r->is.close().then([r] {});
            });
        }).then_wrapped([] (future<> f) -> future<int> {
            try {
                f.get();
                return make_ready_future<int>(0);
            } catch (std::exception& ex) {
                std::cout << ex.what() << "\n";
                return make_ready_future<int>(1);
            } catch (...) {
                std::cout << "unknown exception\n";
                return make_ready_future<int>(1);
            }
        });
    });
}


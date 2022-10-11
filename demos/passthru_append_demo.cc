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
 * Copyright 2022 Jinyong Ha (jyha200@gmail.com), Heewon Shin (shw096@snu.ac.kr)
 */
#include <seastar/core/app-template.hh>
#include <seastar/core/file.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <iostream>

using namespace seastar;
namespace bpo = boost::program_options;

static const size_t IO_SIZE = 4096;
static const size_t ALIGNMENT_SIZE = 4096;

int main(int ac, char** av) {
    app_template app;
    app.add_options()
        ("dev", bpo::value<std::string>(), "e.g. --dev /dev/nvme0n1")
        ;

    return app.run_deprecated(ac, av, [&app] {
        auto&& config = app.configuration();
        auto filepath = config["dev"].as<std::string>();

        return open_file_dma(filepath, open_flags::rw | open_flags::create).then([] (file f) {
            auto wbuf = allocate_aligned_buffer<unsigned char>(IO_SIZE, ALIGNMENT_SIZE);
            size_t address = 0;
            std::fill(wbuf.get(), wbuf.get() + IO_SIZE, (uint8_t)0xa);
            auto wb = wbuf.get();
            auto result = f.dma_append(address * IO_SIZE, wb, IO_SIZE).then([f, address, wbuf = std::move(wbuf)] (io_result ret) mutable {
                auto appended_address = ret.res2;
                assert(ret.res1 == IO_SIZE);
                assert(ret.res2 != -1);
                auto rbuf = allocate_aligned_buffer<unsigned char>(IO_SIZE, ALIGNMENT_SIZE);
                auto rb = rbuf.get();
                (void)f.dma_read(appended_address, rb, IO_SIZE).then([f, rbuf = std::move(rbuf), wbuf = std::move(wbuf)] (size_t ret) mutable{
                    assert(ret == IO_SIZE);
                    assert(std::equal(rbuf.get(), rbuf.get() + IO_SIZE, wbuf.get()));
                    return f.close().then([](){
                        std::cout << "done" << std::endl;
                        engine().exit(0);
                    });
                });
            });
        });
    });
}


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
#include "core/app-template.hh"
#include "core/distributed.hh"
#include "core/reactor.hh"
#include "core/future.hh"
#include "core/shared_ptr.hh"
#include "core/file.hh"
#include "core/sleep.hh"
#include "core/align.hh"
#include "core/timer.hh"
#include <chrono>
#include <boost/range/irange.hpp>
#include <boost/algorithm/string.hpp>
#include <iomanip>
#include <random>

using namespace std::chrono_literals;

static auto random_seed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
static std::default_random_engine random_generator(random_seed);

class context {
    struct class_data {
        static int idgen();

        uint32_t _shares;
        io_priority_class _iop;
        unsigned _final = 0;

        size_t _bytes = 0;
        std::chrono::steady_clock::time_point _start = {};

        class_data(uint32_t shares)
            : _shares(shares)
            , _iop(engine().register_one_priority_class(sprint("test-class-%d", idgen()), shares))
        {}
    };
    std::vector<class_data> _cl;

    sstring _dir;
    unsigned _parallelism;
    std::chrono::seconds _duration;
    size_t _reqsize;

    semaphore _finished;
    file _fq;
    std::uniform_int_distribution<uint32_t> _pos_distribution;
public:
    context(sstring dir, std::vector<uint32_t> shares, unsigned parallelism, unsigned duration, size_t reqsize)
            : _cl(shares.begin(), shares.end())
            , _dir(dir)
            , _parallelism(parallelism)
            , _duration(duration)
            , _reqsize(align_up(reqsize, 4096ul))
            , _finished(0)
            , _pos_distribution(0, parallelism * shares.size() - 1)
    {
    }

    future<> stop() { return make_ready_future<>(); }
    future<> start(sstring name) {
        return open_file_dma(name, open_flags::ro).then([this] (auto f) {
            _fq = f;
        });
    }

    future<> read_class(class_data& cl) {
        auto bufptr = allocate_aligned_buffer<char>(_reqsize, 4096);
        auto buf = bufptr.get();
        auto pos = _pos_distribution(random_generator) * _reqsize;
        return _fq.dma_read(pos, buf, _reqsize, cl._iop).then([bufptr = std::move(bufptr), &cl, this] (size_t size) {
            cl._bytes += size;
            if ((std::chrono::steady_clock::now() - cl._start) < _duration) {
                return this->read_class(cl);
            } else {
                return make_ready_future<>();
            }
        });
    }

    future<> issue_reads() {
        return parallel_for_each(_cl.begin(), _cl.end(), [this] (class_data& cl) {
            auto parallelism = boost::irange(0u, _parallelism);
            cl._start = std::chrono::steady_clock::now();
            return parallel_for_each(parallelism.begin(), parallelism.end(), [this, &cl] (auto dummy) {
                return this->read_class(cl);
            }).then([&cl, this] {
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - cl._start).count();
                cl._final = (cl._bytes / duration) >> 10;
                _finished.signal(1);
            });
        });
    }

    future<> print_stats() {
        return _finished.wait(_cl.size()).then([this] {
            std::stringstream ss;
            ss << "Shard " << std::setw(2) << engine().cpu_id() << ":";
            auto idx = 0;
            for (auto& cl: _cl) {
                ss << " Class " << idx++ << "(" << std::setw(2) << cl._shares << " shares): " << std::setw(8) << cl._final << " KB/s";
            }
            ss << std::endl;
            std::cout << ss.str();
            return make_ready_future<>();
        });
    }
};

int context::class_data::idgen() {
    static thread_local int id = 0;
    return id++;
}

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    app_template app;
    auto opt_add = app.add_options();
    opt_add
        ("directory", bpo::value<sstring>()->default_value("."), "directory where to execute the test")
        ("parallelism", bpo::value<unsigned>()->default_value(10), "number of parallel requests per class")
        ("duration", bpo::value<unsigned>()->default_value(10), "for how long (in seconds) to run the test")
        ("reqsize", bpo::value<size_t>()->default_value(4096), "size of each read request")
        ("shares", bpo::value<sstring>()->default_value("10,10"), "comma-separated list of shares per each class (default: 10,10)")
    ;


    distributed<context> ctx;
    return app.run(ac, av, [&] {
        auto& opts = app.configuration();
        auto& directory = opts["directory"].as<sstring>();
        return file_system_at(directory).then([directory] (auto fs) {
            if (fs != fs_type::xfs) {
                throw std::runtime_error(sprint("This is a performance test. %s is not on XFS", directory));
            }
        }).then([&] {
            auto& parallelism = opts["parallelism"].as<unsigned>();
            auto& share_list = opts["shares"].as<sstring>();
            auto& duration = opts["duration"].as<unsigned>();
            auto& reqsize = opts["reqsize"].as<size_t>();

            std::vector<sstring> strs;
            boost::split(strs, share_list, boost::is_any_of(","));
            std::vector<uint32_t> shares(strs.size(), 0);
            std::transform(strs.begin(), strs.end(), shares.begin(), [] (sstring s) { return boost::lexical_cast<uint32_t>(s); });

            // Create the file, it is the same file so do it from one shard only.
            auto name = sprint("%s/test-queue", directory);
            auto size = reqsize * parallelism * shares.size();
            return open_file_dma(name, open_flags::rw | open_flags::create | open_flags::truncate).then([name, size] (auto f) {
                auto bufptr = allocate_aligned_buffer<char>(size, 4096);
                auto buf = bufptr.get();
                std::uniform_int_distribution<char> fill('@', '~');
                memset(buf, fill(random_generator), size);
                return f.dma_write(0, buf, size).then([size, bufptr = std::move(bufptr), f] (auto s) {
                    assert(s == size);
                    return make_ready_future<>();
                });
            }).then([directory, shares, parallelism, duration, reqsize, name, &ctx] {
                return ctx.start(directory, std::move(shares), parallelism, duration, reqsize).then([&ctx, name = std::move(name)] {
                    engine().at_exit([&ctx] {
                        return ctx.stop();
                    });
                    return ctx.invoke_on_all([name = std::move(name)] (auto& c) {
                        return c.start(name).then([&c] {
                            return c.issue_reads();
                        }).then([&c] {
                            return c.print_stats();
                        });
                    }).or_terminate();
                });
            });
        });
    });
}

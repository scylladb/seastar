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
 * Copyright (C) 2017 ScyllaDB
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
#include <vector>
#include <boost/range/irange.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/p_square_quantile.hpp>
#include <boost/accumulators/statistics/extended_p_square.hpp>
#include <boost/accumulators/statistics/extended_p_square_quantile.hpp>
#include <boost/range/adaptor/filtered.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/array.hpp>
#include <iomanip>
#include <random>
#include <yaml-cpp/yaml.h>

using namespace seastar;
using namespace std::chrono_literals;
using namespace boost::accumulators;

static auto random_seed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
static std::default_random_engine random_generator(random_seed);
// size of each individual file. Every class will have its file, so in a normal system with many shards, we'll naturally have many files and
// that will push the data out of the disk's cache. And static sizes per file are simpler.
static constexpr uint64_t file_data_size = 1ull << 30;

struct context;
enum class request_type { seqread, seqwrite, randread, randwrite, append };

namespace std {

template <>
struct hash<request_type> {
    size_t operator() (const request_type& type) const {
        return static_cast<size_t>(type);
    }
};

}

struct byte_size {
    uint64_t size;
};

struct duration_time {
    std::chrono::duration<float> time;
};

class shard_config {
    std::unordered_set<unsigned> _shards;
public:
    shard_config()
        : _shards(boost::copy_range<std::unordered_set<unsigned>>(boost::irange(0u, smp::count))) {}
    shard_config(std::unordered_set<unsigned> s) : _shards(std::move(s)) {}

    bool is_set(unsigned cpu) const {
        return _shards.count(cpu);
    }
};

struct shard_info {
    unsigned parallelism = 10;
    unsigned shares = 10;
    uint64_t request_size = 4 << 10;
    std::chrono::duration<float> think_time = 0ms;
};

struct job_config {
    std::string name;
    request_type type;
    shard_config shard_placement;
    ::shard_info shard_info;
};

std::array<double, 4> quantiles = { 0.5, 0.95, 0.99, 0.999};

class class_data {
    using accumulator_type = accumulator_set<double, stats<tag::extended_p_square_quantile(quadratic), tag::mean, tag::max>>;

    job_config _config;
    uint64_t _alignment;
    uint64_t _last_pos = 0;

    io_priority_class _iop;

    size_t _bytes = 0;
    std::chrono::duration<float> _total_duration;

    std::chrono::steady_clock::time_point _start = {};
    accumulator_type _latencies;
    std::uniform_int_distribution<uint32_t> _pos_distribution;
    file _file;
public:

    static int idgen();
    class_data(job_config cfg)
        : _config(std::move(cfg))
        , _alignment(_config.shard_info.request_size >= 4096 ? 4096 : 512)
        , _iop(engine().register_one_priority_class(sprint("test-class-%d", idgen()), _config.shard_info.shares))
        , _latencies(extended_p_square_probabilities = quantiles)
        , _pos_distribution(0,  file_data_size / _config.shard_info.request_size)
    {}

    future<> issue_requests(std::chrono::steady_clock::time_point stop) {
        _start = std::chrono::steady_clock::now();
        return parallel_for_each(boost::irange(0u, parallelism()), [this, stop] (auto dummy) mutable {
            auto bufptr = allocate_aligned_buffer<char>(this->req_size(), _alignment);
            auto buf = bufptr.get();
            return do_until([this, stop] { return std::chrono::steady_clock::now() > stop; }, [this, buf] () mutable {
                auto start = std::chrono::steady_clock::now();
                future<size_t> fut = make_ready_future<size_t>(0);
                if (this->is_read()) {
                    fut = _file.dma_read(this->get_pos(), buf, this->req_size(), _iop);
                } else {
                    fut = _file.dma_write(this->get_pos(), buf, this->req_size(), _iop);
                }
                return fut.then([this, start] (auto size) {
                    this->add_result(size, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start));
                    return seastar::sleep(std::chrono::duration_cast<std::chrono::microseconds>(_config.shard_info.think_time));
                });
            }).finally([bufptr = std::move(bufptr)] {});
        }).then([this] {
            _total_duration = std::chrono::steady_clock::now() - _start;
        });
    }

    // Generate the test file for reads and writes alike. It is much simpler to just generate one file per job instead of expecting
    // job dependencies between creators and consumers. So every job (a class in a shard) will have its own file and will operate
    // this file differently depending on the type:
    //
    // sequential reads  : will read the file from pos = 0 onwards, back to 0 on EOF
    // sequential writes : will write the file from pos = 0 onwards, back to 0 on EOF
    // random reads      : will read the file at random positions, between 0 and EOF
    // random writes     : will overwrite the file at a random position, between 0 and EOF
    // append            : will write to the file from pos = EOF onwards, always appending to the end.
    future<> start(sstring dir) {
        auto fname = sprint("%s/test-%s-%d", dir, name(), engine().cpu_id());
        return open_file_dma(fname, open_flags::rw | open_flags::create | open_flags::truncate).then([this, fname] (auto f) {
            _file = f;
            return remove_file(fname).then([this] {
                return do_with(seastar::semaphore(64), [this] (auto& write_parallelism) mutable {
                    auto bufsize = 256ul << 10;
                    auto pos = boost::irange(0ul, (file_data_size / bufsize) + 1);
                    return parallel_for_each(pos.begin(), pos.end(), [this, bufsize, &write_parallelism] (auto pos) mutable {
                        return get_units(write_parallelism, 1).then([this, bufsize, pos] (auto perm) mutable {
                            auto bufptr = allocate_aligned_buffer<char>(bufsize, 4096);
                            auto buf = bufptr.get();
                            std::uniform_int_distribution<char> fill('@', '~');
                            memset(buf, fill(random_generator), bufsize);
                            pos = pos * bufsize;
                            return _file.dma_write(pos, buf, bufsize).finally([this, bufsize, bufptr = std::move(bufptr), perm = std::move(perm), pos] {
                                if ((this->req_type() == request_type::append) && (pos > _last_pos)) {
                                    _last_pos = pos;
                                }
                            }).discard_result();
                        });
                    });
                });
            });
        }).then([this] {
            return _file.flush();
        });
    }
protected:
    sstring type_str() const {
        return std::unordered_map<request_type, sstring>{
            { request_type::seqread, "SEQ READ" },
            { request_type::seqwrite, "SEQ WRITE" },
            { request_type::randread, "RAND READ" },
            { request_type::randwrite, "RAND WRITE" },
            { request_type::append , "APPEND" },
        }[_config.type];;
    }

   const sstring name() const {
        return _config.name;
    }

    request_type req_type() const {
        return _config.type;
    }

    sstring think_time() const {
        if (_config.shard_info.think_time == std::chrono::duration<float>(0)) {
            return "NO think time";
        } else {
            return sprint("%d us think time", std::chrono::duration_cast<std::chrono::microseconds>(_config.shard_info.think_time).count());
        }
    }

    size_t req_size() const {
        return _config.shard_info.request_size;
    }

    unsigned parallelism() const {
        return _config.shard_info.parallelism;
    }

    unsigned shares() const {
        return _config.shard_info.shares;
    }

    std::chrono::duration<float> total_duration() const {
        return _total_duration;
    }

    uint64_t total_bytes() const {
        return _bytes;
    }

    uint64_t max_latency() const {
        return max(_latencies);
    }

    uint64_t average_latency() const {
        return mean(_latencies);
    }

    uint64_t quantile_latency(double q) const {
        return quantile(_latencies, quantile_probability = q);
    }
private:
    bool is_sequential() const {
        return (req_type() == request_type::seqread) || (req_type() == request_type::seqwrite);
    }
    bool is_random() const {
        return (req_type() == request_type::randread) || (req_type() == request_type::randwrite);
    }

    bool is_read() const {
        return (req_type() == request_type::randread) || (req_type() == request_type::seqread);
    }

    uint64_t get_pos() {
        uint64_t pos;
        if (is_random()) {
            pos = _pos_distribution(random_generator) * req_size();
        } else {
            pos = _last_pos + req_size();
            if (is_sequential() && (pos >= file_data_size)) {
                pos = 0;
            }
        }
        _last_pos = pos;
        return pos;
    }

    void add_result(size_t bytes, std::chrono::microseconds latency) {
        _bytes += bytes;
        _latencies(latency.count());
    }
};

// The following two classes inherit from class data and help with pretty printing the output data.
struct class_data_description final : public class_data {
    friend std::basic_ostream<char>& operator<<(std::basic_ostream<char>& ss, const class_data_description& cl) {
        ss << cl.name() << ": " << cl.shares() << " shares, " << cl.req_size() << "-byte " << cl.type_str() << ", " << cl.parallelism() << " concurrent requests, ";
        ss << cl.think_time();
        return ss;
    }
    class_data_description(const class_data& cl) : class_data(cl) {}
};

struct class_data_results final : public class_data {
    friend std::basic_ostream<char>& operator<<(std::basic_ostream<char>& ss, const class_data_results& cl) {
        auto throughput_kbs = (cl.total_bytes() >> 10) / cl.total_duration().count();
        ss << "  Throughput         : " << std::setw(8) << throughput_kbs << " KB/s" << std::endl;
        ss << "  Lat average        : " << std::setw(8) << cl.average_latency() << " usec" << std::endl;

        for (auto& q: quantiles) {
            ss << "  Lat quantile=" << std::setw(5) << q << " : " << std::setw(8) << cl.quantile_latency(q) << " usec" << std::endl;
        }
        ss << "  Lat max            : " << std::setw(8) << cl.max_latency() << " usec" << std::endl;
        return ss;
    }
    class_data_results(const class_data& cl) : class_data(cl) {}
};

/// YAML parsing functions
namespace YAML {
template<>
struct convert<byte_size> {
    static bool decode(const Node& node, byte_size& bs) {
        auto str = node.as<std::string>();
        unsigned shift = 0;
        if (str.back() == 'B') {
            str.pop_back();
            shift = std::unordered_map<char, unsigned>{
                { 'k', 10 },
                { 'M', 20 },
                { 'G', 30 },
            }[str.back()];
            str.pop_back();
        }
        bs.size = (boost::lexical_cast<size_t>(str) << shift);
        return bs.size >= 512;
    }
};

template<>
struct convert<duration_time> {
    static bool decode(const Node& node, duration_time& dt) {
        auto str = node.as<std::string>();
        if (str == "0") {
            dt.time = 0ns;
            return true;
        }
        if (str.back() != 's') {
            return false;
        }
        str.pop_back();
        std::unordered_map<char, std::chrono::duration<float>> unit = {
            { 'n', 1ns },
            { 'u', 1us },
            { 'm', 1ms },
        };

        if (unit.count(str.back())) {
            auto u = str.back();
            str.pop_back();
            dt.time = (boost::lexical_cast<size_t>(str) * unit[u]);
        } else {
            dt.time = (boost::lexical_cast<size_t>(str) * 1s);
        }
        return true;
    }
};

template<>
struct convert<shard_config> {
    static bool decode(const Node& node, shard_config& shards) {
        try {
            auto str = node.as<std::string>();
            return (str == "all");
        } catch (YAML::TypedBadConversion<std::string> e) {
            shards = shard_config(boost::copy_range<std::unordered_set<unsigned>>(node.as<std::vector<unsigned>>()));
            return true;
        }
        return false;
    }
};

template<>
struct convert<request_type> {
    static bool decode(const Node& node, request_type& rt) {
        static std::unordered_map<std::string, request_type> mappings = {
            { "seqread", request_type::seqread },
            { "seqwrite", request_type::seqwrite},
            { "randread", request_type::randread },
            { "randwrite", request_type::randwrite },
            { "append", request_type::append},
        };
        auto reqstr = node.as<std::string>();
        if (!mappings.count(reqstr)) {
            return false;
        }
        rt = mappings[reqstr];
        return true;
    }
};

template<>
struct convert<shard_info> {
    static bool decode(const Node& node, shard_info& sl) {
        if (node["parallelism"]) {
            sl.parallelism = node["parallelism"].as<unsigned>();
        }
        if (node["shares"]) {
            sl.shares = node["shares"].as<unsigned>();
        }
        if (node["reqsize"]) {
            sl.request_size = node["reqsize"].as<byte_size>().size;
        }
        if (node["think_time"]) {
            sl.think_time = node["think_time"].as<duration_time>().time;
        }
        return true;
    }
};

template<>
struct convert<job_config> {
    static bool decode(const Node& node, job_config& cl) {
        cl.name = node["name"].as<std::string>();
        cl.type = node["type"].as<request_type>();
        cl.shard_placement = node["shards"].as<shard_config>();
        if (node["shard_info"]) {
            cl.shard_info = node["shard_info"].as<shard_info>();
        }
        return true;
    }
};
}

/// Each shard has one context, and the context is responsible for creating the classes that should
/// run in this shard.
class context {
    std::vector<class_data> _cl;

    sstring _dir;
    std::chrono::seconds _duration;

    semaphore _finished;
public:
    context(sstring dir, std::vector<job_config> req_config, unsigned duration)
            : _cl(boost::copy_range<std::vector<class_data>>(req_config | boost::adaptors::filtered([] (auto& cfg) {
                return cfg.shard_placement.is_set(engine().cpu_id());
            })))
            , _dir(dir)
            , _duration(duration)
            , _finished(0)
    {}

    future<> stop() { return make_ready_future<>(); }

    future<> start() {
        return parallel_for_each(_cl, [this] (auto& cl) {
            return cl.start(_dir);
        });
    }

    future<> issue_requests() {
        return parallel_for_each(_cl.begin(), _cl.end(), [this] (class_data& cl) {
            return cl.issue_requests(std::chrono::steady_clock::now() + _duration).finally([this] {
                _finished.signal(1);
            });
        });
    }

    future<> print_stats() {
        return _finished.wait(_cl.size()).then([this] {
            std::stringstream ss;
            ss << "Shard " << std::setw(2) << engine().cpu_id() << std::endl;
            auto idx = 0;
            for (auto& cl: _cl) {
                ss << " Class " << idx++ << "(" << std::setw(2) << class_data_description(cl) << ")" << std::endl;
                ss << class_data_results(cl) << std::endl;
            }
            ss << std::endl;
            std::cout << ss.str();
            return make_ready_future<>();
        });
    }
};

int class_data::idgen() {
    static thread_local int id = 0;
    return id++;
}

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    app_template app;
    auto opt_add = app.add_options();
    opt_add
        ("directory", bpo::value<sstring>()->default_value("."), "directory where to execute the test")
        ("duration", bpo::value<unsigned>()->default_value(10), "for how long (in seconds) to run the test")
        ("conf", bpo::value<sstring>()->default_value("./conf.yaml"), "YAML file containing benchmark specification")
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
            auto& duration = opts["duration"].as<unsigned>();
            auto& yaml = opts["conf"].as<sstring>();
            YAML::Node doc = YAML::LoadFile(yaml);
            auto reqs = doc.as<std::vector<job_config>>();
            return ctx.start(directory, reqs, duration).then([&ctx] {
                engine().at_exit([&ctx] {
                    return ctx.stop();
                });
                std::cout << "Creating initial files..." << std::endl;
                return ctx.invoke_on_all([] (auto& c) {
                    return c.start();
                }).then([&ctx] {
                    std::cout << "Starting evaluation..." << std::endl;
                    return ctx.invoke_on_all([] (auto& c) {
                        return c.issue_requests();
                    });
                }).then([&ctx] {
                    return ctx.invoke_on_all([] (auto& c) {
                        return c.print_stats();
                    });
                }).or_terminate();
            });
        });
    });
}

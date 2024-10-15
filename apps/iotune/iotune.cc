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
 * Copyright (C) 2018 ScyllaDB
 *
 * The goal of this program is to allow a user to properly configure the Seastar I/O
 * scheduler.
 */
#include <iostream>
#include <chrono>
#include <random>
#include <memory>
#include <ranges>
#include <vector>
#include <cmath>
#include <sys/vfs.h>
#include <sys/sysmacros.h>
#include <boost/program_options.hpp>
#include <boost/iterator/counting_iterator.hpp>
#include <fstream>
#include <wordexp.h>
#include <yaml-cpp/yaml.h>
#include <fmt/printf.h>
#include <seastar/core/seastar.hh>
#include <seastar/core/file.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/posix.hh>
#include <seastar/core/aligned_buffer.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/fsqual.hh>
#include <seastar/core/loop.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/log.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/read_first_line.hh>

using namespace seastar;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

logger iotune_logger("iotune");

using iotune_clock = std::chrono::steady_clock;
static thread_local std::default_random_engine random_generator(std::chrono::duration_cast<std::chrono::nanoseconds>(iotune_clock::now().time_since_epoch()).count());

void check_device_properties(fs::path dev_sys_file) {
    auto sched_file = dev_sys_file / "queue" / "scheduler";
    auto sched_string = read_first_line(sched_file);
    auto beg = sched_string.find('[');
    size_t len = sched_string.size();
    if (beg == sstring::npos) {
        beg = 0;
    } else {
        auto end = sched_string.find(']');
        if (end != sstring::npos) {
            len = end - beg - 1;
        }
        beg++;
    }
    auto scheduler = sched_string.substr(beg, len);
    if ((scheduler != "noop") && (scheduler != "none")) {
        iotune_logger.warn("Scheduler for {} set to {}. It is recommend to set it to noop before evaluation so as not to skew the results.",
                sched_file.string(), scheduler);
    }

    auto nomerges_file = dev_sys_file / "queue" / "nomerges";
    auto nomerges = read_first_line_as<unsigned>(nomerges_file);
    if (nomerges != 2u) {
        iotune_logger.warn("nomerges for {} set to {}. It is recommend to set it to 2 before evaluation so that merges are disabled. Results can be skewed otherwise.",
                nomerges_file.string(), nomerges);
    }

    auto write_cache_file = dev_sys_file / "queue" / "write_cache";
    auto write_cache = read_first_line_as<std::string>(write_cache_file);
    if (write_cache == "write back") {
        iotune_logger.warn("write_cache for {} is set to write back. Some disks have poor implementation of this mode, pay attention to the measurements accuracy.",
                write_cache_file.string());
    }
}

struct evaluation_directory {
    sstring _name;
    // We know that if we issue more than this, they will be blocked on linux anyway.
    unsigned _max_iodepth = 0;
    uint64_t _available_space;
    uint64_t _min_data_transfer_size = 512;
    unsigned _disks_per_array = 0;

    void scan_device(unsigned dev_maj, unsigned dev_min) {
        scan_device(fmt::format("{}:{}", dev_maj, dev_min));
    }

    void scan_device(std::string dev_str) {
        scan_device(fs::path("/sys/dev/block") / dev_str);
    }

    void scan_device(fs::path sys_file) {
        try {
            sys_file = fs::canonical(sys_file);
            bool is_leaf = true;
            if (fs::exists(sys_file / "slaves")) {
                for (auto& dev : fs::directory_iterator(sys_file / "slaves")) {
                    is_leaf = false;
                    scan_device(read_first_line(dev.path() / "dev"));
                }
            }

            // our work is done if not leaf. We'll tune the leaves
            if (!is_leaf) {
                return;
            }

            if (fs::exists(sys_file / "partition")) {
                scan_device(sys_file.remove_filename());
            } else {
                check_device_properties(sys_file);
                auto queue_dir = sys_file / "queue";
                auto disk_min_io_size = read_first_line_as<uint64_t>(queue_dir / "minimum_io_size");

                _min_data_transfer_size = std::max(_min_data_transfer_size, disk_min_io_size);
                _max_iodepth += read_first_line_as<uint64_t>(queue_dir / "nr_requests");
                _disks_per_array++;
            }
        } catch (std::system_error& se) {
            iotune_logger.error("Error while parsing sysfs. Will continue with guessed values: {}", se.what());
            _max_iodepth = 128;
        }
        _disks_per_array = std::max(_disks_per_array, 1u);
    }
public:
    evaluation_directory(sstring name)
        : _name(name)
        , _available_space(fs::space(fs::path(_name)).available)
    {}

    unsigned max_iodepth() const {
        return _max_iodepth;
    }

    fs::path path() const {
        return fs::path(_name);
    }

    const sstring& name() const {
        return _name;
    }

    unsigned disks_per_array() const {
        return _disks_per_array;
    }

    uint64_t minimum_io_size() const {
        return _min_data_transfer_size;
    }

    future<> discover_directory() {
        return seastar::async([this] {
            auto f = open_directory(_name).get();
            auto st = f.stat().get();
            f.close().get();

            scan_device(major(st.st_dev), minor(st.st_dev));
        });
    }

    uint64_t available_space() const {
        return _available_space;
    }
};

struct io_rates {
    float bytes_per_sec = 0;
    float iops = 0;
    io_rates operator+(const io_rates& a) const {
        return io_rates{bytes_per_sec + a.bytes_per_sec, iops + a.iops};
    }

    io_rates& operator+=(const io_rates& a) {
        bytes_per_sec += a.bytes_per_sec;
        iops += a.iops;
        return *this;
    }
};

struct row_stats {
    size_t points;
    double average;
    double stdev;

    float stdev_percents() const {
        return points > 0 ? stdev / average : 0.0;
    }
};

template <typename T>
static row_stats get_row_stats_for(const std::vector<T>& v) {
    if (v.size() == 0) {
        return row_stats{0, 0.0, 0.0};
    }

    double avg = std::accumulate(v.begin(), v.end(), 0.0) / v.size();
    double stdev = std::sqrt(std::transform_reduce(v.begin(), v.end(), 0.0,
                std::plus<double>(), [avg] (auto& v) -> double { return (v - avg) * (v - avg); }) / v.size());

    return row_stats{ v.size(), avg, stdev };
}

class invalid_position : public std::exception {
public:
    virtual const char* what() const noexcept {
        return "file access position invalid";
    }
};

struct position_generator {
    virtual uint64_t get_pos() = 0;
    virtual bool is_sequential() const = 0;
    virtual ~position_generator() {}
};

class sequential_issuer : public position_generator {
    size_t _buffer_size;
    uint64_t _position = 0;
    uint64_t _size_limit;
public:
    sequential_issuer(size_t buffer_size, uint64_t size_limit)
        : _buffer_size(buffer_size)
        , _size_limit(size_limit)
    {}

    virtual bool is_sequential() const {
        return true;
    }

    virtual uint64_t get_pos() {
        if (_position >= _size_limit) {
            // Wrap around if reaching EOF. The write bandwidth is lower,
            // and we also split the write bandwidth among shards, while we
            // read only from shard 0, so shard 0's file may not be large
            // enough to read from.
            _position = 0;
        }
        auto pos = _position;
        _position += _buffer_size;
        return pos;
    }
};

class random_issuer : public position_generator {
    size_t _buffer_size;
    uint64_t _last_position;
    std::uniform_int_distribution<uint64_t> _pos_distribution;
public:
    random_issuer(size_t buffer_size, uint64_t last_position)
        : _buffer_size(buffer_size)
        , _last_position(last_position)
        , _pos_distribution(0, (last_position / buffer_size) - 1)
    {}

    virtual bool is_sequential() const {
        return false;
    }

    virtual uint64_t get_pos() {
        uint64_t pos = _pos_distribution(random_generator) * _buffer_size;
        if (pos >= _last_position) {
            throw invalid_position();
        }
        return pos;
    }
};

class request_issuer {
public:
    virtual future<size_t> issue_request(uint64_t pos, char* buf, uint64_t size) = 0;
    virtual ~request_issuer() {}
};


class write_request_issuer : public request_issuer {
    file _file;
public:
    explicit write_request_issuer(file f) : _file(f) {}
    future<size_t> issue_request(uint64_t pos, char* buf, uint64_t size) override {
        return _file.dma_write(pos, buf, size);
    }
};

class read_request_issuer : public request_issuer {
    file _file;
public:
    explicit read_request_issuer(file f) : _file(f) {}
    future<size_t> issue_request(uint64_t pos, char* buf, uint64_t size) override {
        return _file.dma_read(pos, buf, size);
    }
};

class io_worker {
    class requests_rate_meter {
        std::vector<unsigned>& _rates;
        const unsigned& _requests;
        unsigned _prev_requests = 0;
        timer<> _tick;

        static constexpr auto period = 1s;

    public:
        requests_rate_meter(std::chrono::duration<double> duration, std::vector<unsigned>& rates, const unsigned& requests)
            : _rates(rates)
            , _requests(requests)
            , _tick([this] {
                _rates.push_back(_requests - _prev_requests);
                _prev_requests = _requests;
            })
        {
            _rates.reserve(256); // ~2 minutes
            if (duration > 4 * period) {
                _tick.arm_periodic(period);
            }
        }

        ~requests_rate_meter() {
            if (_tick.armed()) {
                _tick.cancel();
            } else {
                _rates.push_back(_requests);
            }
        }
    };

    uint64_t _bytes = 0;
    uint64_t _max_offset = 0;
    unsigned _requests = 0;
    size_t _buffer_size;
    std::chrono::time_point<iotune_clock, std::chrono::duration<double>> _start_measuring;
    std::chrono::time_point<iotune_clock, std::chrono::duration<double>> _end_measuring;
    std::chrono::time_point<iotune_clock, std::chrono::duration<double>> _end_load;
    // track separately because in the sequential case we may exhaust the file before _duration
    std::chrono::time_point<iotune_clock, std::chrono::duration<double>> _last_time_seen;

    requests_rate_meter _rr_meter;
    std::unique_ptr<position_generator> _pos_impl;
    std::unique_ptr<request_issuer> _req_impl;
public:
    bool is_sequential() const {
        return _pos_impl->is_sequential();
    }

    bool should_stop() const {
        return iotune_clock::now() >= _end_load;
    }

    io_worker(size_t buffer_size, std::chrono::duration<double> duration, std::unique_ptr<request_issuer> reqs, std::unique_ptr<position_generator> pos, std::vector<unsigned>& rates)
        : _buffer_size(buffer_size)
        , _start_measuring(iotune_clock::now() + std::chrono::duration<double>(10ms))
        , _end_measuring(_start_measuring + duration)
        , _end_load(_end_measuring + 10ms)
        , _last_time_seen(_start_measuring)
        , _rr_meter(duration, rates, _requests)
        , _pos_impl(std::move(pos))
        , _req_impl(std::move(reqs))
    {}

    std::unique_ptr<char[], free_deleter> get_buffer() {
        return allocate_aligned_buffer<char>(_buffer_size, _buffer_size);
    }

    future<> issue_request(char* buf) {
        uint64_t pos = _pos_impl->get_pos();
        return _req_impl->issue_request(pos, buf, _buffer_size).then([this, pos] (size_t size) {
            auto now = iotune_clock::now();
            _max_offset = std::max(_max_offset, pos + size);
            if ((now > _start_measuring) && (now < _end_measuring)) {
                _last_time_seen = now;
                _bytes += size;
                _requests++;
            }
        });
    }

    uint64_t max_offset() const noexcept { return _max_offset; }

    io_rates get_io_rates() const {
        io_rates rates;
        auto t = _last_time_seen - _start_measuring;
        if (!t.count()) {
            throw std::runtime_error("No data collected");
        }
        rates.bytes_per_sec = _bytes / t.count();
        rates.iops = _requests / t.count();
        return rates;
    }
};

class test_file {
public:
    enum class pattern { sequential, random };
private:
    fs::path _dirpath;
    uint64_t _file_size;
    file _file;
    uint64_t _forced_random_io_buffer_size;

    std::unique_ptr<position_generator> get_position_generator(size_t buffer_size, pattern access_pattern) {
        if (access_pattern == pattern::sequential) {
            return std::make_unique<sequential_issuer>(buffer_size, _file_size);
        } else {
            return std::make_unique<random_issuer>(buffer_size, _file_size);
        }
    }

    uint64_t calculate_buffer_size(pattern access_pattern, uint64_t buffer_size, uint64_t operation_alignment) const {
        if (access_pattern == pattern::random && _forced_random_io_buffer_size != 0u) {
            return _forced_random_io_buffer_size;
        }

        return std::max(buffer_size, operation_alignment);
    }

public:
    test_file(const ::evaluation_directory& dir, uint64_t maximum_size, uint64_t random_io_buffer_size)
        : _dirpath(dir.path() / fs::path(fmt::format("ioqueue-discovery-{}", this_shard_id())))
        , _file_size(maximum_size)
        , _forced_random_io_buffer_size(random_io_buffer_size)
    {}

    future<> create_data_file() {
        // XFS likes access in many directories better.
        return make_directory(_dirpath.string()).then([this] {
            auto testfile = _dirpath / fs::path("testfile");
            file_open_options options;
            options.extent_allocation_size_hint = _file_size;
            return open_file_dma(testfile.string(), open_flags::rw | open_flags::create, std::move(options)).then([this, testfile] (file file) {
                _file = file;
                if (this_shard_id() == 0) {
                    iotune_logger.info("Filesystem parameters: read alignment {}, write alignment {}", _file.disk_read_dma_alignment(), _file.disk_write_dma_alignment());
                }
                return remove_file(testfile.string()).then([this] {
                    return remove_file(_dirpath.string());
                });
            }).then([this] {
                return _file.truncate(_file_size);
            });
        });
    }

    future<io_rates> do_workload(std::unique_ptr<io_worker> worker_ptr, unsigned max_os_concurrency, bool update_file_size = false) {
        if (update_file_size) {
            _file_size = 0;
        }

        auto worker = worker_ptr.get();
        auto concurrency = std::views::iota(0u, max_os_concurrency);
        return parallel_for_each(std::move(concurrency), [worker] (unsigned idx) {
            auto bufptr = worker->get_buffer();
            auto buf = bufptr.get();
            return do_until([worker] { return worker->should_stop(); }, [buf, worker] {
                return worker->issue_request(buf);
            }).finally([alive = std::move(bufptr)] {});
        }).then_wrapped([this, worker = std::move(worker_ptr), update_file_size] (future<> f) {
            try {
                f.get();
            } catch (invalid_position& ip) {
                // expected if sequential. Example: reading and the file ended.
                if (!worker->is_sequential()) {
                    throw;
                }
            }

            if (update_file_size) {
                _file_size = worker->max_offset();
            }
            return make_ready_future<io_rates>(worker->get_io_rates());
        });
    }

    future<io_rates> read_workload(size_t buffer_size, pattern access_pattern, unsigned max_os_concurrency, std::chrono::duration<double> duration, std::vector<unsigned>& rates) {
        buffer_size = calculate_buffer_size(access_pattern, buffer_size, _file.disk_read_dma_alignment());
        auto worker = std::make_unique<io_worker>(buffer_size, duration, std::make_unique<read_request_issuer>(_file), get_position_generator(buffer_size, access_pattern), rates);
        return do_workload(std::move(worker), max_os_concurrency);
    }

    future<io_rates> write_workload(size_t buffer_size, pattern access_pattern, unsigned max_os_concurrency, std::chrono::duration<double> duration, std::vector<unsigned>& rates) {
        buffer_size = calculate_buffer_size(access_pattern, buffer_size, _file.disk_write_dma_alignment());
        auto worker = std::make_unique<io_worker>(buffer_size, duration, std::make_unique<write_request_issuer>(_file), get_position_generator(buffer_size, access_pattern), rates);
        bool update_file_size = worker->is_sequential();
        return do_workload(std::move(worker), max_os_concurrency, update_file_size).then([this] (io_rates r) {
            return _file.flush().then([r = std::move(r)] () mutable {
                return make_ready_future<io_rates>(std::move(r));
            });
        });
    }

    future<> stop() {
        return _file ? _file.close() : make_ready_future<>();
    }
};

class iotune_multi_shard_context {
    ::evaluation_directory _test_directory;
    uint64_t _random_io_buffer_size;

    unsigned per_shard_io_depth() const {
        auto iodepth = _test_directory.max_iodepth() / smp::count;
        if (this_shard_id() < _test_directory.max_iodepth() % smp::count) {
            iodepth++;
        }
        return std::min(iodepth, 128u);
    }
    seastar::sharded<test_file> _iotune_test_file;

    std::vector<unsigned> serial_rates;
    seastar::sharded<std::vector<unsigned>> sharded_rates;

public:
    future<> stop() {
        return _iotune_test_file.stop().then([this] { return sharded_rates.stop(); });
    }

    future<> start() {
       const auto maximum_size = (_test_directory.available_space() / (2 * smp::count));
       return _iotune_test_file.start(_test_directory, maximum_size, _random_io_buffer_size).then([this] {
           return sharded_rates.start();
       });
    }

    future<row_stats> get_serial_rates() {
        row_stats ret = get_row_stats_for<unsigned>(serial_rates);
        serial_rates.clear();
        return make_ready_future<row_stats>(ret);
    }

    future<row_stats> get_sharded_worst_rates() {
        return sharded_rates.map_reduce0([] (std::vector<unsigned>& rates) {
            row_stats ret = get_row_stats_for<unsigned>(rates);
            rates.clear();
            return ret;
        }, row_stats{0, 0.0, 0.0},
        [] (const row_stats& res, row_stats lres) {
            return res.stdev < lres.stdev ? lres : res;
        });
    }

    future<> create_data_file() {
        return _iotune_test_file.invoke_on_all([] (test_file& tf) {
            return tf.create_data_file();
        });
    }

    future<io_rates> write_sequential_data(unsigned shard, size_t buffer_size, std::chrono::duration<double> duration) {
        return _iotune_test_file.invoke_on(shard, [this, buffer_size, duration] (test_file& tf) {
            return tf.write_workload(buffer_size, test_file::pattern::sequential, 4 * _test_directory.disks_per_array(), duration, serial_rates);
        });
    }

    future<io_rates> read_sequential_data(unsigned shard, size_t buffer_size, std::chrono::duration<double> duration) {
        return _iotune_test_file.invoke_on(shard, [this, buffer_size, duration] (test_file& tf) {
            return tf.read_workload(buffer_size, test_file::pattern::sequential, 4 * _test_directory.disks_per_array(), duration, serial_rates);
        });
    }

    future<io_rates> write_random_data(size_t buffer_size, std::chrono::duration<double> duration) {
        return _iotune_test_file.map_reduce0([buffer_size, this, duration] (test_file& tf) {
            const auto shard_io_depth = per_shard_io_depth();
            if (shard_io_depth == 0) {
                return make_ready_future<io_rates>();
            } else {
                return tf.write_workload(buffer_size, test_file::pattern::random, shard_io_depth, duration, sharded_rates.local());
            }
        }, io_rates(), std::plus<io_rates>());
    }

    future<io_rates> read_random_data(size_t buffer_size, std::chrono::duration<double> duration) {
        return _iotune_test_file.map_reduce0([buffer_size, this, duration] (test_file& tf) {
            const auto shard_io_depth = per_shard_io_depth();
            if (shard_io_depth == 0) {
                return make_ready_future<io_rates>();
            } else {
                return tf.read_workload(buffer_size, test_file::pattern::random, shard_io_depth, duration, sharded_rates.local());
            }
        }, io_rates(), std::plus<io_rates>());
    }

private:
    template <typename Fn>
    future<uint64_t> saturate(float rate_threshold, size_t buffer_size, std::chrono::duration<double> duration, Fn&& workload) {
        return _iotune_test_file.invoke_on(0, [this, rate_threshold, buffer_size, duration, workload] (test_file& tf) {
            return (tf.*workload)(buffer_size, test_file::pattern::sequential, 1, duration, serial_rates).then([this, rate_threshold, buffer_size, duration, workload] (io_rates rates) {
                serial_rates.clear();
                if (rates.bytes_per_sec < rate_threshold) {
                    // The throughput with the given buffer-size is already "small enough", so
                    // return back its previous value
                    return make_ready_future<uint64_t>(buffer_size * 2);
                } else {
                    return saturate(rate_threshold, buffer_size / 2, duration, workload);
                }
            });
        });
    }

public:
    future<uint64_t> saturate_write(float rate_threshold, size_t buffer_size, std::chrono::duration<double> duration) {
        return saturate(rate_threshold, buffer_size, duration, &test_file::write_workload);
    }

    future<uint64_t> saturate_read(float rate_threshold, size_t buffer_size, std::chrono::duration<double> duration) {
        return saturate(rate_threshold, buffer_size, duration, &test_file::read_workload);
    }

    iotune_multi_shard_context(::evaluation_directory dir, uint64_t random_io_buffer_size)
        : _test_directory(dir)
        , _random_io_buffer_size(random_io_buffer_size)
    {}
};

struct disk_descriptor {
    std::string mountpoint;
    uint64_t read_iops;
    uint64_t read_bw;
    uint64_t write_iops;
    uint64_t write_bw;
    std::optional<uint64_t> read_sat_len;
    std::optional<uint64_t> write_sat_len;
};

void string_to_file(sstring conf_file, sstring buf) {
    auto f = file_desc::open(conf_file, O_WRONLY | O_CLOEXEC | O_CREAT | O_TRUNC, 0664);
    auto ret = f.write(buf.data(), buf.size());
    if (!ret || (*ret != buf.size())) {
        throw std::runtime_error(fmt::format("Can't write {}: {}", conf_file, *ret));
    }
}

void write_configuration_file(sstring conf_file, std::string format, sstring properties_file) {
    sstring buf;
    if (format == "seastar") {
        buf = fmt::format("io-properties-file={}\n", properties_file);
    } else {
        buf = fmt::format("SEASTAR_IO=\"--io-properties-file={}\"\n", properties_file);
    }
    string_to_file(conf_file, buf);
}

void write_property_file(sstring conf_file, std::vector<disk_descriptor> disk_descriptors) {
    YAML::Emitter out;
    out << YAML::BeginMap;
    out << YAML::Key << "disks";
    out << YAML::BeginSeq;
    for (auto& desc : disk_descriptors) {
        out << YAML::BeginMap;
        out << YAML::Key << "mountpoint" << YAML::Value << desc.mountpoint;
        out << YAML::Key << "read_iops" << YAML::Value << desc.read_iops;
        out << YAML::Key << "read_bandwidth" << YAML::Value << desc.read_bw;
        out << YAML::Key << "write_iops" << YAML::Value << desc.write_iops;
        out << YAML::Key << "write_bandwidth" << YAML::Value << desc.write_bw;
        if (desc.read_sat_len) {
            out << YAML::Key << "read_saturation_length" << YAML::Value << *desc.read_sat_len;
        }
        if (desc.write_sat_len) {
            out << YAML::Key << "write_saturation_length" << YAML::Value << *desc.write_sat_len;
        }
        out << YAML::EndMap;
    }
    out << YAML::EndSeq;
    out << YAML::EndMap;
    out << YAML::Newline;

    string_to_file(conf_file, sstring(out.c_str(), out.size()));
}

// Returns the mountpoint of a path. It works by walking backwards from the canonical path
// (absolute, with symlinks resolved), until we find a point that crosses a device ID.
fs::path mountpoint_of(sstring filename) {
    fs::path mnt_candidate = fs::canonical(fs::path(filename));
    std::optional<dev_t> candidate_id = {};
    auto current = mnt_candidate;
    do {
        auto f = open_directory(current.string()).get();
        auto st = f.stat().get();
        if ((candidate_id) && (*candidate_id != st.st_dev)) {
            return mnt_candidate;
        }
        mnt_candidate = current;
        candidate_id = st.st_dev;
        current = current.parent_path();
    } while (mnt_candidate != current);

    return mnt_candidate;
}

int main(int ac, char** av) {
    namespace bpo = boost::program_options;
    bool fs_check = false;

    app_template::config app_cfg;
    app_cfg.name = "IOTune";

    app_template app(std::move(app_cfg));
    auto opt_add = app.add_options();
    opt_add
        ("evaluation-directory", bpo::value<std::vector<sstring>>()->required(), "directory where to execute the evaluation")
        ("properties-file", bpo::value<sstring>(), "path in which to write the YAML file")
        ("options-file", bpo::value<sstring>(), "path in which to write the legacy conf file")
        ("duration", bpo::value<unsigned>()->default_value(120), "time, in seconds, for which to run the test")
        ("format", bpo::value<sstring>()->default_value("seastar"), "Configuration file format (seastar | envfile)")
        ("fs-check", bpo::bool_switch(&fs_check), "perform FS check only")
        ("accuracy", bpo::value<unsigned>()->default_value(3), "acceptable deviation of measurements (percents)")
        ("saturation", bpo::value<sstring>()->default_value(""), "measure saturation lengths (read | write | both) (this is very slow!)")
        ("random-io-buffer-size", bpo::value<unsigned>()->default_value(0), "force buffer size for random write and random read")
    ;

    return app.run(ac, av, [&] {
        return seastar::async([&] {
            auto& configuration = app.configuration();
            auto eval_dirs = configuration["evaluation-directory"].as<std::vector<sstring>>();
            auto format = configuration["format"].as<sstring>();
            auto duration = std::chrono::duration<double>(configuration["duration"].as<unsigned>() * 1s);
            auto accuracy = configuration["accuracy"].as<unsigned>();
            auto saturation = configuration["saturation"].as<sstring>();
            auto random_io_buffer_size = configuration["random-io-buffer-size"].as<unsigned>();

            bool read_saturation, write_saturation;
            if (saturation == "") {
                read_saturation = false;
                write_saturation = false;
            } else if (saturation == "both") {
                read_saturation = true;
                write_saturation = true;
            } else if (saturation == "read") {
                read_saturation = true;
                write_saturation = false;
            } else if (saturation == "write") {
                read_saturation = false;
                write_saturation = true;
            } else {
                fmt::print("Bad --saturation value\n");
                return 1;
            }

            std::vector<disk_descriptor> disk_descriptors;
            std::unordered_map<sstring, sstring> mountpoint_map;
            // We want to evaluate once per mountpoint, but we still want to write in one of the
            // directories that we were provided - we may not have permissions to write into the
            // mountpoint itself. If we are passed more than one directory per mountpoint, we don't
            // really care to which one we write, so this simple hash will do.
            for (auto& eval_dir : eval_dirs) {
                mountpoint_map[mountpoint_of(eval_dir).string()] = eval_dir;
            }
            for (auto eval: mountpoint_map) {
                auto mountpoint = eval.first;
                auto eval_dir = eval.second;

                if (!filesystem_has_good_aio_support(eval_dir, false)) {
                    iotune_logger.error("Linux AIO is not supported by filesystem at {}", eval_dir);
                    return 1;
                }

                auto rec = 10000000000ULL;
                auto avail = fs_avail(eval_dir).get();
                if (avail < rec) {
                    uint64_t val;
                    const char* units;
                    if (avail >= 1000000000) {
                        val = (avail + 500000000) / 1000000000;
                        units = "GB";
                    } else if (avail >= 1000000) {
                        val = (avail + 500000) / 1000000;
                        units = "MB";
                    } else {
                        val = avail;
                        units = "bytes";
                    }
                    iotune_logger.warn("Available space on filesystem at {}: {} {}: is less than recommended: {} GB",
                                       eval_dir, val, units, rec / 1000000000ULL);
                }

                iotune_logger.info("{} passed sanity checks", eval_dir);
                if (fs_check) {
                    continue;
                }

                // Directory is the same object for all tests.
                ::evaluation_directory test_directory(eval_dir);
                test_directory.discover_directory().get();
                iotune_logger.info("Disk parameters: max_iodepth={} disks_per_array={} minimum_io_size={}",
                        test_directory.max_iodepth(), test_directory.disks_per_array(), test_directory.minimum_io_size());

                if (test_directory.max_iodepth() < smp::count) {
                    iotune_logger.warn("smp::count={} is greater than max_iodepth={} - shards above max_io_depth "
                                       "will be ignored during random read and random write measurements",
                                       smp::count, test_directory.max_iodepth());
                }

                if (random_io_buffer_size != 0u) {
                    iotune_logger.info("Forcing buffer_size={} for random IO!", random_io_buffer_size);
                }

                ::iotune_multi_shard_context iotune_tests(test_directory, random_io_buffer_size);
                iotune_tests.start().get();
                auto stop = defer([&iotune_tests] () noexcept {
                    try {
                        iotune_tests.stop().get();
                    } catch (...) {
                        fmt::print("Error occurred during iotune context shutdown: {}", std::current_exception());
                        abort();
                    }
                });

                row_stats rates;
                auto accuracy_msg = [accuracy, &rates] {
                    auto stdev = rates.stdev_percents() * 100.0;
                    return (accuracy == 0 || stdev > accuracy) ? fmt::format(" (deviation {}%)", int(round(stdev))) : std::string("");
                };

                iotune_tests.create_data_file().get();

                fmt::print("Starting Evaluation. This may take a while...\n");
                fmt::print("Measuring sequential write bandwidth: ");
                std::cout.flush();
                io_rates write_bw;
                size_t sequential_buffer_size = 1 << 20;
                for (unsigned shard = 0; shard < smp::count; ++shard) {
                    write_bw += iotune_tests.write_sequential_data(shard, sequential_buffer_size, duration * 0.70 / smp::count).get();
                }
                write_bw.bytes_per_sec /= smp::count;
                rates = iotune_tests.get_serial_rates().get();
                fmt::print("{} MB/s{}\n", uint64_t(write_bw.bytes_per_sec / (1024 * 1024)), accuracy_msg());

                std::optional<uint64_t> write_sat;

                if (write_saturation) {
                    fmt::print("Measuring write saturation length: ");
                    std::cout.flush();
                    write_sat = iotune_tests.saturate_write(write_bw.bytes_per_sec * (1.0 - rates.stdev_percents()), sequential_buffer_size/2, duration * 0.70).get();
                    fmt::print("{}\n", *write_sat);
                }

                fmt::print("Measuring sequential read bandwidth: ");
                std::cout.flush();
                auto read_bw = iotune_tests.read_sequential_data(0, sequential_buffer_size, duration * 0.1).get();
                rates = iotune_tests.get_serial_rates().get();
                fmt::print("{} MB/s{}\n", uint64_t(read_bw.bytes_per_sec / (1024 * 1024)), accuracy_msg());

                std::optional<uint64_t> read_sat;

                if (read_saturation) {
                    fmt::print("Measuring read saturation length: ");
                    std::cout.flush();
                    read_sat = iotune_tests.saturate_read(read_bw.bytes_per_sec * (1.0 - rates.stdev_percents()), sequential_buffer_size/2, duration * 0.1).get();
                    fmt::print("{}\n", *read_sat);
                }

                fmt::print("Measuring random write IOPS: ");
                std::cout.flush();
                auto write_iops = iotune_tests.write_random_data(test_directory.minimum_io_size(), duration * 0.1).get();
                rates = iotune_tests.get_sharded_worst_rates().get();
                fmt::print("{} IOPS{}\n", uint64_t(write_iops.iops), accuracy_msg());

                fmt::print("Measuring random read IOPS: ");
                std::cout.flush();
                auto read_iops = iotune_tests.read_random_data(test_directory.minimum_io_size(), duration * 0.1).get();
                rates = iotune_tests.get_sharded_worst_rates().get();
                fmt::print("{} IOPS{}\n", uint64_t(read_iops.iops), accuracy_msg());

                struct disk_descriptor desc;
                desc.mountpoint = mountpoint;
                desc.read_iops = read_iops.iops;
                desc.read_bw = read_bw.bytes_per_sec;
                desc.read_sat_len = read_sat;
                desc.write_iops = write_iops.iops;
                desc.write_bw = write_bw.bytes_per_sec;
                desc.write_sat_len = write_sat;
                disk_descriptors.push_back(std::move(desc));
            }

            if (fs_check) {
                return 0;
            }

            auto file = "properties file";
            try {
                if (configuration.count("properties-file")) {
                    fmt::print("Writing result to {}\n", configuration["properties-file"].as<sstring>());
                    write_property_file(configuration["properties-file"].as<sstring>(), disk_descriptors);
                }

                file = "configuration file";
                if (configuration.count("options-file")) {
                    fmt::print("Writing result to {}\n", configuration["options-file"].as<sstring>());
                    write_configuration_file(configuration["options-file"].as<sstring>(), format, configuration["properties-file"].as<sstring>());
                }
            } catch (...) {
                iotune_logger.error("Exception when writing {}: {}.\nPlease add the above values manually to your seastar command line.", file, std::current_exception());
                return 1;
            }
            return 0;
        });
    });
}

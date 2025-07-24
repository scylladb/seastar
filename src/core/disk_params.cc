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
 * Copyright 2025 ScyllaDB
 */


#ifdef SEASTAR_MODULE
module;
#endif

#include <chrono>
#include <unordered_set>
#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <sys/stat.h>
#include <yaml-cpp/yaml.h>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/disk_params.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/smp_options.hh>
#include <seastar/util/conversions.hh>

#endif

using namespace std::chrono_literals;

namespace YAML {
template<>
struct convert<seastar::internal::disk_params> {
    static bool decode(const Node& node, seastar::internal::disk_params& mp) {
        using namespace seastar;
        if (node["mountpoints"]) {
            mp.mountpoints = node["mountpoints"].as<std::vector<std::string>>();
        } else {
            mp.mountpoints.push_back(node["mountpoint"].as<std::string>());
        }
        mp.read_bytes_rate = parse_memory_size(node["read_bandwidth"].as<std::string>());
        mp.read_req_rate = parse_memory_size(node["read_iops"].as<std::string>());
        mp.write_bytes_rate = parse_memory_size(node["write_bandwidth"].as<std::string>());
        mp.write_req_rate = parse_memory_size(node["write_iops"].as<std::string>());
        if (node["read_saturation_length"]) {
            mp.read_saturation_length = parse_memory_size(node["read_saturation_length"].as<std::string>());
        }
        if (node["write_saturation_length"]) {
            mp.write_saturation_length = parse_memory_size(node["write_saturation_length"].as<std::string>());
        }
        if (node["duplex"]) {
            mp.duplex = node["duplex"].as<bool>();
        }
        if (node["rate_factor"]) {
            mp.rate_factor = node["rate_factor"].as<float>();
        }
        if (node["max_cost_function"]) {
            mp.max_cost_function = node["max_cost_function"].as<bool>();
        }
        return true;
    }
};
}

namespace seastar {

extern logger seastar_logger;

namespace internal {

void disk_config_params::parse_config(const smp_options& smp_opts, const reactor_options& reactor_opts) {
    seastar_logger.debug("smp::count: {}", smp::count);
    _latency_goal = std::chrono::duration_cast<std::chrono::duration<double>>(latency_goal_opt(reactor_opts) * 1ms);
    seastar_logger.debug("latency_goal: {}", latency_goal().count());
    _flow_ratio_backpressure_threshold = reactor_opts.io_flow_ratio_threshold.get_value();
    seastar_logger.debug("flow-ratio threshold: {}", _flow_ratio_backpressure_threshold);
    _stall_threshold = reactor_opts.io_completion_notify_ms.defaulted() ? std::chrono::milliseconds::max() : reactor_opts.io_completion_notify_ms.get_value() * 1ms;

    if (smp_opts.num_io_groups) {
        _num_io_groups = smp_opts.num_io_groups.get_value();
        if (!_num_io_groups) {
            throw std::runtime_error("num-io-groups must be greater than zero");
        }
    }
    if (smp_opts.io_properties_file && smp_opts.io_properties) {
        throw std::runtime_error("Both io-properties and io-properties-file specified. Don't know which to trust!");
    }

    std::optional<YAML::Node> doc;
    if (smp_opts.io_properties_file) {
        doc = YAML::LoadFile(smp_opts.io_properties_file.get_value());
    } else if (smp_opts.io_properties) {
        doc = YAML::Load(smp_opts.io_properties.get_value());
    }

    if (doc) {
        if (!doc->IsMap()) {
            throw std::runtime_error("Bogus io-properties (did you mix up --io-properties and --io-properties-file?)");
        }
        for (auto&& section : *doc) {
            auto sec_name = section.first.as<std::string>();
            if (sec_name != "disks") {
                throw std::runtime_error(fmt::format("While parsing I/O options: section {} currently unsupported.", sec_name));
            }
            auto disks = section.second.as<std::vector<disk_params>>();
            unsigned queue_id_gen = 1;
            std::unordered_set<dev_t> devices;
            for (auto& d : disks) {
                for (auto mp : d.mountpoints) {
                    struct ::stat buf;
                    auto ret = stat(mp.c_str(), &buf);
                    if (ret < 0) {
                        throw std::runtime_error(fmt::format("Couldn't stat {}", mp));
                    }

                    auto st_dev = S_ISBLK(buf.st_mode) ? buf.st_rdev : buf.st_dev;
                    d.devices.push_back(st_dev);
                    auto [ it, inserted ] = devices.insert(st_dev);
                    if (!inserted) {
                        throw std::runtime_error(fmt::format("Mountpoint {}, device {} already configured", mp, st_dev));
                    }
                }

                if (_disks.size() >= _max_queues) {
                    throw std::runtime_error(fmt::format("Configured number of queues {} is larger than the maximum {}",
                                                _disks.size(), _max_queues));
                }

                d.read_bytes_rate *= d.rate_factor;
                d.write_bytes_rate *= d.rate_factor;
                d.read_req_rate *= d.rate_factor;
                d.write_req_rate *= d.rate_factor;

                if (d.read_bytes_rate == 0 || d.write_bytes_rate == 0 ||
                        d.read_req_rate == 0 || d.write_req_rate == 0) {
                    throw std::runtime_error(fmt::format("R/W bytes and req rates must not be zero"));
                }

                unsigned q = queue_id_gen++;
                seastar_logger.debug("queue-id: {} mountpoints: {} devices: {}", q, d.mountpoints, d.devices);
                _disks.emplace(q, d);
            }
        }
    }

    // Placeholder for unconfigured disks.
    disk_params d = {};
    d.devices.push_back(0);
    _disks.emplace(0, d);
}

struct io_queue::config disk_config_params::generate_config(unsigned q, unsigned nr_groups) const {
    auto it = _disks.find(q);
    if (it == _disks.end()) {
        throw std::runtime_error(fmt::format("No disk configuration for queue-id {}", q));
    }
    return generate_config(it->second, q, nr_groups);
}

struct io_queue::config disk_config_params::generate_config(const disk_params& p, unsigned q, unsigned nr_groups) const {
    seastar_logger.debug("generate_config queue-id: {}", q);
    struct io_queue::config cfg;

    cfg.id = q;

    if (p.read_bytes_rate != std::numeric_limits<uint64_t>::max()) {
        cfg.blocks_count_rate = (io_queue::read_request_base_count * (unsigned long)per_io_group(p.read_bytes_rate, nr_groups)) >> io_queue::block_size_shift;
        cfg.disk_blocks_write_to_read_multiplier = (io_queue::read_request_base_count * p.read_bytes_rate) / p.write_bytes_rate;
    }
    if (p.read_req_rate != std::numeric_limits<uint64_t>::max()) {
        cfg.req_count_rate = io_queue::read_request_base_count * (unsigned long)per_io_group(p.read_req_rate, nr_groups);
        cfg.disk_req_write_to_read_multiplier = (io_queue::read_request_base_count * p.read_req_rate) / p.write_req_rate;
    }
    if (p.read_saturation_length != std::numeric_limits<uint64_t>::max()) {
        cfg.disk_read_saturation_length = p.read_saturation_length;
    }
    if (p.write_saturation_length != std::numeric_limits<uint64_t>::max()) {
        cfg.disk_write_saturation_length = p.write_saturation_length;
    }
    cfg.mountpoint = fmt::to_string(fmt::join(p.mountpoints, ":"));
    cfg.duplex = p.duplex;
    cfg.rate_limit_duration = latency_goal();
    cfg.flow_ratio_backpressure_threshold = _flow_ratio_backpressure_threshold;
    // Block count limit should not be less than the minimal IO size on the device
    // On the other hand, even this is not good enough -- in the worst case the
    // scheduler will self-tune to allow for the single 64k request, while it would
    // be better to sacrifice some IO latency, but allow for larger concurrency
    cfg.block_count_limit_min = (64 << 10) >> io_queue::block_size_shift;
    cfg.stall_threshold = stall_threshold();

    cfg.max_cost_function = p.max_cost_function;

    return cfg;
}

} // namespace internal

} // namespace seastar

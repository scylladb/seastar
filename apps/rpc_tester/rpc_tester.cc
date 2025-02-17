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
 * Copyright (C) 2022 ScyllaDB
 */

#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <ranges>
#include <yaml-cpp/yaml.h>
#include <fmt/core.h>
#pragma GCC diagnostic push
// see https://github.com/boostorg/accumulators/pull/54
#pragma GCC diagnostic ignored "-Wuninitialized"
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/p_square_quantile.hpp>
#include <boost/accumulators/statistics/extended_p_square.hpp>
#include <boost/accumulators/statistics/extended_p_square_quantile.hpp>
#pragma GCC diagnostic pop
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/with_scheduling_group.hh>
#include <seastar/core/sleep.hh>
#include <seastar/rpc/rpc.hh>
#include <seastar/util/assert.hh>

using namespace seastar;
using namespace boost::accumulators;
using namespace std::chrono_literals;

struct serializer {};

template <typename T, typename Output>
inline
void write_arithmetic_type(Output& out, T v) {
    static_assert(std::is_arithmetic_v<T>, "must be arithmetic type");
    return out.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T, typename Input>
inline
T read_arithmetic_type(Input& in) {
    static_assert(std::is_arithmetic_v<T>, "must be arithmetic type");
    T v;
    in.read(reinterpret_cast<char*>(&v), sizeof(T));
    return v;
}

template <typename Output>
inline void write(serializer, Output& output, int32_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, uint32_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, int64_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, uint64_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, double v) { return write_arithmetic_type(output, v); }
template <typename Input>
inline int32_t read(serializer, Input& input, rpc::type<int32_t>) { return read_arithmetic_type<int32_t>(input); }
template <typename Input>
inline uint32_t read(serializer, Input& input, rpc::type<uint32_t>) { return read_arithmetic_type<uint32_t>(input); }
template <typename Input>
inline uint64_t read(serializer, Input& input, rpc::type<uint64_t>) { return read_arithmetic_type<uint64_t>(input); }
template <typename Input>
inline uint64_t read(serializer, Input& input, rpc::type<int64_t>) { return read_arithmetic_type<int64_t>(input); }
template <typename Input>
inline double read(serializer, Input& input, rpc::type<double>) { return read_arithmetic_type<double>(input); }

template <typename Output>
inline void write(serializer, Output& out, const sstring& v) {
    write_arithmetic_type(out, uint32_t(v.size()));
    out.write(v.c_str(), v.size());
}

template <typename Input>
inline sstring read(serializer, Input& in, rpc::type<sstring>) {
    auto size = read_arithmetic_type<uint32_t>(in);
    sstring ret = uninitialized_string(size);
    in.read(ret.data(), size);
    return ret;
}

using payload_t = std::vector<uint64_t>;

template <typename Output>
inline void write(serializer, Output& out, const payload_t& v) {
    write_arithmetic_type(out, uint32_t(v.size()));
    out.write((const char*)v.data(), v.size() * sizeof(payload_t::value_type));
}

template <typename Input>
inline payload_t read(serializer, Input& in, rpc::type<payload_t>) {
    auto size = read_arithmetic_type<uint32_t>(in);
    payload_t ret;
    ret.resize(size);
    in.read((char*)ret.data(), size * sizeof(payload_t::value_type));
    return ret;
}

class pause_distribution {
public:

    virtual std::chrono::duration<double> get() = 0;

    template <typename Dur>
    Dur get_as() {
        return std::chrono::duration_cast<Dur>(get());
    }

    virtual ~pause_distribution() {}
};

class steady_process : public pause_distribution {
    std::chrono::duration<double> _pause;
public:
    steady_process(std::chrono::duration<double> period) : _pause(period) { }
    std::chrono::duration<double> get() override { return _pause; }
};

std::unique_ptr<pause_distribution> make_steady_pause(std::chrono::duration<double> d) {
    return std::make_unique<steady_process>(d);
}

class uniform_process : public pause_distribution {
    std::random_device _rd;
    std::mt19937 _rng;
    std::uniform_real_distribution<double> _range;

public:
    uniform_process(std::chrono::duration<double> min, std::chrono::duration<double> max)
            : _rng(_rd()) , _range(min.count(), max.count()) { }

    std::chrono::duration<double> get() override {
        return std::chrono::duration<double>(_range(_rng));
    }
};

struct duration_range {
    std::chrono::duration<double> min;
    std::chrono::duration<double> max;
};

std::unique_ptr<pause_distribution> make_uniform_pause(duration_range range) {
    return std::make_unique<uniform_process>(range.min, range.max);
}

struct client_config {
    bool nodelay = true;
};

struct server_config {
    bool nodelay = true;
};

struct job_config {
    std::string name;
    std::string type;
    std::string verb;
    unsigned parallelism;
    unsigned shares = 100;
    std::chrono::duration<double> exec_time;
    std::optional<duration_range> exec_time_range;
    std::optional<std::chrono::duration<double>> sleep_time;
    std::optional<duration_range> sleep_time_range;
    std::optional<std::chrono::duration<double>> timeout;
    size_t payload;

    bool client = false;
    bool server = false;

    std::chrono::seconds duration;
    std::string sg_name;
    scheduling_group sg = default_scheduling_group();
};

struct config {
    client_config client;
    server_config server;
    std::vector<job_config> jobs;
};

struct duration_time {
    std::chrono::duration<float> time;
};

struct byte_size {
    uint64_t size;
};

namespace YAML {

template<>
struct convert<client_config> {
    static bool decode(const Node& node, client_config& cfg) {
        if (node["nodelay"]) {
            cfg.nodelay = node["nodelay"].as<bool>();
        }
        return true;
    }
};

template<>
struct convert<server_config> {
    static bool decode(const Node& node, server_config& cfg) {
        if (node["nodelay"]) {
            cfg.nodelay = node["nodelay"].as<bool>();
        }
        return true;
    }
};

template <>
struct convert<job_config> {
    static bool decode(const Node& node, job_config& cfg) {
        cfg.name = node["name"].as<std::string>();
        cfg.type = node["type"].as<std::string>();
        cfg.parallelism = node["parallelism"].as<unsigned>();
        if (cfg.type == "rpc") {
            cfg.verb = node["verb"].as<std::string>();
            cfg.payload = node["payload"].as<byte_size>().size;
            cfg.client = true;
            if (node["sleep_time"]) {
                cfg.sleep_time = node["sleep_time"].as<duration_time>().time;
            }
            if (node["timeout"]) {
                cfg.timeout = node["timeout"].as<duration_time>().time;
            }
        } else if (cfg.type == "cpu") {
            if (node["execution_time"]) {
                cfg.exec_time = node["execution_time"].as<duration_time>().time;
            } else {
                duration_range r;
                r.min = node["execution_time_min"].as<duration_time>().time;
                r.max = node["execution_time_max"].as<duration_time>().time;
                cfg.exec_time_range = r;
            }
            if (node["sleep_time"]) {
                cfg.sleep_time = node["sleep_time"].as<duration_time>().time;
            } else if (node["sleep_time_min"] && node["sleep_time_max"]) {
                duration_range r;
                r.min = node["sleep_time_min"].as<duration_time>().time;
                r.max = node["sleep_time_max"].as<duration_time>().time;
                cfg.sleep_time_range = r;
            }
            cfg.client = !node["side"] || (node["side"].as<std::string>() == "client");
            cfg.server = !node["side"] || (node["side"].as<std::string>() == "server");
        }
        if (node["shares"]) {
            cfg.shares = node["shares"].as<unsigned>();
        }
        if (node["sched_group"]) {
            cfg.sg_name = node["sched_group"].as<std::string>();
        } else {
            cfg.sg_name = cfg.name;
        }
        return true;
    }
};

template<>
struct convert<config> {
    static bool decode(const Node& node, config& cfg) {
        if (node["client"]) {
            cfg.client = node["client"].as<client_config>();
        }
        if (node["server"]) {
            cfg.server = node["server"].as<server_config>();
        }
        if (node["jobs"]) {
            cfg.jobs = node["jobs"].as<std::vector<job_config>>();
        }
        return true;
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

        std::chrono::duration<double> unit;
        if (str.back() == 'm') {
            unit = 1ms;
            str.pop_back();
        } else if (str.back() == 'u') {
            unit = 1us;
            str.pop_back();
        } else if (str.back() == 'n') {
            unit = 1ns;
            str.pop_back();
        } else {
            unit = 1s;
        }

        dt.time = boost::lexical_cast<size_t>(str) * unit;
        return true;
    }
};

template<>
struct convert<byte_size> {
    static bool decode(const Node& node, byte_size& bs) {
        auto str = node.as<std::string>();
        unsigned shift = 0;
        if (str.back() == 'B') {
            str.pop_back();
            if (str.back() != 'k') {
                return false;
            }
            str.pop_back();
            shift = 10;
        }
        bs.size = (boost::lexical_cast<size_t>(str) << shift);
        return bs.size;
    }
};

} // YAML namespace

enum class rpc_verb : int32_t {
    HELLO = 0,
    BYE = 1,
    ECHO = 2,
    WRITE = 3,
};

using rpc_protocol = rpc::protocol<serializer, rpc_verb>;
static std::array<double, 4> quantiles = { 0.5, 0.95, 0.99, 0.999};

class job {
public:
    virtual std::string name() const = 0;
    virtual future<> run() = 0;
    virtual void emit_result(YAML::Emitter& out) const = 0;
    virtual ~job() {}
};

class job_rpc : public job {
    using accumulator_type = accumulator_set<double, stats<tag::extended_p_square_quantile(quadratic), tag::mean, tag::max>>;

    job_config _cfg;
    socket_address _caddr;
    client_config _ccfg;
    rpc_protocol& _rpc;
    std::unique_ptr<rpc_protocol::client> _client;
    std::function<future<>(unsigned)> _call;
    std::chrono::steady_clock::time_point _stop;
    uint64_t _total_messages = 0;
    accumulator_type _latencies;

    future<> call_echo(unsigned dummy) {
        auto cln = _rpc.make_client<uint64_t(uint64_t)>(rpc_verb::ECHO);
        if (_cfg.timeout) {
            return cln(*_client, std::chrono::duration_cast<seastar::rpc::rpc_clock_type::duration>(*_cfg.timeout), dummy).discard_result();
        } else {
            return cln(*_client, dummy).discard_result();
        }
    }

    future<> call_write(unsigned dummy, const payload_t& pl) {
        return _rpc.make_client<uint64_t(payload_t)>(rpc_verb::WRITE)(*_client, pl).then([exp = pl.size()] (auto res) {
            SEASTAR_ASSERT(res == exp);
            return make_ready_future<>();
        });
    }

public:
    job_rpc(job_config cfg, rpc_protocol& rpc, client_config ccfg, socket_address caddr)
            : _cfg(cfg)
            , _caddr(std::move(caddr))
            , _ccfg(ccfg)
            , _rpc(rpc)
            , _stop(std::chrono::steady_clock::now() + _cfg.duration)
            , _latencies(extended_p_square_probabilities = quantiles)
    {
        if (_cfg.verb == "echo") {
            _call = [this] (unsigned x) { return call_echo(x); };
        } else if (_cfg.verb == "write") {
            payload_t payload;
            payload.resize(_cfg.payload / sizeof(payload_t::value_type), 0);
            _call = [this, payload = std::move(payload)] (unsigned x) { return call_write(x, payload); };
        } else if (_cfg.verb == "vecho") {
            _call = [this] (unsigned x) {
                fmt::print("{}.{} send echo\n", this_shard_id(), x);
                return call_echo(x).then([x] {
                        fmt::print("{}.{} got response\n", this_shard_id(), x);
                }).handle_exception([x] (auto ex) {
                        fmt::print("{}.{} got error {}\n", this_shard_id(), x, ex);
                });
            };
        } else {
            throw std::runtime_error("unknown verb");
        }
    }

    virtual std::string name() const override { return _cfg.name; }

    virtual future<> run() override {
      return with_scheduling_group(_cfg.sg, [this] {
        rpc::client_options co;
        co.tcp_nodelay = _ccfg.nodelay;
        co.isolation_cookie = _cfg.sg_name;
        _client = std::make_unique<rpc_protocol::client>(_rpc, co, _caddr);
        return parallel_for_each(std::views::iota(0u, _cfg.parallelism), [this] (auto dummy) {
          auto f = make_ready_future<>();
          if (_cfg.sleep_time) {
              // Do initial small delay to de-synchronize fibers
              f = seastar::sleep(std::chrono::duration_cast<std::chrono::nanoseconds>(*_cfg.sleep_time / _cfg.parallelism * dummy));
          }
          return std::move(f).then([this, dummy] {
            return do_until([this] {
                return std::chrono::steady_clock::now() > _stop;
            }, [this, dummy] {
                _total_messages++;
                auto now = std::chrono::steady_clock::now();
                return _call(dummy).then([this, start = now] {
                    std::chrono::microseconds lat = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start);
                    _latencies(lat.count());
                }).then([this] {
                    if (_cfg.sleep_time) {
                        return seastar::sleep(std::chrono::duration_cast<std::chrono::nanoseconds>(*_cfg.sleep_time));
                    } else {
                        return make_ready_future<>();
                    }
                });
            });
          });
        }).finally([this] {
            return _client->stop();
        });
      });
    }

    virtual void emit_result(YAML::Emitter& out) const override {
        out << YAML::Key << "messages" << YAML::Value << _total_messages;
        out << YAML::Key << "latencies" << YAML::Comment("usec");
        out << YAML::BeginMap;
        out << YAML::Key << "average" << YAML::Value << (uint64_t)mean(_latencies);
        for (auto& q: quantiles) {
            out << YAML::Key << fmt::format("p{}", q) << YAML::Value << (uint64_t)quantile(_latencies, quantile_probability = q);
        }
        out << YAML::Key << "max" << YAML::Value << (uint64_t)max(_latencies);
        out << YAML::EndMap;
    }
};

class job_cpu : public job {
    job_config _cfg;
    std::chrono::steady_clock::time_point _stop;
    uint64_t _total_invocations = 0;
    std::unique_ptr<pause_distribution> _pause;
    std::unique_ptr<pause_distribution> _sleep;

    std::unique_ptr<pause_distribution> make_pause() {
        if (_cfg.exec_time_range) {
            return make_uniform_pause(*_cfg.exec_time_range);
        } else {
            return make_steady_pause(_cfg.exec_time);
        }
    }

    std::unique_ptr<pause_distribution> make_sleep() {
        if (_cfg.sleep_time) {
            return make_steady_pause(*_cfg.sleep_time);
        }
        if (_cfg.sleep_time_range) {
            return make_uniform_pause(*_cfg.sleep_time_range);
        }
        return nullptr;
    }

public:
    job_cpu(job_config cfg)
            : _cfg(cfg)
            , _pause(make_pause())
            , _sleep(make_sleep())
    {
    }

    virtual std::string name() const override { return _cfg.name; }
    virtual void emit_result(YAML::Emitter& out) const override {
        out << YAML::Key << "total" << YAML::Value << _total_invocations;
    }

    virtual future<> run() override {
        _stop = std::chrono::steady_clock::now() + _cfg.duration;
        return with_scheduling_group(_cfg.sg, [this] {
          return parallel_for_each(std::views::iota(0u, _cfg.parallelism), [this] (auto dummy) {
            return do_until([this] {
                return std::chrono::steady_clock::now() > _stop;
            }, [this] {
                _total_invocations++;
                auto start  = std::chrono::steady_clock::now();
                auto pause = _pause->get();
                while ((std::chrono::steady_clock::now() - start) < pause);
                if (!_sleep) {
                    return make_ready_future<>();
                } else {
                    auto sleep = std::chrono::duration_cast<std::chrono::nanoseconds>(_sleep->get());
                    return seastar::sleep(sleep);
                }
            });
          });
        });
    }
};

class context {
    std::unique_ptr<rpc_protocol> _rpc;
    std::unique_ptr<rpc_protocol::server> _server;
    std::unique_ptr<rpc_protocol::client> _client;
    promise<> _bye;
    promise<> _server_jobs;
    config _cfg;
    std::vector<std::unique_ptr<job>> _jobs;
    std::unordered_map<std::string, scheduling_group> _sched_groups;

    std::unique_ptr<job> make_job(job_config cfg, std::optional<socket_address> caddr) {
        if (cfg.type == "rpc") {
            return std::make_unique<job_rpc>(cfg, *_rpc, _cfg.client, *caddr);
        }
        if (cfg.type == "cpu") {
            return std::make_unique<job_cpu>(cfg);
        }

        throw std::runtime_error("unknown job type");
    }

    future<> run_jobs() {
        return parallel_for_each(_jobs, [] (auto& job) {
            return job->run();
        });
    }

    rpc::isolation_config isolate_connection(std::string group_name) {
        rpc::isolation_config cfg;
        if (group_name != "") {
            cfg.sched_group = _sched_groups[group_name];
        }
        return cfg;
    }

public:
    context(std::optional<socket_address> laddr, std::optional<socket_address> caddr, uint16_t port, config cfg, std::unordered_map<std::string, scheduling_group> groups)
            : _rpc(std::make_unique<rpc_protocol>(serializer{}))
            , _cfg(cfg)
            , _sched_groups(std::move(groups))
    {
        _rpc->register_handler(rpc_verb::HELLO, [this] {
            fmt::print("Got HELLO message from client\n");
            run_jobs().discard_result().forward_to(std::move(_server_jobs));
        });
        _rpc->register_handler(rpc_verb::BYE, [this] {
            fmt::print("Got BYE message from client, exiting\n");
            _bye.set_value();
        });
        _rpc->register_handler(rpc_verb::ECHO, [] (uint64_t val) {
            return make_ready_future<uint64_t>(val);
        });
        _rpc->register_handler(rpc_verb::WRITE, [] (payload_t val) {
            return make_ready_future<uint64_t>(val.size());
        });

        if (laddr) {
            rpc::server_options so;
            so.tcp_nodelay = _cfg.server.nodelay;
            rpc::resource_limits limits;
            limits.isolate_connection = [this] (sstring cookie) { return isolate_connection(cookie); };
            _server = std::make_unique<rpc_protocol::server>(*_rpc, so, *laddr, limits);

            for (auto&& jc : _cfg.jobs) {
                if (jc.server) {
                    _jobs.push_back(make_job(jc, {}));
                }
            }
        }

        if (caddr) {
            rpc::client_options co;
            co.tcp_nodelay = _cfg.client.nodelay;
            _client = std::make_unique<rpc_protocol::client>(*_rpc, co, *caddr);

            for (auto&& jc : _cfg.jobs) {
                if (jc.client) {
                    _jobs.push_back(make_job(jc, *caddr));
                }
            }
        }
    }

    future<> start() {
        if (_client) {
            return _rpc->make_client<void()>(rpc_verb::HELLO)(*_client);
        }

        return make_ready_future<>();
    }

    future<> stop() {
        if (_client) {
            return _rpc->make_client<void()>(rpc_verb::BYE)(*_client).finally([this] {
                return _client->stop();
            });
        }

        if (_server) {
            return _server->stop();
        }

        return make_ready_future<>();
    }

    future<> run() {
        if (_client) {
            return run_jobs();
        }

        if (_server) {
            return when_all(_bye.get_future(), _server_jobs.get_future()).discard_result();
        }

        return make_ready_future<>();
    }

    future<> emit_result(YAML::Emitter& out) const {
        for (const auto& job : _jobs) {
            out << YAML::Key << job->name();
            out << YAML::BeginMap;
            job->emit_result(out);
            out << YAML::EndMap;
        }

        return make_ready_future<>();
    }
};

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    app_template app;
    auto opt_add = app.add_options();
    opt_add
        ("listen", bpo::value<sstring>()->default_value(""), "address to start server on")
        ("connect", bpo::value<sstring>()->default_value(""), "address to connect client to")
        ("port", bpo::value<int>()->default_value(9123), "port to listen on or connect to")
        ("conf", bpo::value<sstring>()->default_value("./conf.yaml"), "config with jobs and options")
        ("duration", bpo::value<unsigned>()->default_value(30), "duration in seconds")
    ;

    sharded<context> ctx;
    return app.run(ac, av, [&] {
        return seastar::async([&] {
            auto& opts = app.configuration();
            auto& listen = opts["listen"].as<sstring>();
            auto& connect = opts["connect"].as<sstring>();
            auto& port = opts["port"].as<int>();
            auto& conf = opts["conf"].as<sstring>();
            auto duration = std::chrono::seconds(opts["duration"].as<unsigned>());

            std::optional<socket_address> laddr;
            if (listen != "") {
                if (listen[0] == '.' || listen[0] == '/') {
                    unix_domain_addr addr(listen);
                    laddr.emplace(std::move(addr));
                } else {
                    ipv4_addr addr(listen, port);
                    laddr.emplace(std::move(addr));
                }
            }
            std::optional<socket_address> caddr;
            if (connect != "") {
                if (connect[0] == '.' || connect[0] == '/') {
                    unix_domain_addr addr(connect);
                    caddr.emplace(std::move(addr));
                } else {
                    ipv4_addr addr(connect, port);
                    caddr.emplace(std::move(addr));
                }
            }

            YAML::Node doc = YAML::LoadFile(conf);
            auto cfg = doc.as<config>();
            std::unordered_map<std::string, scheduling_group> groups;

            for (auto&& jc : cfg.jobs) {
                jc.duration = duration;
                if (groups.count(jc.sg_name) == 0) {
                    fmt::print("Make sched group {}, {} shares\n", jc.sg_name, jc.shares);
                    groups[jc.sg_name] = create_scheduling_group(jc.sg_name, jc.shares).get();
                }
                jc.sg = groups[jc.sg_name];
            }

            ctx.start(laddr, caddr, port, cfg, groups).get();
            ctx.invoke_on_all(&context::start).get();
            ctx.invoke_on_all(&context::run).get();

            YAML::Emitter out;
            out << YAML::BeginDoc;
            out << YAML::BeginSeq;
            for (unsigned i = 0; i < smp::count; i++) {
                out << YAML::BeginMap;
                out << YAML::Key << "shard" << YAML::Value << i;
                ctx.invoke_on(i, [&out] (auto& c) {
                    return c.emit_result(out);
                }).get();
                out << YAML::EndMap;
            }
            out << YAML::EndSeq;
            out << YAML::EndDoc;
            std::cout << out.c_str();

            ctx.stop().get();
        });
    });
}

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

#include <random>
#include <ranges>
#include <fmt/core.h>
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/later.hh>

using namespace seastar;
using namespace std::chrono;

class thinker {
    class poisson_process {
        std::random_device _rd;
        std::mt19937 _rng;
        std::exponential_distribution<double> _exp;

    public:
        poisson_process(duration<double> period)
                : _rng(_rd())
                , _exp(1.0 / period.count())
        {
        }

        duration<double> get() {
            return duration<double>(_exp(_rng));
        }
    };

    poisson_process _pause;
    bool _stop;
    future<> _done;

    future<> start_thinking(unsigned concurrency) {
        return parallel_for_each(std::views::iota(0u, concurrency), [this] (unsigned f) {
            return do_until([this] { return _stop; }, [this] {
                auto until = steady_clock::now() + _pause.get();
                while (steady_clock::now() < until) {
                    ; // do nothing
                }
                return make_ready_future<>();
            });
        });
    }

public:
    thinker(unsigned concurrency, microseconds think) noexcept
        : _pause(think)
        , _stop(false)
        , _done(start_thinking(concurrency))
    {
        fmt::print("shard {} starts {}x{}us thinkers\n", this_shard_id(), concurrency, think.count());
    }

    future<> stop() {
        _stop = true;
        return std::move(_done);
    }
};

enum class respond_type { ready, yield, io, timer };

static respond_type parse_respond_type(std::string s) {
    if (s == "ready") {
        return respond_type::ready;
    }
    if (s == "yield") {
        return respond_type::yield;
    }
    if (s == "io") {
        return respond_type::io;
    }
    if (s == "timer") {
        return respond_type::timer;
    }

    throw std::runtime_error("unknown respond type");
}

class worker {
    const unsigned _to;

    std::unique_ptr<thinker> _think;

    uint64_t _total;
    bool _stop;
    future<> _done;

    static unsigned my_target(unsigned targets) noexcept {
        unsigned group_size = (smp::count + (targets - 1)) / targets;
        unsigned group_no = this_shard_id() / group_size;
        return group_size * group_no;
    }

    future<> start_working(unsigned concurrency, respond_type resp, microseconds tmo) {
        return parallel_for_each(std::views::iota(0u, concurrency), [this, resp, tmo] (unsigned f) {
            return do_until([this] { return _stop; }, [this, resp, tmo] {
                return smp::submit_to(_to, [resp, tmo] {
                    switch (resp) {
                    case respond_type::ready:
                        return make_ready_future<>();
                    case respond_type::yield:
                        return yield();
                    case respond_type::io:
                        return check_for_io_immediately();
                    case respond_type::timer:
                        return seastar::sleep<lowres_clock>(tmo);
                    }

                    __builtin_unreachable();
                }).then([this] {
                    _total++;
                    return make_ready_future<>();
                });
            });
        });
    }

public:
    struct config {
        unsigned targets;
        unsigned thinkers;
        microseconds think;
        respond_type respond;
        microseconds respond_tmo;
        unsigned concurrency;
    };

    worker(config cfg) noexcept
        : _to(my_target(cfg.targets))
        , _think(is_target() && (cfg.thinkers > 0) ? std::make_unique<thinker>(cfg.thinkers, cfg.think) : nullptr)
        , _total(0)
        , _stop(false)
        , _done(start_working(cfg.concurrency, cfg.respond, cfg.respond_tmo))
    {
    }

    future<> stop() {
        if (_stop) {
            return make_ready_future<>();
        }

        _stop = true;
        return std::move(_done).then([this] {
            return _think ? _think->stop() : make_ready_future<>();
        });
    }

    bool is_target() const noexcept { return _to == this_shard_id(); }
    uint64_t total() const noexcept { return _total; }
};

class stats {
    uint64_t _min = std::numeric_limits<uint64_t>::max();
    uint64_t _max = std::numeric_limits<uint64_t>::min();
    uint64_t _sum = 0;
    unsigned _nr = 0;
    const unsigned _norm;

public:

    stats(unsigned norm) noexcept : _norm(norm) {}

    void append(uint64_t val) noexcept {
        _min = std::min(_min, val);
        _max = std::max(_max, val);
        _sum += val;
        _nr++;
    }

    double min() const noexcept { return (double)_min / _norm; }
    double max() const noexcept { return (double)_max / _norm; }
    double avg() const noexcept { return _nr > 0 ? (double)_sum / _nr / _norm : 0.0; }
    unsigned nr() const noexcept { return _nr; }
};

int main(int ac, char** av) {
    app_template at;
    namespace bpo = boost::program_options;
    at.add_options()
            ("duration", bpo::value<unsigned>()->default_value(32), "time to run the test (seconds)")
            ("targets", bpo::value<unsigned>()->default_value(1), "number of responder shards")
            ("thinkers", bpo::value<unsigned>()->default_value(0), "thinker fibers to run in parallel on targets")
            ("think", bpo::value<unsigned>()->default_value(100), "time (us) thinkers busyloop for")
            ("respond", bpo::value<std::string>()->default_value("ready"), "how to respond on target (ready, yield, io, timer)")
            ("respond-timeout", bpo::value<unsigned>()->default_value(1), "the 'timer' respond timeout (us)")
            ("concurrency", bpo::value<unsigned>()->default_value(1), "smp::submit_to operations to issue in parallel")
        ;

    return at.run(ac, av, [&at] {
        auto duration = seconds(at.configuration()["duration"].as<unsigned>());
        worker::config cfg;
        cfg.targets = at.configuration()["targets"].as<unsigned>();
        cfg.thinkers = at.configuration()["thinkers"].as<unsigned>();
        cfg.think = microseconds(at.configuration()["think"].as<unsigned>());
        cfg.respond = parse_respond_type(at.configuration()["respond"].as<std::string>());
        cfg.respond_tmo = microseconds(at.configuration()["respond-timeout"].as<unsigned>());
        cfg.concurrency = at.configuration()["concurrency"].as<unsigned>();

        return async([cfg, duration] {
            sharded<worker> workers;
            auto start = steady_clock::now();

            workers.start(cfg).get();
            seastar::sleep(duration).get();
            workers.invoke_on_all(&worker::stop).get();

            auto real_duration = duration_cast<seconds>(steady_clock::now() - start);
            fmt::print("took {}s (expected {}s)\n", real_duration.count(), duration.count());
            stats st(real_duration.count()), st_targets(real_duration.count());
            for (unsigned i = 0; i < smp::count; i++) {
                workers.invoke_on(i, [&st, &st_targets] (worker& w) {
                    if (w.is_target()) {
                        st_targets.append(w.total());
                    } else {
                        st.append(w.total());
                    }
                }).get();
            }
            fmt::print("workers({:2}): min {:.1f} avg {:.1f} max {:.1f} op/s\n", st.nr(), st.min(), st.avg(), st.max());
            fmt::print("targets({:2}): min {:.1f} avg {:.1f} max {:.1f} op/s\n", st_targets.nr(), st_targets.min(), st_targets.avg(), st_targets.max());

            workers.stop().get();
        });
    });
}

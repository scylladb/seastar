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

#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/io_queue.hh>
#include <seastar/core/internal/io_request.hh>
#include <seastar/core/internal/io_sink.hh>

using namespace seastar;
using namespace std::chrono;

static seastar::logger testlog("testlog");

struct io_queue_for_tests {
    struct io_group_for_tests {
        io_group_ptr group;

        io_group_for_tests(const io_queue::config& cfg, unsigned nr_queues)
            : group(std::make_shared<io_group>(cfg, nr_queues))
        {
        }
    };

    internal::io_sink sink;
    io_queue queue;

    io_queue_for_tests(io_group_for_tests& group)
        : queue(group.group, sink)
    {
    }

    void poll_once() {
        queue.poll_io_queue();
        sink.drain([] (const internal::io_request& rq, io_completion* desc) -> bool {
            const auto& op = rq.as<internal::io_request::operation::write>();
            desc->complete_with(op.size);
            return true;
        });
    }

    future<> enqueue(scheduling_group sg, size_t req_size) {
        return queue.queue_request(internal::priority_class(sg),
            internal::io_direction_and_length(internal::io_direction_and_length::read_idx, req_size),
            internal::io_request::make_read(0, 0, nullptr, req_size, false),
            nullptr, {}).discard_result();
    }

    future<> enqueue(scheduling_group sg, unsigned nr, size_t req_size) {
        return parallel_for_each(std::views::iota(0u, nr), [this, sg, req_size] (auto i) -> future<> {
            return enqueue(sg, req_size);
        });
    }
};

int main(int ac, char** av) {
    app_template at;
    namespace bpo = boost::program_options;
    at.add_options()
            ("iterations", bpo::value<unsigned>()->default_value(7), "Number of iterations")
            ("nr-queues", bpo::value<unsigned>()->default_value(4), "Number of queues to simulate")
            ("iops", bpo::value<unsigned>()->default_value(10000), "IOPS")
            ("bandwidth-mb", bpo::value<unsigned>()->default_value(100), "Bandwidth in MB/s")
            ("concurrency", bpo::value<unsigned>()->default_value(4), "Concurrency of competing low-prio group")
            ("fg-concurrency", bpo::value<unsigned>()->default_value(1), "Concurrency of foreground group (burst size)")
            ("poll-interval-us", bpo::value<unsigned>()->default_value(250), "Interval between queue polls")
        ;

    return at.run(ac, av, [&at] {
        auto nr_iterations = at.configuration()["iterations"].as<unsigned>();
        auto nr_queues = at.configuration()["nr-queues"].as<unsigned>();
        auto iops = at.configuration()["iops"].as<unsigned>();
        auto bandwidth_mb = at.configuration()["bandwidth-mb"].as<unsigned>();
        auto bg_concurrency = at.configuration()["concurrency"].as<unsigned>();
        auto fg_concurrency = at.configuration()["fg-concurrency"].as<unsigned>();
        auto poll_interval = microseconds(at.configuration()["poll-interval-us"].as<unsigned>());

        return async([nr_iterations, iops, bandwidth_mb, nr_queues, bg_concurrency, fg_concurrency, poll_interval] {
            const size_t bg_req_size = 128*1024;
            const size_t fg_req_size = 4*1024;

            auto bg_sg = create_scheduling_group("bg", 200).get();
            auto fg_sg = create_scheduling_group("fg", 1000).get();

            io_queue::config cfg{0};
            cfg.req_count_rate = io_queue::read_request_base_count * iops;
            cfg.blocks_count_rate = io_queue::read_request_base_count * ((bandwidth_mb << 20) >> io_queue::block_size_shift);
            cfg.with_metrics = false;
            io_queue_for_tests::io_group_for_tests group(cfg, nr_queues);
            std::vector<std::unique_ptr<io_queue_for_tests>> queues;
            for (unsigned i = 0; i < nr_queues; i++) {
                queues.emplace_back(std::make_unique<io_queue_for_tests>(group));
            }

            std::chrono::duration<double> total_delay(0);
            double total_bg_iops = 0;
            const unsigned bg_reqs_per_iter = nr_queues * bg_concurrency;
            for (unsigned iter = 0; iter < nr_iterations; iter++) {
                std::vector<future<>> bgv;
                auto bg_start = steady_clock::now();
                for (auto& q : queues) {
                    auto f = q->enqueue(bg_sg, bg_concurrency, bg_req_size);
                    bgv.push_back(std::move(f));
                }
                auto bg = when_all(bgv.begin(), bgv.end()).then([bg_start, bg_reqs_per_iter, &total_bg_iops] (auto) {
                    auto elapsed = duration_cast<std::chrono::duration<double>>(steady_clock::now() - bg_start);
                    double iops = bg_reqs_per_iter / elapsed.count();
                    total_bg_iops += iops;
                    testlog.info("BG IOPS {:.0f}", iops);
                    return make_ready_future<std::tuple<>>();
                });
                auto start = steady_clock::now();
                auto fg = queues[0]->enqueue(fg_sg, fg_concurrency, fg_req_size).then([start, &total_delay] {
                    auto delay = steady_clock::now() - start;
                    total_delay += delay;
                    testlog.info("FG delay {:.3f} ms", duration_cast<std::chrono::duration<double, std::milli>>(delay).count());
                    return make_ready_future<>();
                });
                while (!bg.available() || !fg.available()) {
                    for (unsigned i = 1; i < nr_queues; i++) {
                        queues[i]->poll_once();
                    }
                    // Poll 0th queue last to let all other queues grab tokens from
                    // shared bucket for their front requests
                    queues[0]->poll_once();
                    seastar::sleep(poll_interval).get();
                }
            }
            testlog.info("Average FG delay {:.3f} ms", duration_cast<std::chrono::duration<double, std::milli>>(total_delay).count() / nr_iterations);
            testlog.info("Average BG IOPS {:.0f}", total_bg_iops / nr_iterations);
        });
    });
}

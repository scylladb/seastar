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
 * Copyright (C) 2020 ScyllaDB Ltd.
 */


#include <seastar/testing/perf_tests.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/fair_queue.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/when_all.hh>
#include <boost/range/irange.hpp>

struct local_fq_and_class {
    seastar::fair_queue fq;
    seastar::priority_class_ptr pclass;
    unsigned executed = 0;

    local_fq_and_class(seastar::fair_queue::config cfg)
        : fq(std::move(cfg))
        , pclass(fq.register_priority_class(1))
    {}

    ~local_fq_and_class() {
        fq.unregister_priority_class(pclass);
    }
};

struct perf_fair_queue {

    static constexpr unsigned requests_to_dispatch = 1000;

    seastar::fair_queue::config cfg;
    seastar::sharded<local_fq_and_class> local_fq;

    seastar::fair_queue shared_fq;
    std::vector<priority_class_ptr> shared_pclass;

    uint64_t shared_executed = 0;
    uint64_t shared_acked = 0;

    perf_fair_queue()
        : cfg({ std::chrono::milliseconds(100), 1, 1 })
        , shared_fq(cfg)
    {
        local_fq.start(cfg).get();
        for (unsigned i = 0; i < smp::count; ++i) {
            shared_pclass.push_back(shared_fq.register_priority_class(1));
        }
    }

    ~perf_fair_queue() {
        local_fq.stop().get();
        for (auto& pc : shared_pclass) {
            shared_fq.unregister_priority_class(pc);
        }
    }
};

PERF_TEST_F(perf_fair_queue, contended_local)
{
    auto invokers = local_fq.invoke_on_all([] (local_fq_and_class& local) {
        return parallel_for_each(boost::irange(0u, requests_to_dispatch), [&local] (unsigned dummy) {
            local.fq.queue(local.pclass, seastar::fair_queue_ticket{1, 1}, [&local] {
                local.executed++;
                local.fq.notify_requests_finished(seastar::fair_queue_ticket{1, 1});
            });
            return make_ready_future<>();
        });
    });

    auto collectors = local_fq.invoke_on_all([] (local_fq_and_class& local) {
        // Zeroing this counter must be here, otherwise should the collectors win the
        // execution order in when_all_succeed(), the do_until()'s stopping callback
        // would return true immediately and the queue would not be dispatched.
        //
        // At the same time, although this counter is incremented by the lambda from
        // invokers, it's not called until the fq.dispatch_requests() is, so there's no
        // opposite problem if zeroing it here.
        local.executed = 0;

        return do_until([&local] { return local.executed == requests_to_dispatch; }, [&local] {
            local.fq.dispatch_requests();
            return make_ready_future<>();
        });
    });

    return when_all_succeed(std::move(invokers), std::move(collectors)).discard_result();
}

PERF_TEST_F(perf_fair_queue, contended_shared)
{
    shared_acked = 0;
    shared_executed = 0;
    auto invokers = local_fq.invoke_on_all([this, coordinator = this_shard_id()] (local_fq_and_class& dummy) {
        return parallel_for_each(boost::irange(0u, requests_to_dispatch), [this, coordinator] (unsigned dummy) {
            return smp::submit_to(coordinator, [this] {
                shared_fq.queue(shared_pclass[this_shard_id()], seastar::fair_queue_ticket{1, 1}, [this] {
                    shared_executed++;
                });
                return make_ready_future<>();
            });
        });
    });

    auto collectors = do_until([this] { return shared_acked == requests_to_dispatch * smp::count; }, [this] {
        shared_fq.dispatch_requests();
        uint32_t pending_ack = shared_executed - shared_acked;
        shared_acked = shared_executed;
        shared_fq.notify_requests_finished(seastar::fair_queue_ticket{pending_ack, pending_ack}, pending_ack);
        return make_ready_future<>();
    });
    return when_all_succeed(std::move(invokers), std::move(collectors)).discard_result();
}
PERF_TEST_F(perf_fair_queue, contended_shared_amortized)
{
    shared_acked = 0;
    shared_executed = 0;
    auto invokers = local_fq.invoke_on_all([this, coordinator = this_shard_id()] (local_fq_and_class& dummy) {
        return smp::submit_to(coordinator, [this] {
            return parallel_for_each(boost::irange(0u, requests_to_dispatch), [this] (unsigned dummy) {
                shared_fq.queue(shared_pclass[this_shard_id()], seastar::fair_queue_ticket{1, 1}, [this] {
                    shared_executed++;
                });
                return make_ready_future<>();
            });
        });
    });

    auto collectors = do_until([this] { return shared_acked == requests_to_dispatch * smp::count; }, [this] {
        shared_fq.dispatch_requests();
        uint32_t pending_ack = shared_executed - shared_acked;
        shared_acked = shared_executed;
        shared_fq.notify_requests_finished(seastar::fair_queue_ticket{pending_ack, pending_ack}, pending_ack);
        return make_ready_future<>();
    });
    return when_all_succeed(std::move(invokers), std::move(collectors)).discard_result();
}

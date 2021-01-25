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
    seastar::fair_group fg;
    seastar::fair_queue fq;
    seastar::fair_queue sfq;
    seastar::priority_class_ptr pclass;
    unsigned executed = 0;

    seastar::fair_queue& queue(bool local) noexcept { return local ? fq : sfq; }

    local_fq_and_class(seastar::fair_group& sfg)
        : fg(seastar::fair_group::config(1, 1))
        , fq(fg, seastar::fair_queue::config())
        , sfq(sfg, seastar::fair_queue::config())
        , pclass(fq.register_priority_class(1))
    {}

    ~local_fq_and_class() {
        fq.unregister_priority_class(pclass);
    }
};

struct local_fq_entry {
    seastar::fair_queue_entry ent;
    std::function<void()> submit;

    template <typename Func>
    local_fq_entry(unsigned weight, unsigned index, Func&& f)
        : ent(seastar::fair_queue_ticket(weight, index))
        , submit(std::move(f)) {}
};

struct perf_fair_queue {

    static constexpr unsigned requests_to_dispatch = 1000;

    seastar::sharded<local_fq_and_class> local_fq;

    seastar::fair_group shared_fg;

    perf_fair_queue()
        : shared_fg(seastar::fair_group::config(smp::count, smp::count))
    {
        local_fq.start(std::ref(shared_fg)).get();
    }

    ~perf_fair_queue() {
        local_fq.stop().get();
    }

    future<> test(bool local);
};

future<> perf_fair_queue::test(bool loc) {

    auto invokers = local_fq.invoke_on_all([loc] (local_fq_and_class& local) {
        return parallel_for_each(boost::irange(0u, requests_to_dispatch), [&local, loc] (unsigned dummy) {
            auto req = std::make_unique<local_fq_entry>(1, 1, [&local, loc] {
                local.executed++;
                local.queue(loc).notify_requests_finished(seastar::fair_queue_ticket{1, 1});
            });
            local.queue(loc).queue(local.pclass, req->ent);
            req.release();
            return make_ready_future<>();
        });
    });

    auto collectors = local_fq.invoke_on_all([loc] (local_fq_and_class& local) {
        // Zeroing this counter must be here, otherwise should the collectors win the
        // execution order in when_all_succeed(), the do_until()'s stopping callback
        // would return true immediately and the queue would not be dispatched.
        //
        // At the same time, although this counter is incremented by the lambda from
        // invokers, it's not called until the fq.dispatch_requests() is, so there's no
        // opposite problem if zeroing it here.
        local.executed = 0;

        return do_until([&local] { return local.executed == requests_to_dispatch; }, [&local, loc] {
            local.queue(loc).dispatch_requests([] (fair_queue_entry& ent) {
                local_fq_entry* le = boost::intrusive::get_parent_from_member(&ent, &local_fq_entry::ent);
                le->submit();
                delete le;
            });
            return make_ready_future<>();
        });
    });

    return when_all_succeed(std::move(invokers), std::move(collectors)).discard_result();
}

PERF_TEST_F(perf_fair_queue, contended_local)
{
    return test(true);
}
PERF_TEST_F(perf_fair_queue, contended_shared)
{
    return test(false);
}

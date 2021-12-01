// -*- mode:C++; tab-width:4; c-basic-offset:4; indent-tabs-mode:nil -*-
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
 * Copyright (C) 2018 Red Hat
 */

#pragma once

#include <atomic>
#include <deque>
#include <future>
#include <memory>

#include <boost/lockfree/queue.hpp>

#include <seastar/core/future.hh>
#include <seastar/core/cacheline.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/metrics_registration.hh>

/// \file

namespace seastar {

class reactor;

/// \brief Integration with non-seastar applications.
namespace alien {

class message_queue {
    static constexpr size_t batch_size = 128;
    static constexpr size_t prefetch_cnt = 2;
    struct work_item;
    struct lf_queue_remote {
        reactor* remote;
    };
    using lf_queue_base = boost::lockfree::queue<work_item*>;
    // use inheritence to control placement order
    struct lf_queue : lf_queue_remote, lf_queue_base {
        lf_queue(reactor* remote)
            : lf_queue_remote{remote}, lf_queue_base{batch_size} {}
        void maybe_wakeup();
    } _pending;
    struct alignas(seastar::cache_line_size) {
        std::atomic<size_t> value{0};
    } _sent;
    // keep this between two structures with statistics
    // this makes sure that they have at least one cache line
    // between them, so hw prefetcher will not accidentally prefetch
    // cache line used by another cpu.
    metrics::metric_groups _metrics;
    struct alignas(seastar::cache_line_size) {
        size_t _received = 0;
        size_t _last_rcv_batch = 0;
    };
    struct work_item {
        virtual ~work_item() = default;
        virtual void process() = 0;
    };
    template <typename  Func>
    struct async_work_item : work_item {
        Func _func;
        async_work_item(Func&& func) : _func(std::move(func)) {}
        void process() override {
            _func();
        }
    };
    template<typename Func>
    size_t process_queue(lf_queue& q, Func process);
    void submit_item(std::unique_ptr<work_item> wi);
public:
    message_queue(reactor *to);
    void start();
    void stop();
    template <typename Func>
    void submit(Func&& func) {
        auto wi = std::make_unique<async_work_item<Func>>(std::forward<Func>(func));
        submit_item(std::move(wi));
    }
    size_t process_incoming();
    bool pure_poll_rx() const;
};

namespace internal {

struct qs_deleter {
    unsigned count;
    qs_deleter(unsigned n = 0) : count(n) {}
    qs_deleter(const qs_deleter& d) : count(d.count) {}
    void operator()(message_queue* qs) const;
};

}

/// Represents the Seastar system from alien's point of view. In a normal
/// system, there is just one instance, but for in-process clustering testing
/// there may be more than one. Function such as run_on() direct messages to
/// and (instance, shard) tuple.
class instance {
    using qs = std::unique_ptr<message_queue[], internal::qs_deleter>;
public:
    static qs create_qs(const std::vector<reactor*>& reactors);
    qs _qs;
    bool poll_queues();
    bool pure_poll_queues();
};

namespace internal {

extern instance* default_instance;

}

/// Runs a function on a remote shard from an alien thread where engine() is not available.
///
/// \param instance designates the Seastar instance to process the message
/// \param shard designates the shard to run the function on
/// \param func a callable to run on shard \c t.  If \c func is a temporary object,
///          its lifetime will be extended by moving it.  If \c func is a reference,
///          the caller must guarantee that it will survive the call.
/// \note the func must not throw and should return \c void. as we cannot identify the
///          alien thread, hence we are not able to post the fulfilled promise to the
///          message queue managed by the shard executing the alien thread which is
///          interested to the return value. Please use \c submit_to() instead, if
///          \c func throws.
template <typename Func>
void run_on(instance& instance, unsigned shard, Func func) {
    instance._qs[shard].submit(std::move(func));
}

/// Runs a function on a remote shard from an alien thread where engine() is not available.
///
/// \param shard designates the shard to run the function on
/// \param func a callable to run on shard \c t.  If \c func is a temporary object,
///          its lifetime will be extended by moving it.  If \c func is a reference,
///          the caller must guarantee that it will survive the call.
/// \note the func must not throw and should return \c void. as we cannot identify the
///          alien thread, hence we are not able to post the fulfilled promise to the
///          message queue managed by the shard executing the alien thread which is
///          interested to the return value. Please use \c submit_to() instead, if
///          \c func throws.
template <typename Func>
[[deprecated("Use run_on(instance&, unsigned shard, Func) instead")]]
void run_on(unsigned shard, Func func) {
    run_on(*internal::default_instance, shard, std::move(func));
}

namespace internal {
template<typename Func>
using return_value_t = typename futurize<std::invoke_result_t<Func>>::value_type;

template<typename Func,
         bool = std::is_empty_v<return_value_t<Func>>>
struct return_type_of {
    using type = void;
    static void set(std::promise<void>& p, return_value_t<Func>&&) {
        p.set_value();
    }
};
template<typename Func>
struct return_type_of<Func, false> {
    using return_tuple_t = typename futurize<std::invoke_result_t<Func>>::tuple_type;
    using type = std::tuple_element_t<0, return_tuple_t>;
    static void set(std::promise<type>& p, return_value_t<Func>&& t) {
#if SEASTAR_API_LEVEL < 5
        p.set_value(std::get<0>(std::move(t)));
#else
        p.set_value(std::move(t));
#endif
    }
};
template <typename Func> using return_type_t = typename return_type_of<Func>::type;
}

/// Runs a function on a remote shard from an alien thread where engine() is not available.
///
/// \param instance designates the Seastar instance to process the message
/// \param shard designates the shard to run the function on
/// \param func a callable to run on \c shard.  If \c func is a temporary object,
///          its lifetime will be extended by moving it.  If \c func is a reference,
///          the caller must guarantee that it will survive the call.
/// \return whatever \c func returns, as a \c std::future<>
/// \note the caller must keep the returned future alive until \c func returns
template<typename Func, typename T = internal::return_type_t<Func>>
std::future<T> submit_to(instance& instance, unsigned shard, Func func) {
    std::promise<T> pr;
    auto fut = pr.get_future();
    run_on(instance, shard, [pr = std::move(pr), func = std::move(func)] () mutable {
        // std::future returned via std::promise above.
        (void)func().then_wrapped([pr = std::move(pr)] (auto&& result) mutable {
            try {
                internal::return_type_of<Func>::set(pr, result.get());
            } catch (...) {
                pr.set_exception(std::current_exception());
            }
        });
    });
    return fut;
}

/// Runs a function on a remote shard from an alien thread where engine() is not available.
///
/// \param shard designates the shard to run the function on
/// \param func a callable to run on \c shard.  If \c func is a temporary object,
///          its lifetime will be extended by moving it.  If \c func is a reference,
///          the caller must guarantee that it will survive the call.
/// \return whatever \c func returns, as a \c std::future<>
/// \note the caller must keep the returned future alive until \c func returns
template<typename Func, typename T = internal::return_type_t<Func>>
[[deprecated("Use submit_to(instance&, unsigned shard, Func) instead.")]]
std::future<T> submit_to(unsigned shard, Func func) {
    return submit_to(*internal::default_instance, shard, std::move(func));
}

}
}

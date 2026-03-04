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
 * Copyright 2019 ScyllaDB
 */

#pragma once

#include <seastar/core/internal/pollable_fd.hh>
#include <seastar/core/future.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/noncopyable_function.hh>
#include <boost/lockfree/spsc_queue.hpp>

namespace seastar {

class syscall_work_queue {
    static constexpr size_t queue_length = 128;
    struct work_item;
    using lf_queue = boost::lockfree::spsc_queue<work_item*,
                            boost::lockfree::capacity<queue_length>>;
    lf_queue _pending;
    lf_queue _completed;
    writeable_eventfd _start_eventfd;
    semaphore _queue_has_room = { queue_length };
    struct work_item {
        virtual ~work_item() {}
        virtual void process() = 0;
        virtual void complete() = 0;
        virtual void set_exception(std::exception_ptr) = 0;
    };
    template <typename T>
    struct work_item_returning :  work_item {
        noncopyable_function<T ()> _func;
        promise<T> _promise;
        std::optional<T> _result;
        work_item_returning(noncopyable_function<T ()> func) : _func(std::move(func)) {}
        virtual void process() override { _result = this->_func(); }
        virtual void complete() override { _promise.set_value(std::move(*_result)); }
        virtual void set_exception(std::exception_ptr eptr) override { _promise.set_exception(eptr); };
        future<T> get_future() { return _promise.get_future(); }
    };
public:
    syscall_work_queue();
    template <typename T>
    future<T> submit(noncopyable_function<T ()> func) noexcept {
      try {
        auto wi = std::make_unique<work_item_returning<T>>(std::move(func));
        auto fut = wi->get_future();
        submit_item(std::move(wi));
        return fut;
      } catch (...) {
        return current_exception_as_future<T>();
      }
    }
private:
    void work();
    // Scans the _completed queue, that contains the requests already handled by the syscall thread,
    // effectively opening up space for more requests to be submitted. One consequence of this is
    // that from the reactor's point of view, a request is not considered handled until it is
    // removed from the _completed queue.
    //
    // Returns the number of requests handled.
    unsigned complete();
    void submit_item(std::unique_ptr<syscall_work_queue::work_item> wi);

    friend class thread_pool;
};

}

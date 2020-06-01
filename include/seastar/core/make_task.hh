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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include <memory>
#include <seastar/core/task.hh>
#include <seastar/core/future.hh>

namespace seastar {

template <typename Func>
class lambda_task final : public task {
    Func _func;
    using futurator = futurize<std::result_of_t<Func()>>;
    typename futurator::promise_type _result;
public:
    lambda_task(scheduling_group sg, const Func& func) : task(sg), _func(func) {}
    lambda_task(scheduling_group sg, Func&& func) : task(sg), _func(std::move(func)) {}
    typename futurator::type get_future() { return _result.get_future(); }
    virtual void run_and_dispose() noexcept override {
        futurator::invoke(_func).forward_to(std::move(_result));
        delete this;
    }
    virtual task* waiting_task() noexcept override {
        return _result.waiting_task();
    }
};

template <typename Func>
inline
lambda_task<Func>*
make_task(Func&& func) noexcept {
    return new lambda_task<Func>(current_scheduling_group(), std::forward<Func>(func));
}

template <typename Func>
inline
lambda_task<Func>*
make_task(scheduling_group sg, Func&& func) noexcept {
    return new lambda_task<Func>(sg, std::forward<Func>(func));
}

}

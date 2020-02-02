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
#include <seastar/core/scheduling.hh>

namespace seastar {

class task {
    scheduling_group _sg;
protected:
    // Task destruction is performed by run_and_dispose() via a concrete type,
    // so no need for a virtual destructor here. Derived classes that implement
    // run_and_dispose() should be declared final to avoid losing concrete type
    // information via inheritance.
    ~task() = default;
public:
    explicit task(scheduling_group sg = current_scheduling_group()) : _sg(sg) {}
    virtual void run_and_dispose() noexcept = 0;
    scheduling_group group() const { return _sg; }
};

void schedule(task* t) noexcept;
void schedule_urgent(task* t) noexcept;

template <typename Func>
class lambda_task final : public task {
    Func _func;
public:
    lambda_task(scheduling_group sg, const Func& func) : task(sg), _func(func) {}
    lambda_task(scheduling_group sg, Func&& func) : task(sg), _func(std::move(func)) {}
    virtual void run_and_dispose() noexcept override {
        _func();
        delete this;
    }
};

template <typename Func>
inline
task*
make_task(Func&& func) noexcept {
    return new lambda_task<Func>(current_scheduling_group(), std::forward<Func>(func));
}

template <typename Func>
inline
task*
make_task(scheduling_group sg, Func&& func) noexcept {
    return new lambda_task<Func>(sg, std::forward<Func>(func));
}

}

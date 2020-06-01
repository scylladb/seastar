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
 * Copyright (C) 2018 ScyllaDB Ltd.
 */

#include <seastar/core/execution_stage.hh>
#include <seastar/core/print.hh>
#include <seastar/core/make_task.hh>

namespace seastar {

namespace internal {

void execution_stage_manager::register_execution_stage(execution_stage& stage) {
    auto ret = _stages_by_name.emplace(stage.name(), &stage);
    if (!ret.second) {
        throw std::invalid_argument(format("Execution stage {} already exists.", stage.name()));
    }
    try {
        _execution_stages.push_back(&stage);
    } catch (...) {
        _stages_by_name.erase(stage.name());
        throw;
    }
}

void execution_stage_manager::unregister_execution_stage(execution_stage& stage) noexcept {
    auto it = std::find(_execution_stages.begin(), _execution_stages.end(), &stage);
    if (it == _execution_stages.end()) {
        return; // was changed by update_execution_stage_registration
    }
    _execution_stages.erase(it);
    _stages_by_name.erase(stage.name());
}

void execution_stage_manager::update_execution_stage_registration(execution_stage& old_es, execution_stage& new_es) noexcept {
    auto it = std::find(_execution_stages.begin(), _execution_stages.end(), &old_es);
    *it = &new_es;
    _stages_by_name.find(new_es.name())->second = &new_es;
}

execution_stage* execution_stage_manager::get_stage(const sstring& name) {
    return _stages_by_name[name];
}

bool execution_stage_manager::flush() noexcept {
    bool did_work = false;
    for (auto&& stage : _execution_stages) {
        did_work |= stage->flush();
    }
    return did_work;
}

bool execution_stage_manager::poll() const noexcept {
    for (auto&& stage : _execution_stages) {
        if (stage->poll()) {
            return true;
        }
    }
    return false;
}

execution_stage_manager& execution_stage_manager::get() noexcept {
    static thread_local execution_stage_manager instance;
    return instance;
}

}

execution_stage::~execution_stage()
{
    internal::execution_stage_manager::get().unregister_execution_stage(*this);
}

execution_stage::execution_stage(execution_stage&& other)
    : _sg(other._sg)
    , _stats(other._stats)
    , _name(std::move(other._name))
    , _metric_group(std::move(other._metric_group))
{
    internal::execution_stage_manager::get().update_execution_stage_registration(other, *this);
}

execution_stage::execution_stage(const sstring& name, scheduling_group sg)
    : _sg(sg)
    , _name(name)
{
    internal::execution_stage_manager::get().register_execution_stage(*this);
    auto undo = defer([&] { internal::execution_stage_manager::get().unregister_execution_stage(*this); });
    _metric_group = metrics::metric_group("execution_stages", {
             metrics::make_derive("tasks_scheduled",
                                  metrics::description("Counts tasks scheduled by execution stages"),
                                  { metrics::label_instance("execution_stage", name), },
                                  [name, &esm = internal::execution_stage_manager::get()] {
                                      return esm.get_stage(name)->get_stats().tasks_scheduled;
                                  }),
             metrics::make_derive("tasks_preempted",
                                  metrics::description("Counts tasks which were preempted before execution all queued operations"),
                                  { metrics::label_instance("execution_stage", name), },
                                  [name, &esm = internal::execution_stage_manager::get()] {
                                      return esm.get_stage(name)->get_stats().tasks_preempted;
                                  }),
             metrics::make_derive("function_calls_enqueued",
                                  metrics::description("Counts function calls added to execution stages queues"),
                                  { metrics::label_instance("execution_stage", name), },
                                  [name, &esm = internal::execution_stage_manager::get()] {
                                      return esm.get_stage(name)->get_stats().function_calls_enqueued;
                                  }),
             metrics::make_derive("function_calls_executed",
                                  metrics::description("Counts function calls executed by execution stages"),
                                  { metrics::label_instance("execution_stage", name), },
                                  [name, &esm = internal::execution_stage_manager::get()] {
                                      return esm.get_stage(name)->get_stats().function_calls_executed;
                                  }),
           });
    undo.cancel();
}

bool execution_stage::flush() noexcept {
    if (_empty || _flush_scheduled) {
        return false;
    }
    _stats.tasks_scheduled++;
    schedule(make_task(_sg, [this] {
        do_flush();
        _flush_scheduled = false;
    }));
    _flush_scheduled = true;
    return true;
};

}

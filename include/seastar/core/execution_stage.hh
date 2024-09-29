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
 * Copyright (C) 2017 ScyllaDB Ltd.
 */

#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/chunked_fifo.hh>
#include <seastar/core/function_traits.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/scheduling.hh>
#include <seastar/util/reference_wrapper.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/tuple_utils.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#ifndef SEASTAR_MODULE
#include <fmt/format.h>
#include <vector>
#include <boost/container/static_vector.hpp>
#endif

namespace seastar {

/// \defgroup execution-stages Execution Stages
///
/// \brief
/// Execution stages provide an infrastructure for processing function calls in
/// batches in order to improve instruction cache locality.
///
/// When the application logic becomes more and more complex and the length
/// of the data processing pipeline grows it may happen that the most
/// significant bottleneck are instruction cache misses. The solution for that
/// problem may be processing similar operations in batches so that instruction
/// cache locality is improved at the cost of potentially higher latencies and
/// worse data cache locality.
///
/// Execution stages allow batching calls to the specified function object.
/// Every time concrete_execution_stage::operator()() is used the function call
/// is added to the queue and a future is returned. Once the number of queued
/// calls reaches certain threshold the stage is flushed and a task is which
/// would execute these function calls is scheduled. Execution stages are also
/// flushed when the reactor polls for events.
///
/// When calling a function that is wrapped inside execution stage it is
/// important to remember that the actual function call will happen at some
/// later time and it has to be guaranteed the objects passed by lvalue
/// reference are still alive. In order to avoid accidental passing of a
/// temporary object by lvalue reference the interface of execution stages
/// accepts only lvalue references wrapped in reference_wrapper. It is safe to
/// pass rvalue references, they are decayed and the objects are moved. See
/// concrete_execution_stage::operator()() for more details.

/// \addtogroup execution-stages
/// @{

/// \cond internal
namespace internal {

// Execution wraps lreferences in reference_wrapper so that the caller is forced
// to use seastar::ref(). Then when the function is actually called the
// reference is unwrapped. However, we need to distinguish between functions
// which argument is lvalue reference and functions that take
// reference_wrapper<> as an argument and not unwrap the latter. To solve this
// issue reference_wrapper_for_es type is used for wrappings done automatically
// by execution stage.
template<typename T>
struct reference_wrapper_for_es : reference_wrapper<T> {
    reference_wrapper_for_es(reference_wrapper <T> rw) noexcept
        : reference_wrapper<T>(std::move(rw)) {}
};

template<typename T>
struct wrap_for_es {
    using type = T;
};

template<typename T>
struct wrap_for_es<T&> {
    using type = reference_wrapper_for_es<T>;
};

template<typename T>
struct wrap_for_es<T&&> {
    using type = T;
};

template<typename T>
decltype(auto) unwrap_for_es(T&& object) {
    return std::forward<T>(object);
}

template<typename T>
std::reference_wrapper<T> unwrap_for_es(reference_wrapper_for_es<T> ref) {
    return std::reference_wrapper<T>(ref.get());
}

}
/// \endcond

/// Base execution stage class
SEASTAR_MODULE_EXPORT
class execution_stage {
public:
    struct stats {
        uint64_t tasks_scheduled = 0;
        uint64_t tasks_preempted = 0;
        uint64_t function_calls_enqueued = 0;
        uint64_t function_calls_executed = 0;
    };
protected:
    bool _empty = true;
    bool _flush_scheduled = false;
    scheduling_group _sg;
    stats _stats;
    sstring _name;
    metrics::metric_group _metric_group;
protected:
    virtual void do_flush() noexcept = 0;
public:
    explicit execution_stage(const sstring& name, scheduling_group sg = {});
    virtual ~execution_stage();

    execution_stage(const execution_stage&) = delete;

    /// Move constructor
    ///
    /// \warning It is illegal to move execution_stage after any operation has
    /// been pushed to it. The only reason why the move constructor is not
    /// deleted is the fact that C++14 does not guarantee return value
    /// optimisation which is required by make_execution_stage().
    execution_stage(execution_stage&&);

    /// Returns execution stage name
    const sstring& name() const noexcept { return _name; }

    /// Returns execution stage usage statistics
    const stats& get_stats() const noexcept { return _stats; }

    /// Flushes execution stage
    ///
    /// Ensures that a task which would execute all queued operations is
    /// scheduled. Does not schedule a new task if there is one already pending
    /// or the queue is empty.
    ///
    /// \return true if a new task has been scheduled
    bool flush() noexcept;

    /// Checks whether there are pending operations.
    ///
    /// \return true if there is at least one queued operation
    bool poll() const noexcept {
        return !_empty;
    }
};

/// \cond internal
namespace internal {

class execution_stage_manager {
    std::vector<execution_stage*> _execution_stages;
    std::unordered_map<sstring, execution_stage*> _stages_by_name;
private:
    execution_stage_manager() = default;
    execution_stage_manager(const execution_stage_manager&) = delete;
    execution_stage_manager(execution_stage_manager&&) = delete;
public:
    void register_execution_stage(execution_stage& stage);
    void unregister_execution_stage(execution_stage& stage) noexcept;
    void update_execution_stage_registration(execution_stage& old_es, execution_stage& new_es) noexcept;
    execution_stage* get_stage(const sstring& name);
    bool flush() noexcept;
    bool poll() const noexcept;
public:
    static execution_stage_manager& get() noexcept;
};

}
/// \endcond

/// \brief Concrete execution stage class
///
/// \note The recommended way of creating execution stages is to use
/// make_execution_stage().
///
/// \tparam ReturnType return type of the function object
/// \tparam Args  argument pack containing arguments to the function object, needs
///                   to have move constructor that doesn't throw
template<typename ReturnType, typename... Args>
requires std::is_nothrow_move_constructible_v<std::tuple<Args...>>
class concrete_execution_stage final : public execution_stage {
    using args_tuple = std::tuple<Args...>;
    static_assert(std::is_nothrow_move_constructible_v<args_tuple>,
                  "Function arguments need to be nothrow move constructible");

    static constexpr size_t flush_threshold = 128;
    static constexpr size_t max_queue_length = 1024;

    using return_type = futurize_t<ReturnType>;
    using promise_type = typename return_type::promise_type;
    using input_type = typename tuple_map_types<internal::wrap_for_es, args_tuple>::type;

    struct work_item {
        input_type _in;
        promise_type _ready;

        work_item(typename internal::wrap_for_es<Args>::type... args) : _in(std::move(args)...) { }

        work_item(work_item&& other) = delete;
        work_item(const work_item&) = delete;
        work_item(work_item&) = delete;
    };
    chunked_fifo<work_item, flush_threshold> _queue;

    noncopyable_function<ReturnType (Args...)> _function;
private:
    auto unwrap(input_type&& in) {
        return tuple_map(std::move(in), [] (auto&& obj) {
            return internal::unwrap_for_es(std::forward<decltype(obj)>(obj));
        });
    }

    virtual void do_flush() noexcept override {
        while (!_queue.empty()) {
            auto& wi = _queue.front();
            auto wi_in = std::move(wi._in);
            auto wi_ready = std::move(wi._ready);
            _queue.pop_front();
            futurize<ReturnType>::apply(_function, unwrap(std::move(wi_in))).forward_to(std::move(wi_ready));
            _stats.function_calls_executed++;

            if (internal::scheduler_need_preempt()) {
                _stats.tasks_preempted++;
                break;
            }
        }
        _empty = _queue.empty();
    }
public:
    explicit concrete_execution_stage(const sstring& name, scheduling_group sg, noncopyable_function<ReturnType (Args...)> f)
        : execution_stage(name, sg)
        , _function(std::move(f))
    {
        _queue.reserve(flush_threshold);
    }
    explicit concrete_execution_stage(const sstring& name, noncopyable_function<ReturnType (Args...)> f)
        : concrete_execution_stage(name, scheduling_group(), std::move(f)) {
    }

    /// Enqueues a call to the stage's function
    ///
    /// Adds a function call to the queue. Objects passed by value are moved,
    /// rvalue references are decayed and the objects are moved, lvalue
    /// references need to be explicitly wrapped using seastar::ref().
    ///
    /// Usage example:
    /// ```
    /// void do_something(int&, int, std::vector<int>&&);
    /// thread_local auto stage = seastar::make_execution_stage("execution-stage", do_something);
    ///
    /// int global_value;
    ///
    /// future<> func(std::vector<int> vec) {
    ///     //return stage(global_value, 42, std::move(vec)); // fail: use seastar::ref to pass references
    ///     return stage(seastar::ref(global_value), 42, std::move(vec)); // ok
    /// }
    /// ```
    ///
    /// \param args arguments passed to the stage's function
    /// \return future containing the result of the call to the stage's function
    return_type operator()(typename internal::wrap_for_es<Args>::type... args) {
        if (_queue.size() >= max_queue_length) {
            do_flush();
        }
        _queue.emplace_back(std::move(args)...);
        _empty = false;
        _stats.function_calls_enqueued++;
        auto f = _queue.back()._ready.get_future();
        flush();
        return f;
    }
};

/// \brief Base class for execution stages with support for automatic \ref scheduling_group inheritance
class inheriting_execution_stage {
public:
    struct per_scheduling_group_stats {
        scheduling_group sg;
        execution_stage::stats stats;
    };
    using stats = boost::container::static_vector<per_scheduling_group_stats, max_scheduling_groups()>;
};

/// \brief Concrete execution stage class, with support for automatic \ref scheduling_group inheritance
///
/// A variation of \ref concrete_execution_stage that inherits the \ref scheduling_group
/// from the caller. Each call (of `operator()`) can be in its own scheduling group.
///
/// \tparam ReturnType return type of the function object
/// \tparam Args  argument pack containing arguments to the function object, needs
///                   to have move constructor that doesn't throw
template<typename ReturnType, typename... Args>
requires std::is_nothrow_move_constructible_v<std::tuple<Args...>>
class inheriting_concrete_execution_stage final : public inheriting_execution_stage {
    using return_type = futurize_t<ReturnType>;
    using args_tuple = std::tuple<Args...>;
    using per_group_stage_type = concrete_execution_stage<ReturnType, Args...>;

    static_assert(std::is_nothrow_move_constructible_v<args_tuple>,
                  "Function arguments need to be nothrow move constructible");

    sstring _name;
    noncopyable_function<ReturnType (Args...)> _function;
    std::vector<std::optional<per_group_stage_type>> _stage_for_group{max_scheduling_groups()};
private:
    per_group_stage_type make_stage_for_group(scheduling_group sg) {
        // We can't use std::ref(function), because reference_wrapper decays to noncopyable_function& and
        // that selects the noncopyable_function copy constructor. Use a lambda instead.
        auto wrapped_function = [&_function = _function] (Args... args) {
            return _function(std::forward<Args>(args)...);
        };
        auto name = fmt::format("{}.{}", _name, sg.name());
        return per_group_stage_type(name, sg, wrapped_function);
    }
public:
    /// Construct an inheriting concrete execution stage.
    ///
    /// \param name A name for the execution stage; must be unique
    /// \param f Function to be called in response to operator(). The function
    ///        call will be deferred and batched with similar calls to increase
    ///        instruction cache hit rate.
    inheriting_concrete_execution_stage(const sstring& name, noncopyable_function<ReturnType (Args...)> f)
        : _name(std::move(name)),_function(std::move(f)) {
    }

    /// Enqueues a call to the stage's function
    ///
    /// Adds a function call to the queue. Objects passed by value are moved,
    /// rvalue references are decayed and the objects are moved, lvalue
    /// references need to be explicitly wrapped using seastar::ref().
    ///
    /// The caller's \ref scheduling_group will be preserved across the call.
    ///
    /// Usage example:
    /// ```
    /// void do_something(int);
    /// thread_local auto stage = seastar::inheriting_concrete_execution_stage<int>("execution-stage", do_something);
    ///
    /// future<> func(int x) {
    ///     return stage(x);
    /// }
    /// ```
    ///
    /// \param args arguments passed to the stage's function
    /// \return future containing the result of the call to the stage's function
    return_type operator()(typename internal::wrap_for_es<Args>::type... args) {
        auto sg = current_scheduling_group();
        auto sg_id = internal::scheduling_group_index(sg);
        auto& slot = _stage_for_group[sg_id];
        if (!slot) {
            slot.emplace(make_stage_for_group(sg));
        }
        return (*slot)(std::move(args)...);
    }

    /// Returns summary of individual execution stage usage statistics
    ///
    /// \returns a vector of the stats of the individual per-scheduling group
    ///     executation stages. Each element in the vector is a pair composed of
    ///     the scheduling group and the stats for the respective execution
    ///     stage. Scheduling groups that have had no respective calls enqueued
    ///     yet are omitted.
    inheriting_execution_stage::stats get_stats() const noexcept {
        inheriting_execution_stage::stats summary;
        for (unsigned sg_id = 0; sg_id != _stage_for_group.size(); ++sg_id) {
            auto sg = internal::scheduling_group_from_index(sg_id);
            if (_stage_for_group[sg_id]) {
                summary.push_back({sg, _stage_for_group[sg_id]->get_stats()});
            }
        }
        return summary;
    }
};


/// \cond internal
namespace internal {

template <typename Ret, typename ArgsTuple>
struct concrete_execution_stage_helper;

template <typename Ret, typename... Args>
struct concrete_execution_stage_helper<Ret, std::tuple<Args...>> {
    using type = concrete_execution_stage<Ret, Args...>;
};

}
/// \endcond

/// Creates a new execution stage
///
/// Wraps given function object in a concrete_execution_stage. All arguments
/// of the function object are required to have move constructors that do not
/// throw. Function object may return a future or an immediate object or void.
///
/// Moving execution stages is discouraged and illegal after first function
/// call is enqueued.
///
/// Usage example:
/// ```
/// double do_something(int);
/// thread_local auto stage1 = seastar::make_execution_stage("execution-stage1", do_something);
///
/// future<double> func1(int val) {
///     return stage1(val);
/// }
///
/// future<double> do_some_io(int);
/// thread_local auto stage2 = seastar::make_execution_stage("execution-stage2", do_some_io);
///
/// future<double> func2(int val) {
///     return stage2(val);
/// }
/// ```
///
/// \param name unique name of the execution stage
/// \param sg scheduling group to run under
/// \param fn function to be executed by the stage
/// \return concrete_execution_stage
///
SEASTAR_MODULE_EXPORT
template<typename Function>
auto make_execution_stage(const sstring& name, scheduling_group sg, Function&& fn) {
    using traits = function_traits<Function>;
    using ret_type = typename traits::return_type;
    using args_as_tuple = typename traits::args_as_tuple;
    using concrete_execution_stage = typename internal::concrete_execution_stage_helper<ret_type, args_as_tuple>::type;
    return concrete_execution_stage(name, sg, std::forward<Function>(fn));
}

/// Creates a new execution stage (variant taking \ref scheduling_group)
///
/// Wraps given function object in a concrete_execution_stage. All arguments
/// of the function object are required to have move constructors that do not
/// throw. Function object may return a future or an immediate object or void.
///
/// Moving execution stages is discouraged and illegal after first function
/// call is enqueued.
///
/// Usage example:
/// ```
/// double do_something(int);
/// thread_local auto stage1 = seastar::make_execution_stage("execution-stage1", do_something);
///
/// future<double> func1(int val) {
///     return stage1(val);
/// }
///
/// future<double> do_some_io(int);
/// thread_local auto stage2 = seastar::make_execution_stage("execution-stage2", do_some_io);
///
/// future<double> func2(int val) {
///     return stage2(val);
/// }
/// ```
///
/// \param name unique name of the execution stage (variant not taking \ref scheduling_group)
/// \param fn function to be executed by the stage
/// \return concrete_execution_stage
///
SEASTAR_MODULE_EXPORT
template<typename Function>
auto make_execution_stage(const sstring& name, Function&& fn) {
    return make_execution_stage(name, scheduling_group(), std::forward<Function>(fn));
}

/// Creates a new execution stage from a member function
///
/// Wraps a pointer to member function in a concrete_execution_stage. When
/// a function call is pushed to the stage the first argument should be a
/// pointer to the object the function is a member of.
///
/// Usage example:
/// ```
/// struct foo {
///     void do_something(int);
/// };
///
/// thread_local auto stage = seastar::make_execution_stage("execution-stage", &foo::do_something);
///
/// future<> func(foo& obj, int val) {
///     return stage(&obj, val);
/// }
/// ```
///
/// \see make_execution_stage(const sstring&, Function&&)
/// \param name unique name of the execution stage
/// \param fn member function to be executed by the stage
/// \return concrete_execution_stage
SEASTAR_MODULE_EXPORT
template<typename Ret, typename Object, typename... Args>
concrete_execution_stage<Ret, Object*, Args...>
make_execution_stage(const sstring& name, scheduling_group sg, Ret (Object::*fn)(Args...)) {
    return concrete_execution_stage<Ret, Object*, Args...>(name, sg, std::mem_fn(fn));
}

template<typename Ret, typename Object, typename... Args>
concrete_execution_stage<Ret, const Object*, Args...>
make_execution_stage(const sstring& name, scheduling_group sg, Ret (Object::*fn)(Args...) const) {
    return concrete_execution_stage<Ret, const Object*, Args...>(name, sg, std::mem_fn(fn));
}

template<typename Ret, typename Object, typename... Args>
concrete_execution_stage<Ret, Object*, Args...>
make_execution_stage(const sstring& name, Ret (Object::*fn)(Args...)) {
    return make_execution_stage(name, scheduling_group(), fn);
}

template<typename Ret, typename Object, typename... Args>
concrete_execution_stage<Ret, const Object*, Args...>
make_execution_stage(const sstring& name, Ret (Object::*fn)(Args...) const) {
    return make_execution_stage(name, scheduling_group(), fn);
}

/// @}

}

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

#include "future.hh"
#include "chunked_fifo.hh"
#include "function_traits.hh"
#include "util/reference_wrapper.hh"
#include "util/gcc6-concepts.hh"

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
class execution_stage {
protected:
    bool _empty = true;
    bool _flush_scheduled = false;
protected:
    virtual void do_flush() noexcept = 0;
public:
    execution_stage();
    virtual ~execution_stage();

    execution_stage(const execution_stage&) = delete;

    /// Move constructor
    ///
    /// \warning It is illegal to move execution_stage after any operation has
    /// been pushed to it. The only reason why the move constructor is not
    /// deleted is the fact that C++14 does not guarantee return value
    /// optimisation which is required by make_execution_stage().
    execution_stage(execution_stage&&);

    /// Flushes execution stage
    ///
    /// Ensures that a task which would execute all queued operations is
    /// scheduled. Does not schedule a new task if there is one already pending
    /// or the queue is empty.
    ///
    /// \return true if a new task has been scheduled
    bool flush() noexcept {
        if (_empty || _flush_scheduled) {
            return false;
        }
        schedule(make_task([this] {
            do_flush();
            _flush_scheduled = false;
        }));
        _flush_scheduled = true;
        return true;
    };

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
private:
    execution_stage_manager() = default;
    execution_stage_manager(const execution_stage_manager&) = delete;
    execution_stage_manager(execution_stage_manager&&) = delete;
public:
    void register_execution_stage(execution_stage& stage) {
        _execution_stages.push_back(&stage);
    }
    void unregister_execution_stage(execution_stage& stage) noexcept {
        auto it = std::find(_execution_stages.begin(), _execution_stages.end(), &stage);
        _execution_stages.erase(it);
    }
    void update_execution_stage_registration(execution_stage& old_es, execution_stage& new_es) noexcept {
        auto it = std::find(_execution_stages.begin(), _execution_stages.end(), &old_es);
        *it = &new_es;
    }

    bool flush() noexcept {
        bool did_work = false;
        for (auto&& stage : _execution_stages) {
            did_work |= stage->flush();
        }
        return did_work;
    }
    bool poll() const noexcept {
        for (auto&& stage : _execution_stages) {
            if (stage->poll()) {
                return true;
            }
        }
        return false;
    }
public:
    static execution_stage_manager& get() noexcept {
        static thread_local execution_stage_manager instance;
        return instance;
    }
};

}
/// \endcond

/// \brief Concrete execution stage class
///
/// \note The recommended way of creating execution stages is to use
/// make_execution_stage().
///
/// \tparam Function function object to be executed by the stage
/// \tparam ReturnType return type of the function object
/// \tparam ArgsTuple tuple containing arguments to the function object, needs
///                   to have move constructor that doesn't throw
template<typename Function, typename ReturnType, typename ArgsTuple>
GCC6_CONCEPT(requires std::is_nothrow_move_constructible<ArgsTuple>::value)
class concrete_execution_stage final : public execution_stage {
    static_assert(std::is_nothrow_move_constructible<ArgsTuple>::value,
                  "Function arguments need to be nothrow move constructible");

    static constexpr size_t flush_threshold = 128;

    using return_type = futurize_t<ReturnType>;
    using promise_type = typename return_type::promise_type;
    using input_type = typename tuple_map_types<internal::wrap_for_es, ArgsTuple>::type;

    struct work_item {
        input_type _in;
        promise_type _ready;

        template<typename... Args>
        work_item(Args&&... args) : _in(std::forward<Args>(args)...) { }

        work_item(work_item&& other) = delete;
        work_item(const work_item&) = delete;
        work_item(work_item&) = delete;
    };
    chunked_fifo<work_item, flush_threshold> _queue;

    Function _function;
private:
    auto unwrap(input_type&& in) {
        return tuple_map(std::move(in), [] (auto&& obj) {
            return internal::unwrap_for_es(std::forward<decltype(obj)>(obj));
        });
    }

    virtual void do_flush() noexcept override {
        while (!_queue.empty()) {
            auto& wi = _queue.front();
            futurize<ReturnType>::apply(_function, unwrap(std::move(wi._in))).forward_to(std::move(wi._ready));
            _queue.pop_front();

            if (need_preempt()) {
                break;
            }
        }
        _empty = _queue.empty();
    }
public:
    explicit concrete_execution_stage(Function f) : _function(std::move(f)) {
        _queue.reserve(flush_threshold);
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
    /// thread_local auto stage = seastar::make_execution_stage(do_something);
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
    template<typename... Args>
    GCC6_CONCEPT(requires std::is_constructible<input_type, Args...>::value)
    return_type operator()(Args&&... args) {
        _queue.emplace_back(std::forward<Args>(args)...);
        _empty = false;
        auto f = _queue.back()._ready.get_future();
        if (_queue.size() > flush_threshold) {
            flush();
        }
        return f;
    }
};

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
/// thread_local auto stage1 = seastar::make_execution_stage(do_something);
///
/// future<double> func1(int val) {
///     return stage1(val);
/// }
///
/// future<double> do_some_io(int);
/// thread_local auto stage2 = seastar::make_execution_stage(do_some_io);
///
/// future<double> func2(int val) {
///     return stage2(val);
/// }
/// ```
///
/// \param fn function to be executed by the stage
/// \return concrete_execution_stage
template<typename Function>
auto make_execution_stage(Function&& fn) {
    using traits = function_traits<Function>;
    return concrete_execution_stage<Function, typename traits::return_type,
                                    typename traits::args_as_tuple>(std::forward<Function>(fn));
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
/// thread_local auto stage = seastar::make_execution_stage(&foo::do_something);
///
/// future<> func(foo& obj, int val) {
///     return stage(&obj, val);
/// }
/// ```
///
/// \see make_execution_stage(Function&&)
/// \param fn member function to be executed by the stage
/// \return concrete_execution_stage
template<typename Ret, typename Object, typename... Args>
auto make_execution_stage(Ret (Object::*fn)(Args...)) {
    return concrete_execution_stage<decltype(std::mem_fn(fn)), Ret, std::tuple<Object*, Args...>>(std::mem_fn(fn));
}

template<typename Ret, typename Object, typename... Args>
auto make_execution_stage(Ret (Object::*fn)(Args...) const) {
    return concrete_execution_stage<decltype(std::mem_fn(fn)), Ret, std::tuple<const Object*, Args...>>(std::mem_fn(fn));
}

/// @}

inline execution_stage::execution_stage()
{
    internal::execution_stage_manager::get().register_execution_stage(*this);
}

inline execution_stage::~execution_stage()
{
    internal::execution_stage_manager::get().unregister_execution_stage(*this);
}

inline execution_stage::execution_stage(execution_stage&& other)
{
    internal::execution_stage_manager::get().update_execution_stage_registration(other, *this);
}

}

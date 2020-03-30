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

#include <seastar/core/apply.hh>
#include <seastar/core/future.hh>
#include <utility>
#include <memory>
#include <tuple>

namespace seastar {


/// \cond internal

namespace internal {


// Given a future type, find the corresponding continuation_base.
template <typename Future>
struct continuation_base_from_future;

template <typename... T>
struct continuation_base_from_future<future<T...>> {
    using type = continuation_base<T...>;
};

template <typename HeldState, typename Future>
class do_with_state final : public continuation_base_from_future<Future>::type {
    HeldState _held;
    typename Future::promise_type _pr;
public:
    explicit do_with_state(HeldState&& held) : _held(std::move(held)) {}
    virtual void run_and_dispose() noexcept override {
        _pr.set_urgent_state(std::move(this->_state));
        delete this;
    }
    HeldState& data() {
        return _held;
    }
    Future get_future() {
        return _pr.get_future();
    }
};

}
/// \endcond

namespace internal {
template<typename T, typename F>
inline
auto do_with_impl(T&& rvalue, F&& f) {
    auto task = std::make_unique<internal::do_with_state<T, std::result_of_t<F(T&)>>>(std::forward<T>(rvalue));
    auto fut = f(task->data());
    if (fut.available()) {
        return fut;
    }
    auto ret = task->get_future();
    internal::set_callback(fut, task.release());
    return ret;
}
}

/// \addtogroup future-util
/// @{

/// do_with() holds an object alive for the duration until a future
/// completes, and allow the code involved in making the future
/// complete to have easy access to this object.
///
/// do_with() takes two arguments: The first is an temporary object (rvalue),
/// the second is a function returning a future (a so-called "promise").
/// The function is given (a moved copy of) this temporary object, by
/// reference, and it is ensured that the object will not be destructed until
/// the completion of the future returned by the function.
///
/// do_with() returns a future which resolves to whatever value the given future
/// (returned by the given function) resolves to. This returned value must not
/// contain references to the temporary object, as at that point the temporary
/// is destructed.
///
/// \param rvalue a temporary value to protect while \c f is running
/// \param f a callable, accepting an lvalue reference of the same type
///          as \c rvalue, that will be accessible while \c f runs
/// \return whatever \c f returns
template<typename T, typename F>
inline
auto do_with(T&& rvalue, F&& f) noexcept {
    auto func = internal::do_with_impl<T, F>;
    return futurize_invoke(func, std::forward<T>(rvalue), std::forward<F>(f));
}

/// \cond internal
template <typename Tuple, size_t... Idx>
inline
auto
cherry_pick_tuple(std::index_sequence<Idx...>, Tuple&& tuple) {
    return std::make_tuple(std::get<Idx>(std::forward<Tuple>(tuple))...);
}
/// \endcond

/// Executes the function \c func making sure the lock \c lock is taken,
/// and later on properly released.
///
/// \param lock the lock, which is any object having providing a lock() / unlock() semantics.
///        Caller must make sure that it outlives \ref func.
/// \param func function to be executed
/// \returns whatever \c func returns
template<typename Lock, typename Func>
inline
auto with_lock(Lock& lock, Func&& func) {
    return lock.lock().then([func = std::forward<Func>(func)] () mutable {
        return func();
    }).then_wrapped([&lock] (auto&& fut) {
        lock.unlock();
        return std::move(fut);
    });
}

namespace internal {
template <typename T1, typename T2, typename T3_or_F, typename... More>
inline
auto
do_with_impl(T1&& rv1, T2&& rv2, T3_or_F&& rv3, More&&... more) {
    auto all = std::forward_as_tuple(
            std::forward<T1>(rv1),
            std::forward<T2>(rv2),
            std::forward<T3_or_F>(rv3),
            std::forward<More>(more)...);
    constexpr size_t nr = std::tuple_size<decltype(all)>::value - 1;
    using idx = std::make_index_sequence<nr>;
    auto&& just_values = cherry_pick_tuple(idx(), std::move(all));
    auto&& just_func = std::move(std::get<nr>(std::move(all)));
    using value_tuple = std::remove_reference_t<decltype(just_values)>;
    using ret_type = decltype(apply(just_func, just_values));
    auto task = std::make_unique<internal::do_with_state<value_tuple, ret_type>>(std::move(just_values));
    auto fut = apply(just_func, task->data());
    if (fut.available()) {
        return fut;
    }
    auto ret = task->get_future();
    internal::set_callback(fut, task.release());
    return ret;
}
}

/// Multiple argument variant of \ref do_with(T&& rvalue, F&& f).
///
/// This is the same as \ref do_with(T&& tvalue, F&& f), but accepts
/// two or more rvalue parameters, which are held in memory while
/// \c f executes.  \c f will be called with all arguments as
/// reference parameters.
template <typename T1, typename T2, typename T3_or_F, typename... More>
inline
auto
do_with(T1&& rv1, T2&& rv2, T3_or_F&& rv3, More&&... more) noexcept {
    auto func = internal::do_with_impl<T1, T2, T3_or_F, More...>;
    return futurize_invoke(func, std::forward<T1>(rv1), std::forward<T2>(rv2), std::forward<T3_or_F>(rv3), std::forward<More>(more)...);
}

/// @}

}

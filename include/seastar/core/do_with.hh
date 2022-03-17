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

#include <seastar/core/future.hh>
#include <utility>
#include <memory>
#include <tuple>

namespace seastar {


/// \cond internal

namespace internal {

template <typename HeldState, typename Future>
class do_with_state final : public continuation_base_from_future<Future>::type {
    HeldState _held;
    typename Future::promise_type _pr;
public:
    template<typename... T>
    explicit do_with_state(T&&... args) : _held(std::forward<T>(args)...) {}
    virtual void run_and_dispose() noexcept override {
        _pr.set_urgent_state(std::move(this->_state));
        delete this;
    }
    task* waiting_task() noexcept override {
        return _pr.waiting_task();
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
template <typename Tuple, size_t... Idx>
inline
auto
cherry_pick_tuple(std::index_sequence<Idx...>, Tuple&& tuple) {
    return std::forward_as_tuple(std::get<Idx>(std::forward<Tuple>(tuple))...);
}

template <typename Tuple, typename Seq>
struct subtuple;

template <typename Tuple, size_t... Idx>
struct subtuple<Tuple, std::index_sequence<Idx...>> {
    using type = std::tuple<std::decay_t<std::tuple_element_t<Idx, Tuple>>...>;
};

template <typename T1, typename T2, typename... More>
inline
auto
do_with_impl(T1&& rv1, T2&& rv2, More&&... more) {
    auto all = std::forward_as_tuple(
            std::forward<T1>(rv1),
            std::forward<T2>(rv2),
            std::forward<More>(more)...);
    constexpr size_t nr = std::tuple_size<decltype(all)>::value - 1;
    using idx = std::make_index_sequence<nr>;
    auto&& just_values = cherry_pick_tuple(idx(), std::move(all));
    auto&& just_func = std::move(std::get<nr>(std::move(all)));
    using value_tuple = typename subtuple<decltype(all), idx>::type;
    using ret_type = decltype(std::apply(just_func, std::declval<value_tuple&>()));
    auto task = std::apply(
        [](auto&&... x) {
            return std::make_unique<internal::do_with_state<value_tuple, ret_type>>(std::forward<decltype(x)>(x)...);
        },
        std::move(just_values));
    auto fut = std::apply(just_func, task->data());
    if (fut.available()) {
        return fut;
    }
    auto ret = task->get_future();
    internal::set_callback(std::move(fut), task.release());
    return ret;
}
}

/// \addtogroup future-util
/// @{

/// do_with() holds a objects alive until a future completes, and
/// allow the code involved in making the future complete to have easy
/// access to this object.
///
/// do_with() takes multiple arguments: The last is a function
/// returning a future. The other are temporary objects (rvalue). The
/// function is given (a moved copy of) these temporary object, by
/// reference, and it is ensured that the objects will not be
/// destructed until the completion of the future returned by the
/// function.
///
/// do_with() returns a future which resolves to whatever value the given future
/// (returned by the given function) resolves to. This returned value must not
/// contain references to the temporary object, as at that point the temporary
/// is destructed.
///
/// \return whatever the function returns
template <typename T1, typename T2, typename... More>
inline
auto
do_with(T1&& rv1, T2&& rv2, More&&... more) noexcept {
    auto func = internal::do_with_impl<T1, T2, More...>;
    return futurize_invoke(func, std::forward<T1>(rv1), std::forward<T2>(rv2), std::forward<More>(more)...);
}

/// Executes the function \c func making sure the lock \c lock is taken,
/// and later on properly released.
///
/// \param lock the lock, which is any object having providing a lock() / unlock() semantics.
///        Caller must make sure that it outlives \c func.
/// \param func function to be executed
/// \returns whatever \c func returns
template<typename Lock, typename Func>
inline
auto with_lock(Lock& lock, Func&& func) {
    return lock.lock().then([&lock, func = std::forward<Func>(func)] () mutable {
        return futurize_invoke(func).finally([&lock] {
            lock.unlock();
        });
    });
}

/// @}

}

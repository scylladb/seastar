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
 * Copyright (C) 2015 ScyllaDB
 */

#pragma once

#include <deque>
#include "future.hh"

/// \addtogroup future-module
/// @{

/// \brief Like \ref future except the result can be waited for by many fibers.
///
/// Represents a value which may not yet be ready. A fiber can wait for the value using
/// the \ref future obtained by calling \ref get_future() or casting to \ref future type.
/// Multiple fibers are allowed to obtain a \ref future for the result using the same
/// instance of \ref shared_future.
///
/// All futures obtained from shared_future should end up in the same state. However,
/// if the value's copy constructor throws, some of the futures may end up in a failed state
/// with an exception thrown from the copy constructor and end up with a state
/// different than other futures.
///
/// The scope of shared_future instance doesn't have to include scopes of the futures
/// obtained from that instance. In that sense the returned futures are independent.
///
/// shared_future can be copied at any time and all copies will resolve with the same value.
///
/// shared_future can be in a disengaged state when it's default-constructed or moved-from.
/// When it's in such a state we say it's invalid and obtaining futures must not be attempted.
///
/// The types in the parameter pack T must all be copy-constructible.
template<typename... T>
class shared_future {
    /// \cond internal
    class shared_state {
        future_state<T...> _future_state;
        std::deque<promise<T...>> _peers;
    public:
        void resolve(future<T...>&& f) noexcept {
            _future_state = f.get_available_state();
            if (_future_state.failed()) {
                for (auto&& p : _peers) {
                    p.set_exception(_future_state.get_exception());
                }
            } else {
                for (auto&& p : _peers) {
                    try {
                        p.set_value(_future_state.get_value());
                    } catch (...) {
                        p.set_exception(std::current_exception());
                    }
                }
            }
            _peers.clear();
        }

        future<T...> get_future() {
            if (!_future_state.available()) {
                promise<T...> p;
                auto f = p.get_future();
                _peers.emplace_back(std::move(p));
                return f;
            } else if (_future_state.failed()) {
                return make_exception_future<T...>(_future_state.get_exception());
            } else {
                try {
                    return make_ready_future<T...>(_future_state.get_value());
                } catch (...) {
                    return make_exception_future<T...>(std::current_exception());
                }
            }
        }
    };
    /// \endcond
    lw_shared_ptr<shared_state> _state;
public:
    /// \brief Forwards the result of future \c f into this shared_future.
    shared_future(future<T...>&& f)
        : _state(make_lw_shared<shared_state>())
    {
        f.then_wrapped([s = _state] (future<T...>&& f) mutable {
            s->resolve(std::move(f));
        });
    }

    shared_future() = default;
    shared_future(const shared_future&) = default;
    shared_future& operator=(const shared_future&) = default;
    shared_future(shared_future&&) = default;
    shared_future& operator=(shared_future&&) = default;

    /// \brief Creates a new \c future which will resolve with the result of this shared_future
    ///
    /// This object must be in a valid state.
    future<T...> get_future() const {
        return _state->get_future();
    }

    /// \brief Equivalent to \ref get_future()
    operator future<T...>() const {
        return get_future();
    }

    /// \brief Returns true if the instance is in valid state
    bool valid() const {
        return bool(_state);
    }
};

/// \brief Like \ref promise except that its counterpart is \ref shared_future instead of \ref future
///
/// When the shared_promise is made ready, every waiter is also made ready.
///
/// Like the shared_future, the types in the parameter pack T must all be copy-constructible.
template <typename... T>
class shared_promise : public enable_shared_from_this<shared_promise<T...>> {
    promise<T...> _promise;
    shared_future<T...> _shared_future;
    static constexpr bool copy_noexcept = future_state<T...>::copy_noexcept;
public:
    shared_promise(const shared_promise&) = delete;
    shared_promise(shared_promise&&) = default;
    shared_promise& operator=(shared_promise&&) = default;
    shared_promise() : _promise(), _shared_future(_promise.get_future()) {
    }

    future<T...> get_shared_future() {
        return _shared_future.get_future();
    }
    /// \brief Sets the shared_promise's value (as tuple; by copying), same as normal promise
    void set_value(const std::tuple<T...>& result) noexcept(copy_noexcept) {
        _promise.set_value(result);
    }

    /// \brief Sets the shared_promise's value (as tuple; by moving), same as normal promise
    void set_value(std::tuple<T...>&& result) noexcept {
        _promise.set_value(std::move(result));
    }

    /// \brief Sets the shared_promise's value (variadic), same as normal promise
    template <typename... A>
    void set_value(A&&... a) noexcept {
        _promise.set_value(std::forward<A>(a)...);
    }

    /// \brief Marks the shared_promise as failed, same as normal promise
    void set_exception(std::exception_ptr ex) noexcept {
        _promise.set_exception(std::move(ex));
    }

    /// \brief Marks the shared_promise as failed, same as normal promise
    template<typename Exception>
    void set_exception(Exception&& e) noexcept {
        set_exception(make_exception_ptr(std::forward<Exception>(e)));
    }
};

/// @}

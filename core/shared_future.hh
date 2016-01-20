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

namespace seastar {

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
template<typename... U>
class shared_future {
    /// \cond internal
    class shared_state {
	future_state<U...> _future_state;
	std::deque<promise<U...>> _peers;
    public:
        void resolve(future<U...>&& f) noexcept {
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

        future<U...> get_future() {
            if (!_future_state.available()) {
                promise<U...> p;
                auto f = p.get_future();
                _peers.emplace_back(std::move(p));
                return f;
            } else if (_future_state.failed()) {
                return make_exception_future<U...>(_future_state.get_exception());
            } else {
                try {
                    return make_ready_future<U...>(_future_state.get_value());
                } catch (...) {
                    return make_exception_future<U...>(std::current_exception());
                }
            }
        }
    };
    /// \endcond
    lw_shared_ptr<shared_state> _state;
public:
    /// \brief Forwards the result of future \c f into this shared_future.
    shared_future(future<U...>&& f)
        : _state(make_lw_shared<shared_state>())
    {
        f.then_wrapped([s = _state] (future<U...>&& f) mutable {
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
    future<U...> get_future() const {
        return _state->get_future();
    }

    /// \brief Equivalent to \ref get_future()
    operator future<U...>() const {
        return get_future();
    }

    /// \brief Returns true if the instance is in valid state
    bool valid() const {
        return bool(_state);
    }
};

} // namespace seastar

/// @}

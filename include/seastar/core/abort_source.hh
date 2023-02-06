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
 * Copyright (C) 2017 ScyllaDB.
 */

#pragma once

#include <seastar/util/concepts.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/optimized_optional.hh>
#include <seastar/util/std-compat.hh>
#include <boost/intrusive/list.hpp>

#include <exception>
#include <optional>
#include <utility>

namespace bi = boost::intrusive;

namespace seastar {

/// \addtogroup fiber-module
/// @{

/// Exception thrown when an \ref abort_source object has been
/// notified by the \ref abort_source::request_abort() method.
class abort_requested_exception : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "abort requested";
    }
};

/// Facility to communicate a cancellation request to a fiber.
/// Callbacks can be registered with the \c abort_source, which are called
/// atomically with a call to request_abort().
class abort_source {
    using subscription_callback_type = noncopyable_function<void (const std::optional<std::exception_ptr>&) noexcept>;
    using naive_subscription_callback_type = noncopyable_function<void() noexcept>;

public:
    /// Represents a handle to the callback registered by a given fiber. Ending the
    /// lifetime of the \c subscription will unregister the callback, if it hasn't
    /// been invoked yet.
    class subscription : public bi::list_base_hook<bi::link_mode<bi::auto_unlink>> {
        friend class abort_source;

        subscription_callback_type _target;

        explicit subscription(abort_source& as, subscription_callback_type target)
                : _target(std::move(target)) {
            as._subscriptions.push_back(*this);
        }

        struct naive_cb_tag {}; // to disambiguate constructors
        explicit subscription(naive_cb_tag, abort_source& as, naive_subscription_callback_type naive_cb)
                : _target([cb = std::move(naive_cb)] (const std::optional<std::exception_ptr>&) noexcept { cb(); }) {
            as._subscriptions.push_back(*this);
        }

        void on_abort(const std::optional<std::exception_ptr>& ex) noexcept {
            _target(ex);
        }

    public:
        subscription() = default;

        subscription(subscription&& other) noexcept(std::is_nothrow_move_constructible<subscription_callback_type>::value)
                : _target(std::move(other._target)) {
            subscription_list_type::node_algorithms::swap_nodes(other.this_ptr(), this_ptr());
        }

        subscription& operator=(subscription&& other) noexcept(std::is_nothrow_move_assignable<subscription_callback_type>::value) {
            if (this != &other) {
                _target = std::move(other._target);
                unlink();
                subscription_list_type::node_algorithms::swap_nodes(other.this_ptr(), this_ptr());
            }
            return *this;
        }

        explicit operator bool() const noexcept {
            return is_linked();
        }
    };

private:
    using subscription_list_type = bi::list<subscription, bi::constant_time_size<false>>;
    subscription_list_type _subscriptions;
    std::exception_ptr _ex;

    void do_request_abort(std::optional<std::exception_ptr> ex) noexcept {
        if (_ex) {
            return;
        }
        _ex = ex.value_or(get_default_exception());
        assert(_ex);
        auto subs = std::move(_subscriptions);
        while (!subs.empty()) {
            subscription& s = subs.front();
            s.unlink();
            s.on_abort(ex);
        }
    }

public:
    abort_source() = default;
    virtual ~abort_source() = default;

    abort_source(abort_source&&) = default;
    abort_source& operator=(abort_source&&) = default;

    /// Delays the invocation of the callback \c f until \ref request_abort() is called.
    /// \returns an engaged \ref optimized_optional containing a \ref subscription that can be used to control
    ///          the lifetime of the callback \c f, if \ref abort_requested() is \c false. Otherwise,
    ///          returns a disengaged \ref optimized_optional.
    template <typename Func>
    SEASTAR_CONCEPT(requires
            requires (Func f, const std::optional<std::exception_ptr>& opt_ex) { { f(opt_ex) } noexcept -> std::same_as<void>; }
        ||  requires (Func f) { { f() } noexcept -> std::same_as<void>; }
    )
    [[nodiscard]]
    optimized_optional<subscription> subscribe(Func&& f) {
        if (abort_requested()) {
            return { };
        }
        if constexpr (std::is_invocable_v<Func, std::exception_ptr>) {
            return { subscription(*this, std::forward<Func>(f)) };
        } else {
            return { subscription(subscription::naive_cb_tag{}, *this, std::forward<Func>(f)) };
        }
    }

    /// Requests that the target operation be aborted. Current subscriptions
    /// are invoked inline with this call with a disengaged optional<std::exception_ptr>,
    /// and no new ones can be registered.
    void request_abort() noexcept {
        do_request_abort(std::nullopt);
    }

    /// Requests that the target operation be aborted with a given \c exception_ptr.
    /// Current subscriptions are invoked inline with this exception,
    /// and no new ones can be registered.
    void request_abort_ex(std::exception_ptr ex) noexcept {
        do_request_abort(std::make_optional(std::move(ex)));
    }

    /// Requests that the target operation be aborted with a given \c Exception object.
    /// Current subscriptions are invoked inline with this exception, converted to std::exception_ptr,
    /// and no new ones can be registered.
    template <typename Exception>
    void request_abort_ex(Exception&& e) noexcept {
        do_request_abort(std::make_optional(std::make_exception_ptr(std::forward<Exception>(e))));
    }

    /// Returns whether an abort has been requested.
    bool abort_requested() const noexcept {
        return bool(_ex);
    }


    /// Throws a \ref abort_requested_exception if cancellation has been requested.
    void check() const {
        if (abort_requested()) {
            std::rethrow_exception(_ex);
        }
    }

    /// Returns the default exception type (\ref abort_requested_exception) for this abort source.
    /// Overridable by derived classes.
    virtual std::exception_ptr get_default_exception() const noexcept {
        return make_exception_ptr(abort_requested_exception());
    }
};

/// @}

}

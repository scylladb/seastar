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

#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/optimized_optional.hh>
#include <seastar/util/std-compat.hh>

#ifndef SEASTAR_MODULE
#include <boost/intrusive/list.hpp>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#endif

namespace bi = boost::intrusive;

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

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
        bool _aborted = false;

        explicit subscription(abort_source& as, subscription_callback_type target)
                : _target(std::move(target)) {
          if (!as.abort_requested()) {
            as._subscriptions.push_back(*this);
          }
        }

        struct naive_cb_tag {}; // to disambiguate constructors
        explicit subscription(naive_cb_tag, abort_source& as, naive_subscription_callback_type naive_cb)
                : _target([cb = std::move(naive_cb)] (const std::optional<std::exception_ptr>&) noexcept { cb(); }) {
          if (!as.abort_requested()) {
            as._subscriptions.push_back(*this);
          }
        }

    public:
        /// Call the subscribed callback (at most once).
        /// This method is called by the \ref abort_source on all listed \ref subscription objects
        /// when \ref request_abort() is called.
        /// It may be called indepdently by the user at any time, causing the \ref subscription
        /// to be unlinked from the \ref abort_source subscriptions list.
        void on_abort(const std::optional<std::exception_ptr>& ex) noexcept {
            unlink();
            if (!std::exchange(_aborted, true)) {
                _target(ex);
            }
        }

    public:
        subscription() = default;

        subscription(subscription&& other) noexcept(std::is_nothrow_move_constructible_v<subscription_callback_type>)
                : _target(std::move(other._target))
                , _aborted(std::exchange(other._aborted, true))
        {
            subscription_list_type::node_algorithms::swap_nodes(other.this_ptr(), this_ptr());
        }

        subscription& operator=(subscription&& other) noexcept(std::is_nothrow_move_assignable_v<subscription_callback_type>) {
            if (this != &other) {
                _target = std::move(other._target);
                _aborted = std::exchange(other._aborted, true);
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
        SEASTAR_ASSERT(_ex);
        auto subs = std::move(_subscriptions);
        while (!subs.empty()) {
            subscription& s = subs.front();
            s.on_abort(ex);
        }
    }

public:
    abort_source() = default;
    virtual ~abort_source() = default;

    abort_source(abort_source&&) = default;
    abort_source& operator=(abort_source&&) = default;

    /// Delays the invocation of the callback \c f until \ref request_abort() is called.
    /// \returns \ref optimized_optional containing a \ref subscription that can be used to control
    ///          the lifetime of the callback \c f.
    ///
    /// Note: the returned \ref optimized_optional evaluates to \c true if and only if
    /// \ref abort_requested() is \c false at the time \ref subscribe is called, and therefore
    /// the \ref subscription is linked to the \ref abort_source subscriptions list.
    ///
    /// Once \ref request_abort() is called or the subscription's \ref on_abort() method are called,
    /// the callback \c f is called (exactly once), and the \ref subscription is unlinked from
    /// the \ref about_source, causing the \ref optimized_optional to evaluate to \c false.
    ///
    /// The returned \ref optimized_optional would initially evaluate to \c false if \ref request_abort()
    /// was already called. In this case, an unlinked \ref subscription is returned as \ref optimized_optional.
    /// That \ref subscription still allows the user to call \ref on_abort() to invoke the callback \c f.
    template <typename Func>
        requires (std::is_nothrow_invocable_r_v<void, Func, const std::optional<std::exception_ptr>&> ||
                  std::is_nothrow_invocable_r_v<void, Func>)
    [[nodiscard]]
    optimized_optional<subscription> subscribe(Func&& f) {
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

    /// Returns an exception with which an abort was requested.
    const std::exception_ptr& abort_requested_exception_ptr() const noexcept {
        return _ex;
    }

    /// Returns the default exception type (\ref abort_requested_exception) for this abort source.
    /// Overridable by derived classes.
    virtual std::exception_ptr get_default_exception() const noexcept {
        return make_exception_ptr(abort_requested_exception());
    }
};

/// @}

SEASTAR_MODULE_EXPORT_END

}

#if FMT_VERSION < 100000
// fmt v10 introduced formatter for std::exception
template <>
struct fmt::formatter<seastar::abort_requested_exception> : fmt::formatter<string_view> {
    auto format(const seastar::abort_requested_exception& e, fmt::format_context& ctx) const {
        return fmt::format_to(ctx.out(), "{}", e.what());
    }
};
#endif

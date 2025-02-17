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
 * Copyright 2014 Cloudius Systems
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <boost/intrusive/list.hpp>

#include <seastar/core/future.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#include <cassert>
#include <exception>
#include <optional>
#include <utility>
#endif

#ifdef SEASTAR_DEBUG
#define SEASTAR_GATE_HOLDER_DEBUG
#endif

namespace seastar {

/// \addtogroup fiber-module
/// @{

/// Exception thrown when a \ref gate object has been closed
/// by the \ref gate::close() method.
SEASTAR_MODULE_EXPORT
class gate_closed_exception : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "gate closed";
    }
};

/// Facility to stop new requests, and to tell when existing requests are done.
///
/// When stopping a service that serves asynchronous requests, we are faced with
/// two problems: preventing new requests from coming in, and knowing when existing
/// requests have completed.  The \c gate class provides a solution.
SEASTAR_MODULE_EXPORT
class gate {
    size_t _count = 0;
    std::optional<promise<>> _stopped;

#ifdef SEASTAR_GATE_HOLDER_DEBUG
    void assert_not_held_when_moved() const noexcept;
    void assert_not_held_when_destroyed() const noexcept;
#else   // SEASTAR_GATE_HOLDER_DEBUG
    void assert_not_held_when_moved() const noexcept {}
    void assert_not_held_when_destroyed() const noexcept {}
#endif  // SEASTAR_GATE_HOLDER_DEBUG

public:
    // Implemented to force noexcept due to boost:intrusive::list
    gate() noexcept {};
    gate(const gate&) = delete;
    gate(gate&& x) noexcept
        : _count(std::exchange(x._count, 0)), _stopped(std::exchange(x._stopped, std::nullopt)) {
        x.assert_not_held_when_moved();
    }
    gate& operator=(gate&& x) noexcept {
        if (this != &x) {
            SEASTAR_ASSERT(!_count && "gate reassigned with outstanding requests");
            x.assert_not_held_when_moved();
            _count = std::exchange(x._count, 0);
            _stopped = std::exchange(x._stopped, std::nullopt);
        }
        return *this;
    }
    ~gate() {
        SEASTAR_ASSERT(!_count && "gate destroyed with outstanding requests");
        assert_not_held_when_destroyed();
    }
    /// Tries to register an in-progress request.
    ///
    /// If the gate is not closed, the request is registered and the function returns `true`,
    /// Otherwise the function just returns `false` and has no other effect.
    bool try_enter() noexcept {
        bool opened = !_stopped;
        if (opened) {
            ++_count;
        }
        return opened;
    }
    /// Registers an in-progress request.
    ///
    /// If the gate is not closed, the request is registered.  Otherwise,
    /// a \ref gate_closed_exception is thrown.
    void enter() {
        if (!try_enter()) {
            throw gate_closed_exception();
        }
    }
    /// Unregisters an in-progress request.
    ///
    /// If the gate is closed, and there are no more in-progress requests,
    /// the `_stopped` promise will be fulfilled.
    void leave() noexcept {
        --_count;
        if (!_count && _stopped) {
            _stopped->set_value();
        }
    }
    /// Potentially stop an in-progress request.
    ///
    /// If the gate is already closed, a \ref gate_closed_exception is thrown.
    /// By using \ref enter() and \ref leave(), the program can ensure that
    /// no further requests are serviced. However, long-running requests may
    /// continue to run. The check() method allows such a long operation to
    /// voluntarily stop itself after the gate is closed, by making calls to
    /// check() in appropriate places. check() with throw an exception and
    /// bail out of the long-running code if the gate is closed.
    void check() const {
        if (_stopped) {
            throw gate_closed_exception();
        }
    }
    /// Closes the gate.
    ///
    /// Future calls to \ref enter() will fail with an exception, and when
    /// all current requests call \ref leave(), the returned future will be
    /// made ready.
    future<> close() noexcept {
        SEASTAR_ASSERT(!_stopped && "seastar::gate::close() cannot be called more than once");
        _stopped = std::make_optional(promise<>());
        if (!_count) {
            _stopped->set_value();
        }
        return _stopped->get_future();
    }

    /// Returns a current number of registered in-progress requests.
    size_t get_count() const noexcept {
        return _count;
    }

    /// Returns whether the gate is closed.
    bool is_closed() const noexcept {
        return bool(_stopped);
    }

    /// Facility to hold a gate opened using RAII.
    ///
    /// A \ref gate::holder is usually obtained using \ref gate::hold.
    ///
    /// The \c gate is entered when the \ref gate::holder is constructed,
    /// And the \c gate is left when the \ref gate::holder is destroyed.
    ///
    /// Copying the \ref gate::holder reenters the \c gate to keep an extra reference on it.
    /// Moving the \ref gate::holder is supported and has no effect on the \c gate itself.
    class holder {
        gate* _g;
#ifdef SEASTAR_GATE_HOLDER_DEBUG
        using member_hook_t = boost::intrusive::list_member_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;
        member_hook_t _link;

        void debug_hold_gate() noexcept {
            if (_g) {
                _g->_holders.push_back(*this);
            }
        }

        void debug_release_gate() noexcept {
            _link.unlink();
        }
#else   // SEASTAR_GATE_HOLDER_DEBUG
        void debug_hold_gate() noexcept {}
        void debug_release_gate() noexcept {}
#endif  // SEASTAR_GATE_HOLDER_DEBUG

        // release the holder from the gate without leaving it.
        // used for release and move.
        gate* release_gate() noexcept {
            auto* g = std::exchange(_g, nullptr);
            if (g) {
                debug_release_gate();
            }
            return g;
        }

        friend class gate;
    public:
        /// Construct a default \ref holder, referencing no \ref gate.
        /// Never throws.
        holder() noexcept : _g(nullptr) { }

        /// Construct a \ref holder by entering the \c gate.
        /// May throw \ref gate_closed_exception if the gate is already closed.
        explicit holder(gate& g) : _g(&g) {
            _g->enter();
            debug_hold_gate();
        }

        /// Construct a \ref holder by copying another \c holder.
        /// Copying a holder never throws: The original holder has already entered the gate,
        /// so even if later the gate was \ref close "close()d", the copy of the holder is also allowed to enter too.
        /// Note that the fiber waiting for the close(), which until now was waiting for the one holder to leave,
        /// will now wait for both copies to leave.
        holder(const holder& x) noexcept : _g(x._g) {
            if (_g) {
                _g->_count++;
                debug_hold_gate();
            }
        }

        /// Construct a \ref holder by moving another \c holder.
        /// The referenced \ref gate is unaffected, and so the
        /// move-constructor must never throw.
        holder(holder&& x) noexcept : _g(std::move(x).release_gate()) {
            debug_hold_gate();
        }

        /// Destroy a \ref holder and leave the referenced \ref gate.
        ~holder() {
            release();
        }

        /// Copy-assign another \ref holder.
        /// \ref leave "Leave()" the current \ref gate before assigning the other one, if they are different.
        /// Copying a holder never throws: The original holder has already entered the gate,
        /// so even if later the gate was \ref close "close()d", the copy of the holder is also allowed to enter too.
        /// Note that the fiber waiting for the close(), which until now was waiting for the one holder to leave,
        /// will now wait for both copies to leave.
        holder& operator=(const holder& x) noexcept {
            if (x._g != _g) {
                release();
                _g = x._g;
                if (_g) {
                    _g->_count++;
                    debug_hold_gate();
                }
            }
            return *this;
        }

        /// Move-assign another \ref holder.
        /// The other \ref gate is unaffected,
        /// and so the move-assign operator must always succeed.
        /// Leave the current \ref gate before assigning the other one.
        holder& operator=(holder&& x) noexcept {
            if (&x != this) {
                release();
                _g = std::move(x).release_gate();
                debug_hold_gate();
            }
            return *this;
        }

        /// Leave the held \c gate
        void release() noexcept {
            if (auto g = release_gate()) {
                g->leave();
            }
        }
    };

    /// Get a RAII-based gate::holder object that \ref enter "enter()s"
    /// the gate when constructed and \ref leave "leave()s" it when destroyed.
    holder hold() {
        return holder(*this);
    }

    /// Try getting an optional RAII-based gate::holder object that \ref enter "enter()s"
    /// the gate when constructed and \ref leave "leave()s" it when destroyed.
    /// Returns std::nullopt iff the gate is closed.
    std::optional<holder> try_hold() noexcept {
        return is_closed() ? std::nullopt : std::make_optional<holder>(*this);
    }

private:
#ifdef SEASTAR_GATE_HOLDER_DEBUG
    using holders_list_t = boost::intrusive::list<holder,
        boost::intrusive::member_hook<holder, holder::member_hook_t, &holder::_link>,
        boost::intrusive::constant_time_size<false>>;

    holders_list_t _holders;
#endif  // SEASTAR_GATE_HOLDER_DEBUG
};

#ifdef SEASTAR_GATE_HOLDER_DEBUG
SEASTAR_MODULE_EXPORT
inline void gate::assert_not_held_when_moved() const noexcept {
    SEASTAR_ASSERT(_holders.empty() && "gate moved with outstanding holders");
}
inline void gate::assert_not_held_when_destroyed() const noexcept {
    SEASTAR_ASSERT(_holders.empty() && "gate destroyed with outstanding holders");
}
#endif  // SEASTAR_GATE_HOLDER_DEBUG

namespace internal {

template <typename Func>
inline
auto
invoke_func_with_gate(gate::holder&& gh, Func&& func) noexcept {
    return futurize_invoke(std::forward<Func>(func)).finally([gh = std::forward<gate::holder>(gh)] {});
}

} // namespace internal

/// Executes the function \c func making sure the gate \c g is properly entered
/// and later on, properly left.
///
/// \param func function to be executed
/// \param g the gate. Caller must make sure that it outlives this function.
/// \returns whatever \c func returns
///
/// \relates gate
SEASTAR_MODULE_EXPORT
template <typename Func>
inline
auto
with_gate(gate& g, Func&& func) {
    return internal::invoke_func_with_gate(g.hold(), std::forward<Func>(func));
}

/// Executes the function \c func if the gate \c g can be entered
/// and later on, properly left.
///
/// \param func function to be executed
/// \param g the gate. Caller must make sure that it outlives this function.
///
/// If the gate is already closed, an exception future holding
/// \ref gate_closed_exception is returned, otherwise
/// \returns whatever \c func returns.
///
/// \relates gate
SEASTAR_MODULE_EXPORT
template <typename Func>
inline
auto
try_with_gate(gate& g, Func&& func) noexcept {
    if (g.is_closed()) {
        using futurator = futurize<std::invoke_result_t<Func>>;
        return futurator::make_exception_future(gate_closed_exception());
    }
    return internal::invoke_func_with_gate(g.hold(), std::forward<Func>(func));
}
/// @}


}

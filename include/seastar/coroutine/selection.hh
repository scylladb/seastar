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
 * Copyright (C) 2026-present ScyllaDB
 */

#pragma once

#include <bit>
#include <cstdint>
#include <optional>
#include <type_traits>

#include <fmt/format.h>

#include <boost/intrusive/list.hpp>

#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/lowres_clock.hh>
#include <seastar/core/on_internal_error.hh>
#include <seastar/core/timer.hh>

namespace seastar {
// Defined in reactor.cc; used to report the fatal draining-contract violation.
extern logger seastar_logger;
}

namespace seastar::coroutine {

/// A callable that takes no arguments and returns a future, as accepted by
/// \ref selection::add.
template <typename Func>
concept future_returning_func = std::invocable<Func> && is_future<std::invoke_result_t<Func>>::value;

namespace internal {

namespace bi = boost::intrusive;

// The logical value type T of a future<T> (unlike future<T>::value_type, this
// is void rather than the internal stored type for future<>).
template <typename F>
struct future_value_type;
template <typename T>
struct future_value_type<future<T>> {
    using type = T;
};
template <typename F>
using future_value_type_t = typename future_value_type<F>::type;

template <typename Token>
class selection_base;

/// Type-erased (over the future value type) base of everything a selection is
/// waiting on. A slot is pinned in memory from the moment it is registered
/// until it fires, because (for future-backed slots) the producer's promise
/// stores a raw pointer to it. It carries the user's \p Token, which is what
/// \c co_await hands back.
///
/// A slot lives on exactly one of the selection's two lists at a time — the
/// waiting list until it fires, then the ready list until it is consumed — so a
/// single hook suffices.
template <typename Token>
struct sel_slot_base {
    bi::list_member_hook<> _hook;
    selection_base<Token>* _sel = nullptr;
    Token _token;
    uint16_t _chunk = 0;                  // first arena chunk backing this slot
    uint16_t _nchunks = 0;                // number of chunks, or 0 when heap-allocated

    explicit sel_slot_base(Token token) : _token(std::move(token)) {}

    // Run the concrete destructor; frees heap storage, leaves arena storage.
    virtual void destroy() noexcept = 0;

protected:
    ~sel_slot_base() = default;
};

[[noreturn]] inline void on_selection_abandoned(size_t pending) noexcept {
    on_fatal_internal_error(seastar_logger, fmt::format(
            "coroutine::selection destroyed with {} pending future(s); a selection "
            "must be drained (co_await until it yields no token) before it is "
            "destroyed, because a future cannot be cancelled and its slot cannot be "
            "relocated out of the coroutine frame", pending));
}

/// Non-template-over-value-type interface the slots and the awaiter talk to.
/// Owns the waiting and ready lists, the parked waiter, and the inline arena
/// bookkeeping. The concrete storage lives in \ref selection.
template <typename Token>
class selection_base {
public:
    using slot = sel_slot_base<Token>;

private:
    using slot_list = bi::list<slot,
            bi::member_hook<slot, bi::list_member_hook<>, &slot::_hook>,
            bi::constant_time_size<false>>;

public:
    // The inline arena is carved into fixed-size chunks; a slot occupies a
    // contiguous run of them. Chunk size equals the maximum fundamental
    // alignment, so any chunk boundary is suitably aligned for any slot.
    static constexpr uint32_t chunk_size = alignof(std::max_align_t);

private:
    std::byte* _arena;
    uint64_t* _free;              // bitmap; bit i set => chunk i is free
    uint32_t _num_chunks;

    slot_list _waiting;           // registered futures, not yet fired
    slot_list _ready;             // fired, not yet consumed
    task* _waiter = nullptr;      // the parked coroutine, while co_await is suspended

protected:
    selection_base(std::byte* arena, uint64_t* free_bitmap, uint32_t num_words, uint32_t num_chunks) noexcept
            : _arena(arena), _free(free_bitmap), _num_chunks(num_chunks) {
        for (uint32_t w = 0; w < num_words; ++w) {
            _free[w] = 0;
        }
        for (uint32_t i = 0; i < num_chunks; ++i) {
            set_free(i);
        }
    }
    ~selection_base() {
        // A slot still on the waiting list is a future in flight. It is pinned
        // (the producer's promise points at it) and cannot be cancelled, so the
        // selection must not be destroyed while any remain: hard error.
        if (!_waiting.empty()) {
            on_selection_abandoned(_waiting.size());
        }
        // Fired-but-unconsumed results (only on an abnormal early exit) are safe
        // to drop here.
        drain_list(_ready);
    }

    // Reserve a run of \p n contiguous free chunks (first fit). Returns the
    // starting chunk index, or -1 if no such run fits the inline arena.
    int alloc_run(uint32_t n) noexcept {
        uint32_t run = 0;
        for (uint32_t i = 0; i < _num_chunks; ++i) {
            if (chunk_free(i)) {
                if (++run == n) {
                    uint32_t start = i + 1 - n;
                    for (uint32_t j = start; j <= i; ++j) {
                        clear_free(j);
                    }
                    return int(start);
                }
            } else {
                run = 0;
            }
        }
        return -1;
    }
    void free_run(uint32_t start, uint32_t n) noexcept {
        for (uint32_t j = start; j < start + n; ++j) {
            set_free(j);
        }
    }
    void* chunk_ptr(uint32_t i) noexcept {
        return _arena + size_t(i) * chunk_size;
    }
    // Register a freshly-constructed slot on the waiting list.
    void register_slot(slot* s) noexcept {
        s->_sel = this;
        _waiting.push_back(*s);
    }

public:
    void on_future_ready(slot* s) noexcept {
        enqueue_ready(s);
    }

    // Destroy a slot (already unlinked from any list by the caller) and reclaim
    // its storage.
    void dispose(slot* s) noexcept {
        uint16_t chunk = s->_chunk;
        uint16_t nchunks = s->_nchunks;
        s->destroy();                       // ~concrete (heap slots delete themselves)
        if (nchunks) {
            free_run(chunk, nchunks);
        }
    }

    task* waiter() const noexcept { return _waiter; }

    // Awaiter support
    bool ready_now() const noexcept { return !_ready.empty(); }
    bool drained() const noexcept { return _waiting.empty() && _ready.empty(); }
    void park(task* t) noexcept { _waiter = t; }

    // Pop the next ready slot, returning its token and disposing the slot (the
    // result, if any, already lives in the caller's future). Returns nullopt
    // when the selection is drained.
    std::optional<Token> take_ready() noexcept {
        if (_ready.empty()) {
            return std::nullopt;
        }
        slot& s = _ready.front();
        _ready.pop_front();
        Token token = std::move(s._token);
        dispose(&s);
        return token;
    }

private:
    // Move a slot from the waiting list to the ready list and wake the parked
    // coroutine, if any.
    void enqueue_ready(slot* s) noexcept {
        _waiting.erase(_waiting.iterator_to(*s));
        _ready.push_back(*s);
        if (_waiter) {
            // Resume the parked coroutine on the next reactor turn rather than
            // recursively, so we never touch *s again after handing control back.
            schedule(std::exchange(_waiter, nullptr));
        }
    }

    void drain_list(slot_list& l) noexcept {
        while (!l.empty()) {
            slot& s = l.front();
            l.pop_front();
            dispose(&s);
        }
    }

    bool chunk_free(uint32_t i) const noexcept { return _free[i >> 6] & (uint64_t(1) << (i & 63)); }
    void set_free(uint32_t i) noexcept { _free[i >> 6] |= (uint64_t(1) << (i & 63)); }
    void clear_free(uint32_t i) noexcept { _free[i >> 6] &= ~(uint64_t(1) << (i & 63)); }
};

/// A slot fed by a future<T>: it *is* the continuation the producer resolves,
/// and it forwards the result into the promise handed back by add().
template <typename Token, typename T>
struct future_slot final : public sel_slot_base<Token>, public continuation_base<T> {
    promise<T> _promise;

    explicit future_slot(Token token) : sel_slot_base<Token>(std::move(token)) {}

    future<T> get_future() noexcept { return _promise.get_future(); }

    void run_and_dispose() noexcept override {
        // The framework has moved the result into continuation_base::_state;
        // forward it to the caller's future, then report readiness.
        if (this->_state.failed()) {
            _promise.set_exception(std::move(this->_state).get_exception());
        } else if constexpr (std::is_void_v<T>) {
            _promise.set_value();
        } else {
            _promise.set_value(std::move(this->_state).get());
        }
        this->_sel->on_future_ready(this);
        // Do not touch *this afterwards.
    }
    task* waiting_task() noexcept override { return this->_sel->waiter(); }
    void destroy() noexcept override {
        if (this->_nchunks == 0) {
            delete this;
        } else {
            this->~future_slot();
        }
    }
};

} // namespace internal

/// \brief A poll()/epoll()-style multiplexer over futures, for use inside a
/// coroutine.
///
/// A \c selection is declared as a local variable in a coroutine frame. You
/// register futures (possibly of different value types) with \ref add, tagging
/// each with a caller-chosen \p Token; \ref add hands back a \c future<T> for
/// the result. You then repeatedly \c co_await the selection in a \c while
/// loop: each \c co_await parks the coroutine until one registered event
/// becomes ready and returns its \p Token (as \c std::optional<Token>, empty
/// once the selection is drained). Use the token to dispatch; the matching
/// future is then ready, so \c co_await on it (or \c get()) yields the result
/// without suspending:
///
/// A deadline is not built in; register a \ref deadline's future like any other
/// (see \ref deadline), which also makes it cancellable.
///
/// ```
/// coroutine::selection sel;
/// std::vector<future<int>> reads;
/// for (size_t i = 0; i < n; ++i) {
///     reads.push_back(sel.add(i, [i] { return read_from(i); }));
/// }
/// int total = 0;
/// while (auto which = co_await sel) {
///     total += co_await std::move(reads[*which]);   // O(1) dispatch
/// }
/// ```
///
/// More events may be registered between iterations (while the coroutine is
/// running), but not while it is parked in \c co_await.
///
/// \par Draining contract
/// A future cannot be cancelled, and a slot registered inside the frame cannot
/// be relocated out of it. Therefore the loop must run until \c co_await yields
/// an empty optional (the drained state) before the \c selection is destroyed.
/// Leaving the loop early (via \c break, \c return, or an exception unwinding
/// the frame) while futures are still pending is a programming error and aborts
/// the process.
///
/// Most callers should use the \ref selection alias, which picks a reasonable
/// inline-storage size; use \c basic_selection directly to control it.
///
/// \tparam Token caller-chosen tag type identifying each registered event
///         (e.g. an index or enum); must be copyable.
/// \tparam InlineStorageBytes size, in bytes, of the in-place arena used to
///         store slots before spilling to the heap.
template <typename Token, size_t InlineStorageBytes>
class [[nodiscard]] basic_selection : private internal::selection_base<Token> {
    using base = internal::selection_base<Token>;
    static constexpr uint32_t num_chunks = InlineStorageBytes / base::chunk_size;
    static constexpr uint32_t num_words = (num_chunks + 63) / 64;
    static_assert(num_chunks >= 1, "InlineStorageBytes is smaller than one chunk");
    static_assert(num_chunks <= 65535, "InlineStorageBytes is too large");

    alignas(std::max_align_t) std::byte _arena[num_chunks * base::chunk_size];
    uint64_t _free[num_words];

    template <typename Slot>
    Slot* make_slot(Token token) {
        static_assert(alignof(Slot) <= base::chunk_size);
        uint32_t n = (sizeof(Slot) + base::chunk_size - 1) / base::chunk_size;
        int start = this->alloc_run(n);
        Slot* s;
        if (start >= 0) {
            s = new (this->chunk_ptr(start)) Slot(std::move(token));
            s->_chunk = start;
            s->_nchunks = n;
        } else {
            s = new Slot(std::move(token));   // _nchunks stays 0: heap-allocated
        }
        this->register_slot(s);
        return s;
    }

public:
    basic_selection() noexcept
            : base(_arena, _free, num_words, num_chunks) {
    }

    basic_selection(const basic_selection&) = delete;
    basic_selection& operator=(const basic_selection&) = delete;

    /// \brief Register work to wait for, tagged with \p token.
    ///
    /// Invokes \p func (a nullary callable returning a future) and starts
    /// listening for the resulting future. An exception thrown synchronously by
    /// \p func is captured into the returned future. When \c co_await on the
    /// selection returns \p token, \c co_await the returned future to obtain the
    /// result.
    ///
    /// Storage for the slot is taken from the selection's inline arena when it
    /// fits and a slot is free, otherwise it is heap-allocated.
    auto add(Token token, future_returning_func auto&& func)
            -> future<internal::future_value_type_t<decltype(func())>> {
        using T = internal::future_value_type_t<decltype(func())>;
        auto* s = make_slot<internal::future_slot<Token, T>>(std::move(token));
        auto f = s->get_future();
        // futurize_invoke turns a synchronous throw into an exceptional future.
        ::seastar::internal::set_callback(futurize_invoke(std::forward<decltype(func)>(func)),
                                          static_cast<continuation_base<T>*>(s));
        return f;
    }

    struct awaiter {
        basic_selection& _sel;
        bool await_ready() const noexcept {
            return _sel.ready_now() || _sel.drained();
        }
        template <typename Promise>
        void await_suspend(std::coroutine_handle<Promise> h SEASTAR_COROUTINE_LOC_PARAM) noexcept {
            SEASTAR_COROUTINE_LOC_STORE(h.promise());
            _sel.park(&h.promise());
        }
        std::optional<Token> await_resume() noexcept {
            return _sel.take_ready();
        }
    };

    awaiter operator co_await() noexcept { return awaiter{*this}; }
};

/// \brief A ready-to-use \ref basic_selection with all parameters chosen.
///
/// This is the type most callers want: a \c std::size_t token and a reasonable
/// amount of in-place slot storage. Use \ref basic_selection directly to choose
/// a different token type or inline-storage size.
using selection = basic_selection<std::size_t, 512>;

/// \brief A cancellable deadline expressed as a future, for use with \ref selection.
///
/// A \c deadline arms a timer on construction; its \ref get_future resolves when
/// the timer expires. Register that future with a \ref selection via
/// \ref selection::add to make a deadline one of the events being selected on —
/// no special support is needed in the selection, since it is an ordinary
/// future:
///
/// ```
/// coroutine::selection sel;
/// coroutine::deadline deadline(100ms);
/// auto tf = sel.add(my_token, [&] { return deadline.get_future(); });
/// ```
///
/// Call \ref cancel to resolve the future early — e.g. once another event has
/// made the deadline moot — so the selection can drain without waiting for the
/// timer to elapse.
///
/// Like \ref selection, a \c deadline is non-movable and must outlive any pending
/// future it handed out.
template <typename Clock = lowres_clock>
class deadline {
    promise<> _promise;
    seastar::timer<Clock> _timer;

public:
    /// Arm the deadline to fire \p delay from now.
    explicit deadline(typename Clock::duration delay)
            : _timer([this] { _promise.set_value(); }) {
        _timer.arm(delay);
    }
    /// Arm the deadline to fire at the absolute time \p when.
    explicit deadline(typename Clock::time_point when)
            : _timer([this] { _promise.set_value(); }) {
        _timer.arm(when);
    }

    deadline(const deadline&) = delete;
    deadline& operator=(const deadline&) = delete;

    /// The future that becomes ready when the timer fires or \ref cancel is called.
    future<> get_future() noexcept { return _promise.get_future(); }

    /// Resolve the future now if the timer has not fired yet; otherwise a no-op.
    void cancel() noexcept {
        if (_timer.cancel()) {
            _promise.set_value();
        }
    }
};

} // namespace seastar::coroutine

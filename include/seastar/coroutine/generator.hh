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
 * Copyright (C) 2022 Kefu Chai ( tchaikov@gmail.com )
 */

#pragma once

#include <coroutine>
#include <optional>
#include <utility>
#include <seastar/core/future.hh>
#include <seastar/util/assert.hh>

namespace seastar::coroutine::experimental {

template<typename T, template <typename> class Container = std::optional>
class generator;

/// `seastar::coroutine::experimental` is used as the type of the first
/// parameter of a buffered generator coroutine.
///
/// the value of a `buffer_size_t` specifies the size of the buffer holding the
/// values produced by the generator coroutine. Unlike its unbuffered variant,
/// the bufferred generator does not wait for its caller to consume every single
/// produced values. Instead, it puts the produced values into an internal
/// buffer, before the buffer is full or the generator is suspended. This helps
/// to alleviate the problem of pingpong between the generator coroutine and
/// its caller.
enum class buffer_size_t : size_t;

namespace internal {

using std::coroutine_handle;
using std::suspend_never;
using std::suspend_always;
using std::suspend_never;
using std::noop_coroutine;

template<typename T>
using next_value_t = std::optional<T>;

template <template <typename> class Container, typename T>
concept Fifo = requires(Container<T>&& c, T&& value) {
    // better off returning a reference though, so we can move away from it
    { c.front() } -> std::same_as<T&>;
    c.pop_front();
    c.push_back(value);
    bool(c.empty());
    { c.size() } -> std::convertible_to<size_t>;
};

template<typename T>
concept NothrowMoveConstructible = std::is_nothrow_move_constructible_v<T>;

template<NothrowMoveConstructible T>
class generator_unbuffered_promise final : public seastar::task {
    using generator_type = seastar::coroutine::experimental::generator<T, std::optional>;
    std::optional<seastar::promise<>> _wait_for_next_value;
    generator_type* _generator = nullptr;

public:
    generator_unbuffered_promise() = default;
    generator_unbuffered_promise(generator_unbuffered_promise&&) = delete;
    generator_unbuffered_promise(const generator_unbuffered_promise&) = delete;

    void return_void() noexcept;
    void unhandled_exception() noexcept;

    template<std::convertible_to<T> U>
    suspend_always yield_value(U&& value) noexcept {
        SEASTAR_ASSERT(_generator);
        _generator->put_next_value(std::forward<U>(value));
        SEASTAR_ASSERT(_wait_for_next_value);
        _wait_for_next_value->set_value();
        _wait_for_next_value = {};
        return {};
    }

    generator_type get_return_object() noexcept;
    void set_generator(generator_type* g) noexcept {
        SEASTAR_ASSERT(!_generator);
        _generator = g;
    }

    suspend_always initial_suspend() const noexcept { return {}; }
    suspend_never final_suspend() const noexcept {
        SEASTAR_ASSERT(_generator);
        _generator->on_finished();
        return {};
    }

    seastar::future<> wait_for_next_value() noexcept {
        SEASTAR_ASSERT(!_wait_for_next_value);
        return _wait_for_next_value.emplace().get_future();
    }

    void run_and_dispose() noexcept final {
        using handle_type = coroutine_handle<generator_unbuffered_promise>;
        handle_type::from_promise(*this).resume();
    }

    seastar::task* waiting_task() noexcept final {
        if (_wait_for_next_value) {
            return _wait_for_next_value->waiting_task();
        } else {
            return nullptr;
        }
    }
};

template <NothrowMoveConstructible T, template <typename> class Container>
requires Fifo<Container, T>
class generator_buffered_promise;

template<typename T, template <typename> class Container>
struct yield_awaiter final {
    using promise_type = generator_buffered_promise<T, Container>;
    seastar::future<> _future;

public:
    yield_awaiter(seastar::future<>&& f) noexcept
        : _future{std::move(f)} {}

    bool await_ready() noexcept {
        return _future.available();
    }

    coroutine_handle<> await_suspend(coroutine_handle<promise_type> coro) noexcept;
    void await_resume() noexcept { }
};


template<NothrowMoveConstructible T, template <typename> class Container>
requires Fifo<Container, T>
class generator_buffered_promise final : public seastar::task {
    using generator_type = seastar::coroutine::experimental::generator<T, Container>;

    std::optional<seastar::promise<>> _wait_for_next_value;
    std::optional<seastar::promise<>> _wait_for_free_space;
    generator_type* _generator = nullptr;
    const size_t _buffer_capacity;

public:
    template<typename... Args>
    generator_buffered_promise(buffer_size_t buffer_capacity, Args&&... args)
        : _buffer_capacity{static_cast<size_t>(buffer_capacity)} {}
    generator_buffered_promise(generator_buffered_promise&&) = delete;
    generator_buffered_promise(const generator_buffered_promise&) = delete;
    ~generator_buffered_promise() = default;
    void return_void() noexcept {
        if (_wait_for_next_value) {
            _wait_for_next_value->set_value();
            _wait_for_next_value = {};
        }
    }
    void unhandled_exception() noexcept;

    template<std::convertible_to<T> U>
    yield_awaiter<T, Container> yield_value(U&& value) noexcept {
        bool ready = _generator->put_next_value(std::forward<U>(value));

        if (_wait_for_next_value) {
            _wait_for_next_value->set_value();
            _wait_for_next_value = {};
            return {make_ready_future()};
        }
        if (ready) {
            return {make_ready_future()};
        } else {
            SEASTAR_ASSERT(!_wait_for_free_space);
            return {_wait_for_free_space.emplace().get_future()};
        }
    }

    auto get_return_object() noexcept -> generator_type;
    void set_generator(generator_type* g) noexcept {
        SEASTAR_ASSERT(!_generator);
        _generator = g;
    }

    suspend_always initial_suspend() const noexcept { return {}; }
    suspend_never final_suspend() const noexcept {
        SEASTAR_ASSERT(_generator);
        _generator->on_finished();
        return {};
    }

    bool is_awaiting() const noexcept {
        return _wait_for_next_value.has_value();
    }

    coroutine_handle<> coroutine() const noexcept {
        return coroutine_handle<>::from_address(_wait_for_next_value->waiting_task());
    }

    seastar::future<> wait_for_next_value() noexcept {
        SEASTAR_ASSERT(!_wait_for_next_value);
        return _wait_for_next_value.emplace().get_future();
    }

    void on_reclaim_free_space() noexcept {
        SEASTAR_ASSERT(_wait_for_free_space);
        _wait_for_free_space->set_value();
        _wait_for_free_space = {};
    }

private:
    void run_and_dispose() noexcept final {
        using handle_type = coroutine_handle<generator_buffered_promise>;
        handle_type::from_promise(*this).resume();
    }

    seastar::task* waiting_task() noexcept final {
        if (_wait_for_next_value) {
            return _wait_for_next_value->waiting_task();
        } else if (_wait_for_free_space) {
            return _wait_for_free_space->waiting_task();
        } else {
            return nullptr;
        }
    }
};

template<typename T, typename Generator>
struct next_awaiter final {
    using next_value_type = next_value_t<T>;
    Generator* const _generator;
    seastar::task* const _task;
    seastar::future<> _next_value_future;

public:
    next_awaiter(Generator* generator,
                 seastar::task* task,
                 seastar::future<>&& f) noexcept
        : _generator{generator}
        , _task(task)
        , _next_value_future(std::move(f)) {}

    next_awaiter(const next_awaiter&) = delete;
    next_awaiter(next_awaiter&&) = delete;

    constexpr bool await_ready() const noexcept {
        return _next_value_future.available() && !seastar::need_preempt();
    }

    template<typename Promise>
    void await_suspend(coroutine_handle<Promise> coro) noexcept {
        auto& current_task = coro.promise();
        if (_next_value_future.available()) {
            seastar::schedule(&current_task);
        } else {
            _next_value_future.set_coroutine(current_task);
            seastar::schedule(_task);
        }
    }

    next_value_type await_resume() {
        SEASTAR_ASSERT(_next_value_future.available());
        SEASTAR_ASSERT(_generator);
        return _generator->take_next_value();
    }
};

} // namespace internal

/// `seastar::coroutine::experimental::generator<T>` can be used to model a
/// generator which yields a sequence of values asynchronously.
///
/// Typically, it is used as the return type of a coroutine which is only
/// allowed to either `co_yield` the next element in the sequence, or
/// `co_return` nothing to indicate the end of this sequence. Please note,
/// despite that `tl::generator` defined in `tests/unit/tl-generator.hh` also
/// represents a generator, it can only produce the elements in a synchronous
/// coroutine which does not suspend itself because of any asynchronous
/// operation.
///
/// Example
///
/// ```
/// auto generate_request = [&input_stream](coroutine::experimental::buffer_size_t)
///     -> seastar::coroutine::generator<Request> {
///     while (!input_stream.eof()) {
///         co_yield co_await input_stream.read_exactly(42);
///     }
/// }
///
/// const coroutine::experimental::buffer_size_t backlog = 42;
/// while (true) {
///     auto request = co_await generate_request(backlog);
///     if (!request) {
///         break;
///     }
///     co_await process(*std::move(request));
/// }
/// ````
template <typename T, template <typename> class Container>
class generator {
public:
    using promise_type = internal::generator_buffered_promise<T, Container>;

private:
    using handle_type = internal::coroutine_handle<promise_type>;
    handle_type _coro;
    promise_type* _promise;
    Container<T> _values;
    const size_t _buffer_capacity;
    std::exception_ptr _exception;

public:
    generator(size_t buffer_capacity,
              handle_type coro,
              promise_type* promise) noexcept
        : _coro{coro}
        , _promise{promise}
        , _buffer_capacity{buffer_capacity} {
        SEASTAR_ASSERT(_promise);
        _promise->set_generator(this);
    }
    generator(const generator&) = delete;
    generator(generator&& other) noexcept
        : _coro{std::exchange(other._coro, {})}
        , _buffer_capacity{other._buffer_capacity} {}
    generator& operator=(generator&& other) noexcept {
        if (std::addressof(other) != this) {
            auto old_coro = std::exchange(_coro, std::exchange(other._coro, {}));
            if (old_coro) {
                old_coro.destroy();
            }
        }
        return *this;
    }
    ~generator() {
        if (_coro) {
            _coro.destroy();
        }
    }

    void swap(generator& other) noexcept {
        std::swap(_coro, other._coro);
    }

    internal::next_awaiter<T, generator> operator()() noexcept {
        if (!_values.empty()) {
            return {this, nullptr, make_ready_future<>()};
        } else if (_exception) [[unlikely]] {
            return {this, nullptr, make_ready_future<>()};
        } else if (_promise) {
            return {this, _promise, _promise->wait_for_next_value()};
        } else {
            return {this, nullptr, make_ready_future<>()};
        }
    }

    template<typename U>
    bool put_next_value(U&& value) {
        _values.push_back(std::forward<U>(value));
        return _values.size() < _buffer_capacity;
    }

    internal::next_value_t<T> take_next_value() {
        if (!_values.empty()) [[likely]] {
            auto value = std::move(_values.front());
            bool maybe_reclaim = _values.size() == _buffer_capacity;
            _values.pop_front();
            if (maybe_reclaim) {
                if (_promise) [[likely]] {
                    _promise->on_reclaim_free_space();
                }
            }
            return internal::next_value_t<T>(std::move(value));
        } else if (_exception) [[unlikely]] {
            std::rethrow_exception(std::exchange(_exception, nullptr));
        } else {
            return std::nullopt;
        }
    }

    void on_finished() {
        _promise = nullptr;
        _coro = nullptr;
    }

    void unhandled_exception() noexcept {
        // called by promise's unhandled_exception()
        SEASTAR_ASSERT(!_exception);
        _exception = std::current_exception();
    }
};

template <typename T>
class generator<T, std::optional> {
public:
    using promise_type = internal::generator_unbuffered_promise<T>;

private:
    using handle_type = internal::coroutine_handle<promise_type>;
    handle_type _coro;
    promise_type* _promise;
    std::optional<T> _maybe_value;
    std::exception_ptr _exception;

public:
    generator(handle_type coro,
              promise_type* promise) noexcept
        : _coro{coro}
        , _promise{promise} {
        SEASTAR_ASSERT(_promise);
        _promise->set_generator(this);
    }
    generator(const generator&) = delete;
    generator(generator&& other) noexcept
        : _coro{std::exchange(other._coro, {})} {}
    generator& operator=(generator&& other) noexcept {
        if (std::addressof(other) != this) {
            auto old_coro = std::exchange(_coro, std::exchange(other._coro, {}));
            if (old_coro) {
                old_coro.destroy();
            }
        }
        return *this;
    }
    ~generator() {
        if (_coro) {
            _coro.destroy();
        }
    }

    void swap(generator& other) noexcept {
        std::swap(_coro, other._coro);
    }

    internal::next_awaiter<T, generator> operator()() noexcept {
        if (_promise) [[likely]] {
            return {this, _promise, _promise->wait_for_next_value()};
        } else {
            return {this, nullptr, make_ready_future<>()};
        }
    }

    template<typename U>
    void put_next_value(U&& value) noexcept {
        _maybe_value.emplace(std::forward<U>(value));
    }

    internal::next_value_t<T> take_next_value() {
        if (_maybe_value.has_value()) [[likely]] {
            return std::exchange(_maybe_value, std::nullopt);
        } else if (_exception) [[unlikely]] {
            std::rethrow_exception(std::exchange(_exception, nullptr));
        } else {
            return std::nullopt;
        }
    }

    void on_finished() {
        _promise = nullptr;
        _coro = nullptr;
    }

    void unhandled_exception() noexcept {
        SEASTAR_ASSERT(!_exception);
        _exception = std::current_exception();
    }
};

namespace internal {

template<NothrowMoveConstructible T>
void generator_unbuffered_promise<T>::return_void() noexcept {
    SEASTAR_ASSERT(_wait_for_next_value);
    _wait_for_next_value->set_value();
    _wait_for_next_value = {};
}

template<NothrowMoveConstructible T>
void generator_unbuffered_promise<T>::unhandled_exception() noexcept {
    // instead of storing the current exception into promise, in order to be
    // more consistent, we let generator preserve all the output of produced
    // value, including the values and the exception if any. so we just signal
    // _wait_for_next_value, and delegate generator's unhandled_exception() to
    // store the exception.
    _generator->unhandled_exception();
    if (_wait_for_next_value.has_value()) {
        _wait_for_next_value->set_value();
        _wait_for_next_value = {};
    }
}

template<NothrowMoveConstructible T>
auto generator_unbuffered_promise<T>::get_return_object() noexcept -> generator_type {
    using handle_type = coroutine_handle<generator_unbuffered_promise<T>>;
    return generator_type{handle_type::from_promise(*this), this};
}

template<NothrowMoveConstructible T, template <typename> class Container>
requires Fifo<Container, T>
void generator_buffered_promise<T, Container>::unhandled_exception() noexcept {
    _generator->unhandled_exception();
    if (_wait_for_next_value.has_value()) {
        _wait_for_next_value->set_value();
        _wait_for_next_value = {};
    }
}

template<NothrowMoveConstructible T, template <typename> class Container>
requires Fifo<Container, T>
auto generator_buffered_promise<T, Container>::get_return_object() noexcept -> generator_type {
    using handle_type = coroutine_handle<generator_buffered_promise<T, Container>>;
    return generator_type{_buffer_capacity, handle_type::from_promise(*this), this};
}

template<typename T, template <typename> class Container>
coroutine_handle<> yield_awaiter<T, Container>::await_suspend(
    coroutine_handle<generator_buffered_promise<T, Container>> coro) noexcept {
    if (_future.available()) {
        auto& current_task = coro.promise();
        seastar::schedule(&current_task);
        return coro;
    } else {
        // we cannot do something like `task.set_coroutine(consumer_task)`.
        // because, instead of waiting for a subcoroutine, we are pending on
        // the caller of current coroutine to consume the produced values to
        // free up at least a free slot in the buffer, if we set the `_task`
        // of the of the awaiting task, we would have an infinite loop of
        // "promise->_task".
        return noop_coroutine();
    }
}

} // namespace internal
} // namespace seastar::coroutine::experimental

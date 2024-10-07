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

#include <cassert>
#include <coroutine>
#include <optional>
#include <utility>
#include <seastar/core/future.hh>

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
    enum class state {
        invalid,
        value,
        exception,
        done,
    };
    state _state = state::invalid;

    using next_value_type = next_value_t<T>;
    std::optional<seastar::promise<next_value_type>> _wait_for_next_value;

public:
    generator_unbuffered_promise() = default;
    generator_unbuffered_promise(generator_unbuffered_promise&&) = delete;
    generator_unbuffered_promise(const generator_unbuffered_promise&) = delete;

    void unhandled_exception() noexcept {
        assert(_state == state::invalid);
        assert(_wait_for_next_value);
        std::exchange(_wait_for_next_value, std::nullopt)->set_to_current_exception();
    }

    void return_void() noexcept {
        assert(_state == state::invalid);
        assert(_wait_for_next_value);
        std::exchange(_wait_for_next_value, std::nullopt)->set_value();
        _state = state::done;
    }

    template<std::convertible_to<T> U>
    suspend_always yield_value(U&& value) noexcept {
        assert(_wait_for_next_value);
        std::exchange(_wait_for_next_value, std::nullopt)->set_value(std::forward<U>(value));
        return {};
    }

    auto get_return_object() noexcept {
        using handle_type = coroutine_handle<generator_unbuffered_promise<T>>;
        using generator_type = generator<T, std::optional>;
        return generator_type{handle_type::from_promise(*this)};
    }

    // lazily-started coroutine, do not execute the coroutine until
    // the coroutine is awaited.
    suspend_always initial_suspend() const noexcept {
        return {};
    }
    suspend_always final_suspend() const noexcept {
        assert(_state != state::value);
        return {};
    }

    // next_awaiter connects to the this promise with the returned future
    seastar::future<next_value_type> wait_for_next_value() noexcept {
        // the coroutine is lazily-started, and we don't yield unless
        // the value is awaited, so the next value is never available
        // at this moment.
        assert(!_wait_for_next_value);
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
    const bool _wait_for_free_space;

public:
    yield_awaiter(bool wait_for_free_space) noexcept
        : _wait_for_free_space{wait_for_free_space} {}

    bool await_ready() noexcept {
        return _wait_for_free_space;
    }

    coroutine_handle<> await_suspend(coroutine_handle<promise_type>) noexcept {
        return std::noop_coroutine();
    }

    void await_resume() noexcept { }
};


template<NothrowMoveConstructible T, template <typename> class Container>
requires Fifo<Container, T>
class generator_buffered_promise final : public seastar::task {
    enum class state {
        invalid,
        value,
        exception,
        done,
    };
    state _state = state::invalid;
    using next_value_type = next_value_t<T>;
    std::optional<seastar::promise<next_value_type>> _wait_for_next_value;
    const size_t _buffer_capacity;
    Container<T> _values;
    std::exception_ptr _exception;

public:
    template<typename... Args>
    generator_buffered_promise(buffer_size_t buffer_capacity, Args&&... args)
        : _buffer_capacity{static_cast<size_t>(buffer_capacity)}
    {}
    generator_buffered_promise(generator_buffered_promise&&) = delete;
    generator_buffered_promise(const generator_buffered_promise&) = delete;

    void unhandled_exception() noexcept {
        assert(_state == state::invalid || _state == state::value);
        if (auto wait_for_next_value = std::exchange(_wait_for_next_value, std::nullopt)) {
            wait_for_next_value->set_to_current_exception();
        } else {
            assert(!_exception);
            _exception = std::current_exception();
            _state = state::exception;
        }
    }

    void return_void() noexcept {
        assert(_state == state::invalid || _state == state::value);
        if (auto wait_for_next_value = std::exchange(_wait_for_next_value, std::nullopt)) {
            wait_for_next_value->set_value();
        }
        _state = state::done;
    }

    template<std::convertible_to<T> U>
    yield_awaiter<T, Container> yield_value(U&& value) noexcept {
        assert(_state == state::invalid || _state == state::value);
        if (auto wait_for_next_value = std::exchange(_wait_for_next_value, std::nullopt)) {
            wait_for_next_value->set_value(std::forward<T>(value));
        } else {
            _values.push_back(std::forward<U>(value));
            _state = state::value;
        }
        return {_values.size() < _buffer_capacity};
    }

    auto get_return_object() noexcept {
        using handle_type = coroutine_handle<generator_buffered_promise<T, Container>>;
        using generator_type = generator<T, Container>;
        return generator_type{handle_type::from_promise(*this)};
    }

    // lazily-started coroutine, do not execute the coroutine until
    // the coroutine is awaited.
    suspend_always initial_suspend() const noexcept {
        return {};
    }

    suspend_always final_suspend() noexcept {
        assert(_state != state::value);
        return {};
    }

    seastar::future<next_value_type> wait_for_next_value() noexcept {
        switch (_state) {
        case state::invalid:
            assert(!_wait_for_next_value);
            return _wait_for_next_value.emplace().get_future();
        case state::done:
            // consume all values before returning the sentry
            if (_values.empty()) {
                return make_ready_future<next_value_type>(std::nullopt);
            }
            [[fallthrough]];
        case state::value: {
            auto next_value = std::move(_values.front());
            _values.pop_front();
            if (_state == state::value && _values.empty()) {
                _state = state::invalid;
            }
            return make_ready_future<next_value_type>(std::move(next_value));
        }
        case state::exception: {
            assert(_exception);
            // if there is an exception, let the consumer handle it before
            // processing the values if any
            auto next_value = std::exchange(_exception, nullptr);
            _state = _values.empty() ? state::invalid : state::value;
            return make_exception_future<next_value_type>(next_value);
        }
        default:
            __builtin_unreachable();
        }
    }

private:
    void run_and_dispose() noexcept final {
        using handle_type = coroutine_handle<generator_buffered_promise>;
        auto coro = handle_type::from_promise(*this);
        if (!coro.done()) {
            coro.resume();
        }
    }

    seastar::task* waiting_task() noexcept final {
        if (_wait_for_next_value) {
            return _wait_for_next_value->waiting_task();
        } else {
            return nullptr;
        }
    }
};

template<typename T, typename GeneratorPromise>
struct next_awaiter final {
    using next_value_type = next_value_t<T>;
    GeneratorPromise& _promise;
    seastar::future<next_value_type> _next_value_future;

public:
    next_awaiter(GeneratorPromise& promise) noexcept
        : _promise{promise}
        , _next_value_future(promise.wait_for_next_value())
    {}

    next_awaiter(const next_awaiter&) = delete;
    next_awaiter(next_awaiter&&) = delete;

    constexpr bool await_ready() const noexcept {
        // TODO: yield when being preempted
        return _next_value_future.available();
    }

    template<typename Promise>
    auto await_suspend(coroutine_handle<Promise> coro) noexcept {
        // i am suspended because the next value is not available yet,
        // let's resume the generator's coroutine
        assert(!_next_value_future.available());
        _next_value_future.set_coroutine(coro.promise());
        return coroutine_handle<GeneratorPromise>::from_promise(_promise);
    }

    next_value_type await_resume() {
        assert(_next_value_future.available());
        return _next_value_future.get0();
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
template <typename T, typename Promise>
class generator_base  {
private:
    using handle_type = internal::coroutine_handle<Promise>;
    handle_type _coro;

public:
    using promise_type = Promise;

    generator_base(handle_type coro) noexcept
        : _coro{coro}
    {}
    generator_base(const generator_base&) = delete;
    generator_base(generator_base&& other) noexcept
        : _coro{std::exchange(other._coro, {})}
    {}
    generator_base& operator=(generator_base&& other) noexcept {
        if (std::addressof(other) != this) {
            auto old_coro = std::exchange(_coro, std::exchange(other._coro, {}));
            if (old_coro) {
                old_coro.destroy();
            }
        }
        return *this;
    }
    ~generator_base() {
        if (_coro) {
            _coro.destroy();
        }
    }

    void swap(generator_base& other) noexcept {
        std::swap(_coro, other._coro);
    }

    internal::next_awaiter<T, Promise> operator()() noexcept {
        assert(_coro);
        return {_coro.promise()};
    }

};

template <typename T>
class generator<T, std::optional>
    : public generator_base<T, internal::generator_unbuffered_promise<T>>
{};

template <typename T, template <typename> class Container>
class generator
    : public generator_base<T, internal::generator_buffered_promise<T, Container>>
{};

} // namespace seastar::coroutine::experimental

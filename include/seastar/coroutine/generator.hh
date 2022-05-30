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
#include <optional>
#include <utility>
#include <seastar/core/future.hh>
#include <seastar/core/std-coroutine.hh>
#include <seastar/util/attribute-compat.hh>

namespace seastar::coroutine::experimental {

template<typename T>
class generator;

namespace internal {

template<typename T>
using next_value_t = std::optional<T>;

template<typename T>
class generator_promise final {
    using next_value_type = next_value_t<T>;
public:
    generator_promise() = default;
    generator_promise(generator_promise&&) = delete;
    generator_promise(const generator_promise&) = delete;

    void return_void() noexcept;
    void unhandled_exception() noexcept;

    template<std::convertible_to<T> U>
    SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_always yield_value(U&& value) noexcept {
        assert(_promise);
        _promise->set_value(std::forward<U>(value));
        _promise = {};
        return {};
    }

    generator<T> get_return_object() noexcept;
    void set_generator(generator<T>* g) noexcept {
        assert(!_generator);
        _generator = g;
    }

    SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_always initial_suspend() const noexcept { return {}; }
    SEASTAR_INTERNAL_COROUTINE_NAMESPACE::suspend_never final_suspend() const noexcept { return {}; }

    seastar::future<next_value_type> get_future() noexcept {
        assert(!_promise);
        return _promise.emplace().get_future();
    }

    seastar::task* get_waiting_task() noexcept {
        assert(_promise);
        return _promise->waiting_task();
    }

private:
    std::optional<seastar::promise<next_value_type>> _promise;
    generator<T>* _generator = nullptr;
};

template<typename T>
struct next_awaiter {
    using next_value_type = next_value_t<T>;

public:
    next_awaiter(seastar::task* task,
                 seastar::future<next_value_type>&& f) noexcept
        : _task(task)
        , _future(std::move(f)) {}

    next_awaiter(const next_awaiter&) = delete;
    next_awaiter(next_awaiter&&) = delete;

    constexpr bool await_ready() const noexcept {
        return _future.available() && !seastar::need_preempt();
    }

    template<typename Promise>
    void await_suspend(SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<Promise> coro) noexcept {
        auto& current_task = coro.promise();
        if (_future.available()) {
            seastar::schedule(&current_task);
        } else {
            _future.set_coroutine(current_task);
            seastar::schedule(_task);
        }
    }

    next_value_type await_resume() { return _future.get0(); }

private:
    seastar::task* const _task;
    seastar::future<next_value_type> _future;
};

} // namespace internal
/// `seastar::coroutine::generator<T>` can be used to model a generator which
/// yields a sequence of values asynchronously.
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
/// auto generate_request = [&input_stream]() -> seastar::coroutine::generator<Request> {
///     while (!input_stream.eof()) {
///         co_yield co_await input_stream.read_exactly(42);
///     }
/// }
///
/// while (true) {
///     auto request = co_await generate_request();
///     if (!request) {
///         break;
///     }
///     co_await process(*std::move(request));
/// }
/// ````
template <typename T>
class SEASTAR_NODISCARD generator : private seastar::task {
    using next_value_type = std::optional<T>;
public:
    using promise_type = internal::generator_promise<T>;
    using handle_type = SEASTAR_INTERNAL_COROUTINE_NAMESPACE::coroutine_handle<internal::generator_promise<T>>;

    explicit generator(handle_type coro, promise_type* promise) noexcept : _coro(coro) {
        assert(promise);
        promise->set_generator(this);
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

    internal::next_awaiter<T> operator()() noexcept {
        return {this, _coro.promise().get_future()};
    }

    void on_exit() {
        _coro = {};
    }

private:
    void run_and_dispose() noexcept final {
        _coro.resume();
    }

    seastar::task* waiting_task() noexcept final {
        return _coro.promise().get_waiting_task();
    }

    handle_type _coro;
};

namespace internal {

template<typename T>
void generator_promise<T>::return_void() noexcept {
    assert(_promise);
    _promise->set_value(std::nullopt);
    _promise = {};
    assert(_generator);
    _generator->on_exit();
}

template<typename T>
void generator_promise<T>:: unhandled_exception() noexcept {
    assert(_promise);
    _promise->set_exception(std::current_exception());
    _promise = {};
    assert(_generator);
    _generator->on_exit();
}

template<typename T>
generator<T> generator_promise<T>::get_return_object() noexcept {
    using handle_type = std::coroutine_handle<generator_promise<T>>;
    return generator<T>(handle_type::from_promise(*this), this);
}
}
} // namespace seastar::coroutine::experimental

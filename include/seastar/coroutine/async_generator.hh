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

#pragma once

#include <cassert>
#include <coroutine>
#include <exception>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <seastar/core/future.hh>

// async_generator concept is heavily inspired by P2502R2
// (https://wg21.link/P2502R2), a proposal accepted into C++23. P2502R2
// introduced std::generator, which provides a synchronous coroutine
// mechanism for generating ranges. in contrast, async_generator offers
// asynchronous generation of element sequences.
namespace seastar::coroutine::experimental {

template<typename Ref, typename Value = void>
class async_generator;

namespace internal {

template <typename Yielded> class next_awaiter;

template <typename Yielded>
class async_generator_promise_base : public seastar::task {
protected:
    std::add_pointer_t<Yielded> _value = nullptr;

protected:
    std::exception_ptr _exception;
    std::coroutine_handle<> _consumer;
    task* _waiting_task = nullptr;

    class yield_awaiter final {
        async_generator_promise_base* _promise;
        std::coroutine_handle<> _consumer;
    public:
        yield_awaiter(async_generator_promise_base* promise,
                      std::coroutine_handle<> consumer) noexcept
            : _promise{promise}
            , _consumer{consumer}
        {}
        bool await_ready() const noexcept {
            return false;
        }
        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> producer) noexcept {
            _promise->_waiting_task = &producer.promise();
            return _consumer;
        }
        void await_resume() noexcept {}
    };

    yield_awaiter do_yield() noexcept {
        return yield_awaiter{this, _consumer};
    }

public:
    async_generator_promise_base() noexcept = default;
    async_generator_promise_base(const async_generator_promise_base &) = delete;
    async_generator_promise_base& operator=(const async_generator_promise_base &) = delete;
    async_generator_promise_base(async_generator_promise_base &&) noexcept = default;
    async_generator_promise_base& operator=(async_generator_promise_base &&) noexcept = default;

    // lazily-started coroutine, do not execute the coroutine until
    // the coroutine is awaited.
    std::suspend_always initial_suspend() const noexcept {
        return {};
    }

    yield_awaiter final_suspend() noexcept {
        _value = nullptr;
        return do_yield();
    }

    void unhandled_exception() noexcept {
        _exception = std::current_exception();
    }

    void return_void() noexcept {}

    // @return if the generator has reached the end of the sequence
    bool finished() const noexcept {
        return _value == nullptr;
    }

    void rethrow_if_unhandled_exception() {
        if (_exception) {
            std::rethrow_exception(std::move(_exception));
        }
    }

    void run_and_dispose() noexcept final {
        using handle_type = std::coroutine_handle<async_generator_promise_base>;
        handle_type::from_promise(*this).resume();
    }

    seastar::task* waiting_task() noexcept final {
        return _waiting_task;
    }

    class element_awaiter {
        std::remove_cvref_t<Yielded> _value;
        constexpr bool await_ready() const noexcept {
            return false;
        }
        template <typename Promise>
        constexpr void await_suspend(std::coroutine_handle<Promise> producer) noexcept {
            auto& current = producer.promise();
            producer._value = std::addressof(_value);
        }
        constexpr void await_resume() const noexcept {}
    };

private:
    friend class next_awaiter<Yielded>;
};

template <typename Yielded>
class next_awaiter {
protected:
    async_generator_promise_base<Yielded>* _promise = nullptr;
    std::coroutine_handle<> _producer = nullptr;

    explicit next_awaiter(std::nullptr_t) noexcept {}
    next_awaiter(async_generator_promise_base<Yielded>& promise,
                 std::coroutine_handle<> producer) noexcept
        : _promise{std::addressof(promise)}
        , _producer{producer} {}

public:
    bool await_ready() const noexcept {
        return false;
    }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> consumer) noexcept {
        _promise->_consumer = consumer;
        return _producer;
    }
};

} // namespace internal

template<typename Ref, typename Value>
class [[nodiscard]] async_generator {
    using value_type = std::conditional_t<std::is_void_v<Value>,
                                          std::remove_cvref_t<Ref>,
                                          Value>;
    using reference_type = std::conditional_t<std::is_void_v<Value>,
                                              Ref&&,
                                              Ref>;
    using yielded_type = std::conditional_t<std::is_reference_v<reference_type>,
                                            reference_type,
                                            const reference_type&>;

public:
    class promise_type;

private:
    using handle_type = std::coroutine_handle<promise_type>;
    handle_type _coro = {};

public:
    class iterator;

    async_generator() noexcept = default;
    explicit async_generator(promise_type& promise) noexcept
        : _coro(std::coroutine_handle<promise_type>::from_promise(promise))
    {}
    async_generator(async_generator&& other) noexcept
        : _coro{std::exchange(other._coro, {})}
    {}
    async_generator(const async_generator&) = delete;
    async_generator& operator=(const async_generator &) = delete;

    ~async_generator() {
        if (_coro) {
            _coro.destroy();
        }
    }

    friend void swap(async_generator& lhs, async_generator& rhs) noexcept {
        std::swap(lhs._coro, rhs._coro);
    }

    async_generator& operator=(async_generator &&other) noexcept {
        if (_coro) {
            _coro.destroy();
        }
        _coro = std::exchange(other._coro, nullptr);
        return *this;
    }

    [[nodiscard]] auto begin() noexcept {
        using base_awaiter = internal::next_awaiter<yielded_type>;
        class begin_awaiter final : public base_awaiter {
            using base_awaiter::_promise;

        public:
            explicit begin_awaiter(std::nullptr_t) noexcept
                : base_awaiter{nullptr}
            {}
            explicit begin_awaiter(handle_type producer_coro) noexcept
                : base_awaiter{producer_coro.promise(), producer_coro}
            {}
            bool await_ready() const noexcept {
                return _promise == nullptr || base_awaiter::await_ready();
            }

            iterator await_resume() {
                if (_promise == nullptr) {
                    return iterator{nullptr};
                }
                if (_promise->finished()) {
                    _promise->rethrow_if_unhandled_exception();
                    return iterator{nullptr};
                }
                return iterator{
                    handle_type::from_promise(*static_cast<promise_type *>(_promise))
                };
            }
        };

        if (_coro) {
            return begin_awaiter{_coro};
        } else {
            return begin_awaiter{nullptr};
        }
    }

    [[nodiscard]] std::default_sentinel_t end() const noexcept {
        return {};
    }
};

template <typename Ref, typename Value>
class async_generator<Ref, Value>::promise_type final : public internal::async_generator_promise_base<yielded_type> {
    using yield_awaiter = internal::async_generator_promise_base<yielded_type>::yield_awaiter;
    using element_awaiter = internal::async_generator_promise_base<yielded_type>::element_awaiter;
    using internal::async_generator_promise_base<yielded_type>::_value;
    using internal::async_generator_promise_base<yielded_type>::_exception;

public:
    async_generator get_return_object() noexcept {
        return async_generator{*this};
    }

    // lazily-started coroutine, do not execute the coroutine until
    // the coroutine is awaited.
    std::suspend_always initial_suspend() const noexcept {
        return {};
    }
    yield_awaiter final_suspend() noexcept {
        _value = nullptr;
        return this->do_yield();
    }

    yield_awaiter yield_value(yielded_type value) noexcept {
        _value = std::addressof(value);
        return this->do_yield();
    }

    element_awaiter yield_value(const std::remove_reference_t<yielded_type>& value)
    requires (std::is_rvalue_reference_v<yielded_type> &&
              std::constructible_from<
                  std::remove_cvref_t<yielded_type>,
                  const std::remove_reference_t<yielded_type>&>) {
        return element_awaiter{value};
    }

    yielded_type value() const noexcept {
        return static_cast<yielded_type>(*_value);
    }

    void unhandled_exception() noexcept {
        _exception = std::current_exception();
    }

    void return_void() noexcept {}

    // @return if the generator has reached the end of the sequence
    bool finished() const noexcept {
        return _value == nullptr;
    }
};

template <typename Ref, typename Value>
class async_generator<Ref, Value>::iterator final {
private:
    using handle_type = async_generator::handle_type;
    handle_type _coro = nullptr;

public:
    using iterator_category = std::input_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = async_generator::value_type;
    using reference = async_generator::reference_type;
    using pointer = std::add_pointer_t<value_type>;

    explicit iterator(handle_type coroutine) noexcept
        : _coro{coroutine}
    {}

    explicit operator bool() const noexcept {
        return _coro && !_coro.done();
    }

    [[nodiscard]] auto operator++() noexcept {
        using base_awaiter = internal::next_awaiter<yielded_type>;
        class increment_awaiter final : public base_awaiter {
            iterator& _iterator;
            using base_awaiter::_promise;

        public:
            explicit increment_awaiter(iterator& iterator) noexcept
                : base_awaiter{iterator._coro.promise(), iterator._coro}
                , _iterator{iterator}
            {}
            iterator& await_resume() {
                if (_promise->finished()) {
                    // update iterator to end()
                    _iterator = iterator{nullptr};
                    _promise->rethrow_if_unhandled_exception();
                }
                return _iterator;
            }
        };

        assert(bool(*this) && "cannot increment end iterator");
        return increment_awaiter{*this};
    }

    reference operator*() const noexcept {
        return _coro.promise().value();
    }

    bool operator==(std::default_sentinel_t) const noexcept {
        return !bool(*this);
    }
};

} // namespace seastar::coroutine::experimental

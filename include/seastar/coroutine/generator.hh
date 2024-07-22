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
#include <exception>
#include <iterator>
#include <memory>
#include <type_traits>
#include <utility>
#include <seastar/core/future.hh>

// seastar::coroutine::generator concept is heavily inspired by P2502R2
// (https://wg21.link/P2502R2), a proposal accepted into C++23. P2502R2
// introduced std::generator, which provides a synchronous coroutine
// mechanism for generating ranges. in contrast, seastar::coroutine::generator offers
// asynchronous generation of element sequences.
namespace seastar::coroutine::experimental {

template<typename Ref, typename Value = void>
class generator;

namespace internal {

template <typename Yielded> class next_awaiter;

template <typename Yielded>
class generator_promise_base : public seastar::task {
protected:
    std::add_pointer_t<Yielded> _value = nullptr;

protected:
    std::exception_ptr _exception;
    std::coroutine_handle<> _consumer;
    task* _waiting_task = nullptr;

    class yield_awaiter final {
        generator_promise_base* _promise;
        std::coroutine_handle<> _consumer;
    public:
        yield_awaiter(generator_promise_base* promise,
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

    class copy_awaiter {
        using value_type = std::remove_cvref_t<Yielded>;
        generator_promise_base* _promise;
        std::coroutine_handle<> _consumer;
        value_type _value;
    public:
        copy_awaiter(generator_promise_base* promise,
                     std::coroutine_handle<> consumer,
                     const value_type& value)
            : _promise{promise}
            , _consumer{consumer}
            , _value{value}
        {}
        constexpr bool await_ready() const noexcept {
            return false;
        }
        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> producer) noexcept {
            auto& current = producer.promise();
            current._value = std::addressof(_value);
            _promise->_waiting_task = &current;
            return _consumer;
        }
        constexpr void await_resume() const noexcept {}
    };

public:
    generator_promise_base() noexcept = default;
    generator_promise_base(const generator_promise_base &) = delete;
    generator_promise_base& operator=(const generator_promise_base &) = delete;
    generator_promise_base(generator_promise_base &&) noexcept = default;
    generator_promise_base& operator=(generator_promise_base &&) noexcept = default;

    // lazily-started coroutine, do not execute the coroutine until
    // the coroutine is awaited.
    std::suspend_always initial_suspend() const noexcept {
        return {};
    }

    yield_awaiter final_suspend() noexcept {
        _value = nullptr;
        return yield_awaiter{this, this->_consumer};
    }

    void unhandled_exception() noexcept {
        _exception = std::current_exception();
    }

    yield_awaiter yield_value(Yielded value) noexcept {
        this->_value = std::addressof(value);
        return yield_awaiter{this, this->_consumer};
    }

    copy_awaiter yield_value(const std::remove_reference_t<Yielded>& value)
    requires (std::is_rvalue_reference_v<Yielded> &&
              std::constructible_from<
                  std::remove_cvref_t<Yielded>,
                  const std::remove_reference_t<Yielded>&>) {
        return copy_awaiter{this, this->_consumer, value};
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
        using handle_type = std::coroutine_handle<generator_promise_base>;
        handle_type::from_promise(*this).resume();
    }

    seastar::task* waiting_task() noexcept final {
        return _waiting_task;
    }

private:
    friend class next_awaiter<Yielded>;
};

template <typename Yielded>
class next_awaiter {
protected:
    generator_promise_base<Yielded>* _promise = nullptr;
    std::coroutine_handle<> _producer = nullptr;

    explicit next_awaiter(std::nullptr_t) noexcept {}
    next_awaiter(generator_promise_base<Yielded>& promise,
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

/// generator represents a view modeling std::ranges::input_range,
/// and has move-only iterators.
///
/// generator has 2 template parameters:
///
/// - Ref
/// - Value
///
/// From Ref and Value, we derive types:
/// - value_type: a cv-unqualified object type that specifies the value type of
///   the generator's range and iterators
/// - reference_Type: the reference type of the generator's range and iterators
/// - yielded_type: the type of the parameter to the primary overload of \c
///   yield_value in the generator's associated promise type
///
/// Under the most circumstances, only the first parameter is specified: like
/// \c generator<meow>. The resulting generator:
/// - has a value type of \c remove_cvref_t<meow>
/// - has a reference type of \c meow, if it is a reference type, or \c meow&&
///   otherwise
/// - the operand of \c co_yield in the body of the generator should be
///   convertible to \c meow, if it is a reference type, otherwise the operand
///   type should be <tt>const meow&</tt>
///
/// Consider following code snippet:
/// \code
/// generator<const std::string&> send_query(std::string query) {
///   auto result_set = db.execute(query);
///   for (auto row : result_set) {
///       co_yield std::format("{}", row);
///   }
/// }
/// \endcode
///
/// In this case, \c Ref is a reference type of \c <tt>const std::string&</tt>,
/// and \c Value is the default value of \c void. So the \c value_type is
/// \c std::string. As the generator always returns a \c std::string, its
/// iterator has the luxury of returning a reference to it.
///
/// But if some rare users want to use a proxy reference type, or to generate a
/// range whose iterators yield prvalue for whatever reason, they should use
/// the two-argument \c generator, like <tt>generator<meow, woof></tt>.
/// The resulting generator:
/// - has a value type of \c woof
/// - has a reference type of \c meow
///
/// For instance, consider following code snippet:
/// \code
/// generator<std::string_view, std::string> generate_strings() {
///   co_yield "[";
///   std::string s;
///   for (auto sv : {"1"sv, "2"sv}) {
///     s = sv;
///     s.push_back(',');
///     co_yield s;
///   }
///   co_yield "]";
/// }
/// \endcode
///
/// In this case, \c Ref is \c std::string_view, and \Value is \c std::string.
/// So we can ensure that the caller cannot invalidate the yielded values by
/// mutating the defererenced value of iterator. As the \c std::string_view
/// instance is immutable. But in the meanwhile, the generator can return
/// a \c std::string by \c co_yield a \c std::string_view or a \c std::string.
/// And the caller can still access the element of the range via the same type:
/// \c std::string_view.

template<typename Ref, typename Value>
class [[nodiscard]] generator {
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

    generator() noexcept = default;
    explicit generator(promise_type& promise) noexcept
        : _coro(std::coroutine_handle<promise_type>::from_promise(promise))
    {}
    generator(generator&& other) noexcept
        : _coro{std::exchange(other._coro, {})}
    {}
    generator(const generator&) = delete;
    generator& operator=(const generator&) = delete;

    ~generator() {
        if (_coro) {
            _coro.destroy();
        }
    }

    friend void swap(generator& lhs, generator& rhs) noexcept {
        std::swap(lhs._coro, rhs._coro);
    }

    generator& operator=(generator&& other) noexcept {
        if (this == &other) {
            return *this;
        }
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
class generator<Ref, Value>::promise_type final : public internal::generator_promise_base<yielded_type> {
public:
    generator get_return_object() noexcept {
        return generator{*this};
    }

    yielded_type value() const noexcept {
        return static_cast<yielded_type>(*this->_value);
    }
};

template <typename Ref, typename Value>
class generator<Ref, Value>::iterator final {
private:
    using handle_type = generator::handle_type;
    handle_type _coro = nullptr;

public:
    using iterator_category = std::input_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = generator::value_type;
    using reference = generator::reference_type;
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

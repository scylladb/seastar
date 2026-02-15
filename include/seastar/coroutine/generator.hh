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
 * Copyright (C) 2025 Kefu Chai ( tchaikov@gmail.com )
 */

#pragma once

#include <coroutine>
#include <exception>
#include <iterator>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <seastar/core/future.hh>
#include <seastar/util/assert.hh>

// seastar::coroutine::generator is inspired by the C++23 proposal
// P2502R2 (https://wg21.link/P2502R2), which introduced std::generator for
// synchronous coroutine-based range generation.
//
// Similar to P2502R2's generator, seastar::coroutine::experimental::generator
// prioritizes storing references to yielded objects instead of copying them.
//
// However, there are key differences in seastar::coroutine::experimental::generator:
//
// * Allocator support:
//   Seastar's generator does not support the Allocator template parameter.
//   Seastar uses its built-in allocator eliminating the need for
//   additional flexibility.
// * Asynchronous Operations:
//   - generator::operator() is a coroutine that returns std::optional<reference_type>
//   - Unlike P2502R2's synchronous iterator-based approach, Seastar's generator
//     uses a simpler function-call API
//   Note: Due to its asynchronous nature, this generator cannot be used in
//   range-based for loops. Instead, use: while (auto val = co_await gen()) { ... }
// * Ranges Integration:
//   Seastar's generator is not a std::ranges::view_interface. It lacks
//   integration with the C++20 Ranges library due to its asynchronous operations.
// * Nesting:
//   Nesting generators is not supported. You cannot yield another generator
//   from within a generator. This prevents implementation of asynchronous,
//   recursive algorithms like depth-first search on trees.
// * Range Yielding:
//   The buffered variant supports yielding both individual elements and ranges/slices.
//   This provides flexibility to yield data in whatever form is most convenient for
//   the producer, while the generator handles efficient batching internally.
namespace seastar::coroutine::experimental {

namespace internal {

namespace unbuffered {

template <typename Yielded> class next_awaiter;

template <typename Yielded>
class generator_promise_base : public seastar::task {
    using yielded_deref_type = std::remove_reference_t<Yielded>;
    using yielded_decvref_type = std::remove_cvref_t<Yielded>;
    using value_ptr_type = std::add_pointer_t<Yielded>;

protected:
    // a glvalue yield expression is passed to co_yield as its operand. and
    // the object denoted by this expression is guaranteed to live until the
    // coroutine resumes. we take advantage of this fact by storing only a
    // pointer to the denoted object in the promise as long as the result of
    // dereferencing this pointer is convertible to the Ref type.
    std::add_pointer_t<Yielded> _value = nullptr;

protected:
    std::exception_ptr _exception;
    std::coroutine_handle<> _consumer;
    task* _waiting_task = nullptr;

    /// awaiter returned by the generator when it produces a new element
    ///
    /// There are different combinations of expression types passed to
    /// \c co_yield and \c Ref. In most cases, zero copies are made. Copies
    /// are only necessary when \c co_yield requires type conversion.
    ///
    /// The following table summarizes the number of copies made for different
    /// scenarios:
    ///
    /// | Ref       | co_yield const T& | co_yield T& | co_yield T&& | co_yield U&& |
    /// | --------- | ----------------- | ----------- | ------------ | ------------ |
    /// | T         | 0                 | 0           | 0            | 1            |
    /// | const T&  | 0                 | 0           | 0            | 1            |
    /// | T&        | ill-formed        | 0           | ill-formed   | ill-formed   |
    /// | T&&       | ill-formed        | ill-formed  | 0            | 1            |
    /// | const T&& | ill-formed        | ill-formed  | 0            | 1            |
    ///
    /// When no copies are required, \c yield_awaiter is used. Otherwise,
    /// \c copy_awaiter is used. The latter converts \c U to \c T, and keeps the converted
    /// value in it.
    struct yield_awaiter;
    struct copy_awaiter;

public:
    generator_promise_base() noexcept = default;
    generator_promise_base(const generator_promise_base &) = delete;
    generator_promise_base& operator=(const generator_promise_base &) = delete;
    generator_promise_base(generator_promise_base &&) noexcept = default;
    generator_promise_base& operator=(generator_promise_base &&) noexcept = default;
    virtual ~generator_promise_base() = default;

    // lazily-started coroutine, do not execute the coroutine until
    // the coroutine is awaited.
    std::suspend_always initial_suspend() const noexcept {
        return {};
    }

    yield_awaiter final_suspend() noexcept {
        _value = nullptr;
        return {this, this->_consumer};
    }

    void unhandled_exception() noexcept {
        _exception = std::current_exception();
    }

    yield_awaiter yield_value(Yielded&& value) noexcept {
        this->_value = std::addressof(value);
        return {this, this->_consumer};
    }

    copy_awaiter yield_value(const yielded_deref_type& value)
        noexcept (std::is_nothrow_constructible_v<
                    yielded_decvref_type,
                    const yielded_deref_type&>)
        requires (std::is_rvalue_reference_v<Yielded> &&
                  std::constructible_from<
                    yielded_decvref_type,
                    const yielded_deref_type&>) {
        return {this, this->_consumer, yielded_decvref_type(value), _value};
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
    template<typename, typename> friend class generator;
    friend class next_awaiter<Yielded>;
};

template <typename Yielded>
struct generator_promise_base<Yielded>::yield_awaiter final {
    generator_promise_base* _promise;
    std::coroutine_handle<> _consumer;

    bool await_ready() const noexcept {
        return false;
    }
    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> producer SEASTAR_COROUTINE_LOC_PARAM) noexcept {
        SEASTAR_COROUTINE_LOC_STORE(producer.promise());
        _promise->_waiting_task = &producer.promise();
        if (seastar::need_preempt()) {
            auto consumer = std::coroutine_handle<seastar::task>::from_address(
                _consumer.address());
            seastar::schedule(&consumer.promise());
            return std::noop_coroutine();
        }
        return _consumer;
    }
    void await_resume() noexcept {}
};

template <typename Yielded>
struct generator_promise_base<Yielded>::copy_awaiter final {
    generator_promise_base* _promise;
    std::coroutine_handle<> _consumer;
    yielded_decvref_type _value;
    value_ptr_type& _value_ptr;

    constexpr bool await_ready() const noexcept {
        return false;
    }
    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> producer SEASTAR_COROUTINE_LOC_PARAM) noexcept {
        SEASTAR_COROUTINE_LOC_STORE(producer.promise());
        _value_ptr = std::addressof(_value);
        auto& current = producer.promise();
        _promise->_waiting_task = &current;
        if (seastar::need_preempt()) {
            auto consumer = std::coroutine_handle<seastar::task>::from_address(
                _consumer.address());
            seastar::schedule(&consumer.promise());
            return std::noop_coroutine();
        }
        return _consumer;
    }
    constexpr void await_resume() const noexcept {}
};

/// awaiter returned when the consumer calls \c operator() to get the next value.
template <typename Yielded>
class [[nodiscard]] next_awaiter {
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
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> consumer SEASTAR_COROUTINE_LOC_PARAM) noexcept {
        SEASTAR_COROUTINE_LOC_STORE(consumer.promise());
        _promise->_consumer = consumer;
        // Check if we need to preempt. If not, directly resume producer.
        // If yes, schedule the producer through the scheduler.
        if (!seastar::need_preempt()) {
            return _producer;
        }
        auto producer_handle = std::coroutine_handle<seastar::task>::from_address(
            _producer.address());
        seastar::schedule(&producer_handle.promise());
        return std::noop_coroutine();
    }

    void await_resume() noexcept {}
};

/// unbuffered generator provides a simple async API for generating values.
///
/// generator has 2 template parameters:
///
/// - Ref
/// - Value
///
/// From Ref and Value, we derive types:
/// - value_type: a cv-unqualified object type that specifies the value type
/// - reference_type: the reference type returned by \c operator()
/// - yielded_type: the type of the parameter to the primary overload of \c
///   yield_value in the generator's associated promise type
///
/// Under the most circumstances, only the first parameter is specified: like
/// \c generator<meow>. The resulting generator:
/// - has a value type of \c remove_cvref_t<meow>
/// - has a reference type of \c meow, if it is a reference type, or \c meow&&
///   otherwise
/// - the operand of \c co_yield in the body of the generator should be
///   convertible to \c meow when \c meow is a reference type; when \c meow
///   is not a reference type, the operand type should be <tt>const meow&</tt>
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
/// \c std::string. The generator returns a reference to the yielded string.
///
/// But if some rare users want to use a proxy reference type, they should use
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
/// In this case, \c Ref is \c std::string_view, and \c Value is \c std::string.
/// So we can ensure that the caller cannot invalidate the yielded values by
/// mutating the returned value, as \c std::string_view is immutable. But in
/// the meanwhile, the generator can \c co_yield either a \c std::string_view
/// or a \c std::string. The caller receives values as \c std::string_view.
///
/// This unbuffered generator implementation minimizes ping-pong overhead by:
/// 1. Avoiding the scheduler when preemption is not needed - directly transferring
///    control between producer and consumer coroutine handles
/// 2. Only going through the scheduler when \c need_preempt() indicates it's time
///    to yield to other tasks
/// 3. Yielding elements in-place via pointer - zero copy/move overhead
///
/// However, the unbuffered design inherently requires one coroutine suspension/resumption
/// pair per yielded value (producer suspends on co_yield, consumer suspends on co_await).
/// While direct handle transfer avoids scheduler overhead, the suspension/resumption itself
/// still impacts icache and branch prediction in high-throughput scenarios.
///
/// Performance tradeoffs:
/// - Use unbuffered generator for latency-sensitive code or when element moves are expensive
/// - Use buffered generator (third template parameter) for throughput-critical code where
///   move cost is acceptable
///
/// The buffered generator amortizes suspension overhead by batching: elements are moved into
/// an internal buffer until it's full or need_preempt() returns true. The consumer then drains
/// the buffer without coroutine suspensions (tight loop). This trades per-element move overhead
/// for reduced suspension overhead, similar to how flat_mutation_reader batches mutations.
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
    // optional_type is used to return values: for references, wrap in reference_wrapper
    using optional_type = std::conditional_t<std::is_reference_v<reference_type>,
                                             std::optional<std::reference_wrapper<std::remove_reference_t<reference_type>>>,
                                             std::optional<reference_type>>;

public:
    class promise_type;

private:
    using handle_type = std::coroutine_handle<promise_type>;
    handle_type _coro = {};
    bool _started = false;

public:
    generator() noexcept = default;
    explicit generator(promise_type& promise) noexcept
        : _coro(std::coroutine_handle<promise_type>::from_promise(promise))
    {}
    generator(generator&& other) noexcept
        : _coro{std::exchange(other._coro, {})}
        , _started{std::exchange(other._started, false)}
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
        std::swap(lhs._started, rhs._started);
    }

    generator& operator=(generator&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (_coro) {
            _coro.destroy();
        }
        _coro = std::exchange(other._coro, nullptr);
        _started = std::exchange(other._started, false);
        return *this;
    }

    /// Get the next value from the generator.
    /// Returns std::optional containing the next value, or std::nullopt if done.
    /// For reference types, returns std::optional<std::reference_wrapper<T>>.
    ///
    /// Example usage:
    /// \code
    /// auto gen = my_generator();
    /// while (auto value = co_await gen()) {
    ///     process(value->get());  // for references
    ///     process(*value);        // for values
    /// }
    /// \endcode
    class [[nodiscard]] call_awaiter {
        generator* _gen;

    public:
        explicit call_awaiter(generator* gen) noexcept
            : _gen(gen)
        {}

        bool await_ready() const noexcept {
            return !_gen->_coro || _gen->_coro.done();
        }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> consumer SEASTAR_COROUTINE_LOC_PARAM) noexcept {
            SEASTAR_COROUTINE_LOC_STORE(consumer.promise());
            auto& promise = _gen->_coro.promise();
            promise._consumer = consumer;
            // Check if we need to preempt. If not, directly resume producer.
            // If yes, schedule the producer through the scheduler.
            if (!seastar::need_preempt()) {
                return _gen->_coro;
            }
            auto producer_handle = std::coroutine_handle<seastar::task>::from_address(
                _gen->_coro.address());
            seastar::schedule(&producer_handle.promise());
            return std::noop_coroutine();
        }

        optional_type await_resume() {
            if (!_gen->_coro) {
                return std::nullopt;
            }

            if (!_gen->_started) {
                _gen->_started = true;
            }

            auto& promise = _gen->_coro.promise();

            // Check for exceptions first, even if coroutine is done
            if (promise.finished() || _gen->_coro.done()) {
                promise.rethrow_if_unhandled_exception();
                return std::nullopt;
            }

            if constexpr (std::is_reference_v<reference_type>) {
                // promise.value() returns a reference, convert to lvalue ref
                auto&& val = promise.value();
                return std::reference_wrapper<std::remove_reference_t<reference_type>>(val);
            } else {
                return promise.value();
            }
        }
    };

    [[nodiscard]] call_awaiter operator()() noexcept {
        return call_awaiter{this};
    }
};

template <typename Ref, typename Value>
class generator<Ref, Value>::promise_type final : public generator_promise_base<yielded_type> {
public:
    generator get_return_object() noexcept {
        return generator{*this};
    }

    yielded_type value() const noexcept {
        return static_cast<yielded_type>(*this->_value);
    }
};

} // namespace unbuffered

namespace buffered::detail {

// Customization point object for checking if more elements can be pushed to the buffer
//
// This CPO follows the C++20 ranges library pattern and provides a flexible
// customization mechanism with the following priority:
//
// 1. Member function: container.can_push_more()
//    Use this if your container has internal state for measuring capacity
//    (e.g., memory usage tracking)
//
// 2. ADL-found free function: can_push_more(container)
//    Use this for non-intrusive customization of third-party containers
//
// 3. Default implementation: container.size() < container.capacity()
//    Element count-based measurement for standard containers
//
// Example member function customization:
//   struct memory_aware_buffer {
//       bool can_push_more() const { return memory_used < memory_limit; }
//   };
//
// Example ADL customization:
//   namespace my_ns {
//       struct my_container { ... };
//       bool can_push_more(const my_container& c) { return ...; }
//   }
struct can_push_more_fn {
    template <typename Container>
    constexpr bool operator()(const Container& container) const {
        // Priority 1: Member function
        if constexpr (requires { { container.can_push_more() } -> std::convertible_to<bool>; }) {
            return container.can_push_more();
        }
        // Priority 2: ADL-found free function
        else if constexpr (requires { { can_push_more(container) } -> std::convertible_to<bool>; }) {
            return can_push_more(container);
        }
        // Priority 3: Default implementation
        else {
            return container.size() < container.capacity();
        }
    }
};

} // namespace buffered::detail

namespace buffered {

/// Customization point object for buffer capacity checking
inline constexpr detail::can_push_more_fn can_push_more{};

/// Concept for bounded containers suitable for buffering generator elements
template <typename T>
concept bounded_container = requires(T container, typename T::value_type element) {
    // Must have value_type
    typename T::value_type;

    // Must support capacity queries
    { container.capacity() } -> std::convertible_to<size_t>;
    { container.size() } -> std::convertible_to<size_t>;

    // Must support element addition and removal
    { container.push_back(std::move(element)) } -> std::same_as<void>;
    { container.clear() } -> std::same_as<void>;

    // Must support indexed access
    { container[size_t()] } -> std::convertible_to<typename T::value_type&>;
};

template <bounded_container Container> class next_awaiter;

template <bounded_container Container>
class generator_promise_base : public seastar::task {
    using element_type = typename Container::value_type;

protected:
    // buffer holds elements yielded by the producer until consumed
    Container _buffer;
    bool _finished = false;

protected:
    std::exception_ptr _exception;
    std::coroutine_handle<> _consumer;
    task* _waiting_task = nullptr;

    struct yield_awaiter;

public:
    generator_promise_base() noexcept = default;
    generator_promise_base(const generator_promise_base &) = delete;
    generator_promise_base& operator=(const generator_promise_base &) = delete;
    generator_promise_base(generator_promise_base &&) noexcept = default;
    generator_promise_base& operator=(generator_promise_base &&) noexcept = default;
    virtual ~generator_promise_base() = default;

    // lazily-started coroutine, do not execute the coroutine until
    // the coroutine is awaited.
    std::suspend_always initial_suspend() const noexcept {
        return {};
    }

    yield_awaiter final_suspend() noexcept {
        _finished = true;
        return yield_awaiter{this, this->_consumer, true};
    }

    void unhandled_exception() noexcept {
        _exception = std::current_exception();
    }

    // Yield a single element
    yield_awaiter yield_value(element_type element) {
        _buffer.push_back(std::move(element));

        // Should we suspend and let consumer drain the buffer?
        // Suspend if: buffer is full OR we need to yield to other tasks
        bool should_suspend = !can_push_more(_buffer) || seastar::need_preempt();
        return yield_awaiter{this, this->_consumer, should_suspend};
    }

    // Yield a range/slice of elements (C++23 ranges support)
    // IMPORTANT: The entire range must fit in the buffer. If the range is larger
    // than the buffer capacity, elements will be lost when the buffer overflows.
    // For large datasets, yield elements individually instead of as a range.
    template <std::ranges::range Range>
    requires std::convertible_to<std::ranges::range_value_t<Range>, element_type>
    yield_awaiter yield_value(Range&& range) {
        // Add all elements from the range to the buffer
        // Note: We cannot break mid-range because rvalue ranges would be destroyed,
        // losing the remaining elements. The buffer must have sufficient capacity.
        for (auto&& element : range) {
            // Move from rvalue ranges, copy/move from lvalue ranges based on element type
            if constexpr (std::is_rvalue_reference_v<decltype(range)>) {
                _buffer.push_back(std::move(element));
            } else {
                _buffer.push_back(std::forward<decltype(element)>(element));
            }
        }

        // All elements added, check if we should suspend
        bool should_suspend = !can_push_more(_buffer) || seastar::need_preempt();
        return yield_awaiter{this, this->_consumer, should_suspend};
    }

    void return_void() noexcept {}

    // @return if the generator has reached the end of the sequence
    bool finished() const noexcept {
        return _finished && _buffer.empty();
    }

    // @return the buffer containing accumulated elements
    Container& buffer() noexcept {
        return _buffer;
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
    template<typename, typename, bounded_container> friend class generator;
    friend class next_awaiter<Container>;
};

template <bounded_container Container>
struct generator_promise_base<Container>::yield_awaiter final {
    generator_promise_base* _promise;
    std::coroutine_handle<> _consumer;
    bool _should_suspend;
public:
    yield_awaiter(generator_promise_base* promise,
                  std::coroutine_handle<> consumer,
                  bool should_suspend) noexcept
        : _promise{promise}
        , _consumer{consumer}
        , _should_suspend{should_suspend}
    {}
    bool await_ready() const noexcept {
        return !_should_suspend;  // If we shouldn't suspend, we're ready immediately
    }
    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> producer SEASTAR_COROUTINE_LOC_PARAM) noexcept {
        SEASTAR_COROUTINE_LOC_STORE(producer.promise());
        _promise->_waiting_task = &producer.promise();
        if (seastar::need_preempt()) {
            auto consumer = std::coroutine_handle<seastar::task>::from_address(
                _consumer.address());
            seastar::schedule(&consumer.promise());
            return std::noop_coroutine();
        }
        return _consumer;
    }
    void await_resume() noexcept {}
};

template <bounded_container Container>
class [[nodiscard]] next_awaiter {
protected:
    generator_promise_base<Container>* _promise = nullptr;
    std::coroutine_handle<> _producer = nullptr;

public:
    explicit next_awaiter(std::nullptr_t) noexcept {}
    next_awaiter(generator_promise_base<Container>& promise,
                 std::coroutine_handle<> producer) noexcept
        : _promise{std::addressof(promise)}
        , _producer{producer} {}

    bool await_ready() const noexcept {
        return false;
    }

    template <typename Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> consumer SEASTAR_COROUTINE_LOC_PARAM) noexcept {
        SEASTAR_COROUTINE_LOC_STORE(consumer.promise());
        _promise->_consumer = consumer;
        // Check if we need to preempt. If not, directly resume producer.
        // If yes, schedule the producer through the scheduler.
        if (!seastar::need_preempt()) {
            return _producer;
        }
        auto producer_handle = std::coroutine_handle<seastar::task>::from_address(
            _producer.address());
        seastar::schedule(&producer_handle.promise());
        return std::noop_coroutine();
    }

    void await_resume() noexcept {}
};

template<typename Ref, typename Value, bounded_container Container>
class [[nodiscard]] generator {
    using value_type = std::conditional_t<std::is_void_v<Value>,
                                          std::remove_cvref_t<Ref>,
                                          Value>;
    using reference_type = std::conditional_t<std::is_void_v<Value>,
                                              Ref&&,
                                              Ref>;
    using container_type = Container;
    // optional_type for returning values
    using optional_type = std::conditional_t<std::is_reference_v<reference_type>,
                                             std::optional<std::reference_wrapper<std::remove_reference_t<reference_type>>>,
                                             std::optional<reference_type>>;

public:
    class promise_type;

private:
    using handle_type = std::coroutine_handle<promise_type>;
    handle_type _coro = {};
    bool _started = false;
    size_t _buffer_index = 0;  // Current position in the buffer

public:
    generator() noexcept = default;
    explicit generator(promise_type& promise) noexcept
        : _coro(std::coroutine_handle<promise_type>::from_promise(promise))
    {}
    generator(generator&& other) noexcept
        : _coro{std::exchange(other._coro, {})}
        , _started{std::exchange(other._started, false)}
        , _buffer_index{std::exchange(other._buffer_index, 0)}
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
        std::swap(lhs._started, rhs._started);
        std::swap(lhs._buffer_index, rhs._buffer_index);
    }

    generator& operator=(generator&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        if (_coro) {
            _coro.destroy();
        }
        _coro = std::exchange(other._coro, nullptr);
        _started = std::exchange(other._started, false);
        _buffer_index = std::exchange(other._buffer_index, 0);
        return *this;
    }

    /// Get the next value from the generator.
    /// Returns std::optional containing the next value, or std::nullopt if done.
    /// For reference types, returns std::optional<std::reference_wrapper<T>>.
    ///
    /// The buffered generator accumulates elements internally until the buffer
    /// is full or need_preempt() returns true, then transfers control to the consumer.
    /// The consumer drains elements one at a time from the buffer without suspension.
    ///
    /// Example usage:
    /// \code
    /// generator<const T&, T, circular_buffer_fixed_capacity<T, 128>> gen = my_generator();
    /// while (auto value = co_await gen()) {
    ///     process(value->get());  // for references
    ///     process(*value);        // for values
    /// }
    /// \endcode
    class [[nodiscard]] call_awaiter {
        generator* _gen;

    public:
        explicit call_awaiter(generator* gen) noexcept
            : _gen(gen)
        {}

        bool await_ready() const noexcept {
            // Empty or done generator
            if (!_gen->_coro || _gen->_coro.done()) {
                return true;
            }

            auto& buffer = _gen->_coro.promise().buffer();
            // If we have elements in the buffer, we're ready (no suspension needed)
            if (_gen->_buffer_index < buffer.size()) {
                return true;
            }

            return false;
        }

        template <typename Promise>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> consumer SEASTAR_COROUTINE_LOC_PARAM) noexcept {
            SEASTAR_COROUTINE_LOC_STORE(consumer.promise());
            // Buffer is empty, need to resume producer to get more elements
            auto& promise = _gen->_coro.promise();
            promise._consumer = consumer;

            // Clear the buffer before resuming producer
            promise.buffer().clear();
            _gen->_buffer_index = 0;

            // Check if we need to preempt. If not, directly resume producer.
            // If yes, schedule the producer through the scheduler.
            if (!seastar::need_preempt()) {
                return _gen->_coro;
            }
            auto producer_handle = std::coroutine_handle<seastar::task>::from_address(
                _gen->_coro.address());
            seastar::schedule(&producer_handle.promise());
            return std::noop_coroutine();
        }

        optional_type await_resume() {
            // Check for invalid/null coroutine first
            if (!_gen->_coro) {
                return std::nullopt;
            }

            if (!_gen->_started) {
                _gen->_started = true;
            }

            auto& promise = _gen->_coro.promise();
            auto& buffer = promise.buffer();

            // Check buffer first - there might be elements even if coroutine is done
            if (_gen->_buffer_index < buffer.size()) {
                auto& element = buffer[_gen->_buffer_index++];
                if constexpr (std::is_reference_v<reference_type>) {
                    return std::reference_wrapper<std::remove_reference_t<reference_type>>(element);
                } else {
                    // For value types, convert/cast the element if needed
                    return static_cast<reference_type>(element);
                }
            }

            // Buffer is empty - check if producer is finished or has exception
            if (promise.finished() || _gen->_coro.done()) {
                promise.rethrow_if_unhandled_exception();
                return std::nullopt;
            }

            // This shouldn't happen - we resumed producer but got no elements
            return std::nullopt;
        }
    };

    [[nodiscard]] call_awaiter operator()() noexcept {
        return call_awaiter{this};
    }
};

/// buffered generator has 3 template parameters:
///
/// - Ref: The reference type returned by operator()
/// - Value: The value type
/// - Container: A bounded container type (must satisfy bounded_container concept)
///
/// The buffered generator supports two yielding patterns:
/// 1. Yield individual elements: co_yield element
/// 2. Yield ranges/slices: co_yield range (using C++23 ranges)
///
/// Elements are accumulated in an internal buffer until:
/// 1. The buffer reaches its capacity, OR
/// 2. need_preempt() returns true
///
/// Only then does the producer suspend. The consumer drains elements from the
/// buffer one at a time without any coroutine suspensions (tight loop within buffer).
///
/// This provides the convenience of yielding/consuming individual elements while
/// achieving the performance benefits of batching.
///
/// Example (yielding individual elements):
/// \code
/// generator<const directory_entry&, directory_entry,
///           circular_buffer_fixed_capacity<directory_entry, 128>> list_directory() {
///     for (auto& entry : entries) {
///         co_yield entry;  // Yields one at a time, batched internally
///     }
/// }
/// \endcode
///
/// Example (yielding ranges):
/// \code
/// generator<const int&, int, circular_buffer_fixed_capacity<int, 128>> generate() {
///     std::vector<int> batch = get_batch();
///     co_yield batch;  // Yields entire range, appended to buffer
///
///     co_yield std::span(data, 10);  // Can yield spans, views, etc.
/// }
/// \endcode
///
/// Consumer usage (same for both patterns):
/// \code
/// auto gen = list_directory();
/// while (auto entry = co_await gen()) {
///     process(entry->get());  // Receives one at a time, no suspension within batch
/// }
/// \endcode
template <typename Ref, typename Value, bounded_container Container>
class generator<Ref, Value, Container>::promise_type final : public generator_promise_base<container_type> {
public:
    generator get_return_object() noexcept {
        return generator{*this};
    }
};

} // namespace buffered

} // namespace internal

// Helper to select generator implementation based on third parameter
template <typename Ref, typename Value, typename ContainerOrVoid>
struct generator_selector {
    using type = std::conditional_t<!std::is_void_v<ContainerOrVoid> && internal::buffered::bounded_container<ContainerOrVoid>,
        internal::buffered::generator<Ref, Value, ContainerOrVoid>,
        internal::unbuffered::generator<Ref, Value>>;
};

// Specialization for void (unbuffered)
template <typename Ref, typename Value>
struct generator_selector<Ref, Value, void> {
    using type = internal::unbuffered::generator<Ref, Value>;
};

template<typename Ref, typename Value = void, typename ContainerOrVoid = void>
using generator = typename generator_selector<Ref, Value, ContainerOrVoid>::type;

} // namespace seastar::coroutine::experimental

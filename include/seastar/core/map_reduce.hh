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
 * Copyright (C) 2020 ScyllaDB.
 */

#pragma once

#include <iterator>

#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>

namespace seastar {

/// \addtogroup future-util
/// @{

/// \cond internal

template <typename T, typename Ptr, bool IsFuture>
struct reducer_with_get_traits;

template <typename T, typename Ptr>
struct reducer_with_get_traits<T, Ptr, false> {
    using result_type = decltype(std::declval<T>().get());
    using future_type = future<result_type>;
    static future_type maybe_call_get(future<> f, Ptr r) {
        return f.then([r = std::move(r)] () mutable {
            return make_ready_future<result_type>(std::move(r->reducer).get());
        });
    }
};

template <typename T, typename Ptr>
struct reducer_with_get_traits<T, Ptr, true> {
    using future_type = decltype(std::declval<T>().get());
    static future_type maybe_call_get(future<> f, Ptr r) {
        return f.then([r = std::move(r)] () mutable {
            return r->reducer.get();
        }).then_wrapped([r] (future_type f) {
            return f;
        });
    }
};

template <typename T, typename Ptr = lw_shared_ptr<T>, typename V = void>
struct reducer_traits {
    using future_type = future<>;
    static future_type maybe_call_get(future<> f, Ptr r) {
        return f.then([r = std::move(r)] {});
    }
};

template <typename T, typename Ptr>
struct reducer_traits<T, Ptr, decltype(std::declval<T>().get(), void())> : public reducer_with_get_traits<T, Ptr, is_future<std::invoke_result_t<decltype(&T::get),T>>::value> {};

/// \endcond

/// Map a function over a range and reduce the result.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the end of the range
/// \param mapper is a callable which transforms values from the iterator range into a future<T>
/// \param r is an object which can be called with T as parameter and yields a future<>
///     It may have a get() method which returns a value of type U which holds the result of reduction.
/// \return Th reduced value wrapped in a future.
///     If the reducer has no get() method then this function returns future<>.
/// \note map-reduce() schedules all invocations of both \c mapper and \c reduce
///       on the current shard. If you want to run a function on all shards in
///       parallel, have a look at \ref smp::invoke_on_all() instead, or combine
///       map_reduce() with \ref smp::submit_to().
///       Sharded services have their own \ref sharded::map_reduce() which
///       map-reduces across all shards.

// TODO: specialize for non-deferring reducer
template <typename Iterator, typename Mapper, typename Reducer>
SEASTAR_CONCEPT( requires requires (Iterator i, Mapper mapper, Reducer reduce) {
     *i++;
     { i != i } -> std::convertible_to<bool>;
     mapper(*i);
     reduce(futurize_invoke(mapper, *i).get0());
} )
inline
auto
map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Reducer&& r)
    -> typename reducer_traits<Reducer>::future_type
{
    struct state {
        Mapper mapper;
        Reducer reducer;
    };
    auto s = make_lw_shared(state{std::forward<Mapper>(mapper), std::forward<Reducer>(r)});
    future<> ret = make_ready_future<>();
    while (begin != end) {
        ret = futurize_invoke(s->mapper, *begin++).then_wrapped([ret = std::move(ret), s] (auto f) mutable {
            return ret.then_wrapped([f = std::move(f), s] (auto rf) mutable {
                if (rf.failed()) {
                    f.ignore_ready_future();
                    return rf;
                } else {
                    return futurize_invoke(s->reducer, std::move(f.get0()));
                }
            });
        });
    }
    return reducer_traits<Reducer, lw_shared_ptr<state>>::maybe_call_get(std::move(ret), s);
}

/// Asynchronous map/reduce transformation.
///
/// Given a range of objects, an asynchronous unary function
/// operating on these objects, an initial value, and a
/// binary function for reducing, map_reduce() will
/// transform each object in the range, then invoke
/// the the reducing function with the result.
///
/// Example:
///
/// Calculate the total size of several files:
///
/// \code
///  map_reduce(files.begin(), files.end(),
///             std::mem_fn(file::size),
///             size_t(0),
///             std::plus<size_t>())
/// \endcode
///
/// Requirements:
///    - Iterator: an InputIterator.
///    - Mapper: unary function taking Iterator::value_type and producing a future<...>.
///    - Initial: any value type
///    - Reduce: a binary function taking two Initial values and returning an Initial
///
/// Return type:
///    - future<Initial>
///
/// \param begin beginning of object range to operate on
/// \param end end of object range to operate on
/// \param mapper map function to call on each object, returning a future
/// \param initial initial input value to reduce function
/// \param reduce binary function for merging two result values from \c mapper
///
/// \return equivalent to \c reduce(reduce(initial, mapper(obj0)), mapper(obj1)) ...
///
/// \note map-reduce() schedules all invocations of both \c mapper and \c reduce
///       on the current shard. If you want to run a function on all shards in
///       parallel, have a look at \ref smp::invoke_on_all() instead, or combine
///       map_reduce() with \ref smp::submit_to().
///       Sharded services have their own \ref sharded::map_reduce() which
///       map-reduces across all shards.
template <typename Iterator, typename Mapper, typename Initial, typename Reduce>
SEASTAR_CONCEPT( requires requires (Iterator i, Mapper mapper, Initial initial, Reduce reduce) {
     *i++;
     { i != i} -> std::convertible_to<bool>;
     mapper(*i);
     requires is_future<decltype(mapper(*i))>::value;
     { reduce(std::move(initial), mapper(*i).get0()) } -> std::convertible_to<Initial>;
} )
inline
future<Initial>
map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Initial initial, Reduce reduce) {
    struct state {
        Mapper mapper;
        Initial result;
        Reduce reduce;
    };
    auto s = make_lw_shared(state{std::forward<Mapper>(mapper), std::move(initial), std::move(reduce)});
    future<> ret = make_ready_future<>();
    while (begin != end) {
        ret = futurize_invoke(s->mapper, *begin++).then_wrapped([s = s.get(), ret = std::move(ret)] (auto f) mutable {
            try {
                s->result = s->reduce(std::move(s->result), std::move(f.get0()));
                return std::move(ret);
            } catch (...) {
                return std::move(ret).then_wrapped([ex = std::current_exception()] (auto f) {
                    f.ignore_ready_future();
                    return make_exception_future<>(ex);
                });
            }
        });
    }
    return ret.then([s] {
        return make_ready_future<Initial>(std::move(s->result));
    });
}

/// Asynchronous map/reduce transformation (range version).
///
/// Given a range of objects, an asynchronous unary function
/// operating on these objects, an initial value, and a
/// binary function for reducing, map_reduce() will
/// transform each object in the range, then invoke
/// the the reducing function with the result.
///
/// Example:
///
/// Calculate the total size of several files:
///
/// \code
///  std::vector<file> files = ...;
///  map_reduce(files,
///             std::mem_fn(file::size),
///             size_t(0),
///             std::plus<size_t>())
/// \endcode
///
/// Requirements:
///    - Iterator: an InputIterator.
///    - Mapper: unary function taking Iterator::value_type and producing a future<...>.
///    - Initial: any value type
///    - Reduce: a binary function taking two Initial values and returning an Initial
///
/// Return type:
///    - future<Initial>
///
/// \param range object range to operate on
/// \param mapper map function to call on each object, returning a future
/// \param initial initial input value to reduce function
/// \param reduce binary function for merging two result values from \c mapper
///
/// \return equivalent to \c reduce(reduce(initial, mapper(obj0)), mapper(obj1)) ...
///
/// \note map-reduce() schedules all invocations of both \c mapper and \c reduce
///       on the current shard. If you want to run a function on all shards in
///       parallel, have a look at \ref smp::invoke_on_all() instead, or combine
///       map_reduce() with \ref smp::submit_to().
///       Sharded services have their own \ref sharded::map_reduce() which
///       map-reduces across all shards.
template <typename Range, typename Mapper, typename Initial, typename Reduce>
SEASTAR_CONCEPT( requires requires (Range range, Mapper mapper, Initial initial, Reduce reduce) {
     std::begin(range);
     std::end(range);
     mapper(*std::begin(range));
     requires is_future<std::remove_reference_t<decltype(mapper(*std::begin(range)))>>::value;
     { reduce(std::move(initial), mapper(*std::begin(range)).get0()) } -> std::convertible_to<Initial>;
} )
inline
future<Initial>
map_reduce(Range&& range, Mapper&& mapper, Initial initial, Reduce reduce) {
    return map_reduce(std::begin(range), std::end(range), std::forward<Mapper>(mapper),
            std::move(initial), std::move(reduce));
}

/// Implements @Reducer concept. Calculates the result by
/// adding elements to the accumulator.
template <typename Result, typename Addend = Result>
class adder {
private:
    Result _result;
public:
    future<> operator()(const Addend& value) {
        _result += value;
        return make_ready_future<>();
    }
    Result get() && {
        return std::move(_result);
    }
};

/// @}

} // namespace seastar

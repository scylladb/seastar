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
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 */


/** @file */

#ifndef CORE_FUTURE_UTIL_HH_
#define CORE_FUTURE_UTIL_HH_

#include "task.hh"
#include "future.hh"
#include "shared_ptr.hh"
#include "do_with.hh"
#include "timer.hh"
#include "util/bool_class.hh"
#include <tuple>
#include <iterator>
#include <vector>
#include <experimental/optional>
#include "util/tuple_utils.hh"

namespace seastar {

/// \cond internal
extern __thread size_t task_quota;
/// \endcond


/// \addtogroup future-util
/// @{

/// \cond internal

struct parallel_for_each_state {
    // use optional<> to avoid out-of-line constructor
    std::experimental::optional<std::exception_ptr> ex;
    size_t waiting = 0;
    promise<> pr;
    void complete() {
        if (--waiting == 0) {
            if (ex) {
                pr.set_exception(std::move(*ex));
            } else {
                pr.set_value();
            }
        }
    }
};

/// \endcond

/// Run tasks in parallel (iterator version).
///
/// Given a range [\c begin, \c end) of objects, run \c func on each \c *i in
/// the range, and return a future<> that resolves when all the functions
/// complete.  \c func should return a future<> that indicates when it is
/// complete.  All invocations are performed in parallel.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the end of the range
/// \param func Function to apply to each element in the range (returning
///             a \c future<>)
/// \return a \c future<> that resolves when all the function invocations
///         complete.  If one or more return an exception, the return value
///         contains one of the exceptions.
template <typename Iterator, typename Func>
GCC6_CONCEPT( requires requires (Func f, Iterator i) { { f(*i++) } -> future<>; } )
inline
future<>
parallel_for_each(Iterator begin, Iterator end, Func&& func) {
    if (begin == end) {
        return make_ready_future<>();
    }
    return do_with(parallel_for_each_state(), [&] (parallel_for_each_state& state) -> future<> {
        // increase ref count to ensure all functions run
        ++state.waiting;
        while (begin != end) {
            ++state.waiting;
            try {
                func(*begin++).then_wrapped([&] (future<> f) {
                    if (f.failed()) {
                        // We can only store one exception.  For more, use when_all().
                        if (!state.ex) {
                            state.ex = f.get_exception();
                        } else {
                            f.ignore_ready_future();
                        }
                    }
                    state.complete();
                });
            } catch (...) {
                if (!state.ex) {
                    state.ex = std::move(std::current_exception());
                }
                state.complete();
            }
        }
        // match increment on top
        state.complete();
        return state.pr.get_future();
    });
}

/// Run tasks in parallel (range version).
///
/// Given a \c range of objects, apply \c func to each object
/// in the range, and return a future<> that resolves when all
/// the functions complete.  \c func should return a future<> that indicates
/// when it is complete.  All invocations are performed in parallel.
///
/// \param range A range of objects to iterate run \c func on
/// \param func  A callable, accepting reference to the range's
///              \c value_type, and returning a \c future<>.
/// \return a \c future<> that becomes ready when the entire range
///         was processed.  If one or more of the invocations of
///         \c func returned an exceptional future, then the return
///         value will contain one of those exceptions.
template <typename Range, typename Func>
GCC6_CONCEPT( requires requires (Func f, Range r) { { f(*r.begin()) } -> future<>; } )
inline
future<>
parallel_for_each(Range&& range, Func&& func) {
    return parallel_for_each(std::begin(range), std::end(range),
            std::forward<Func>(func));
}

// The AsyncAction concept represents an action which can complete later than
// the actual function invocation. It is represented by a function which
// returns a future which resolves when the action is done.

/// \cond internal
template<typename AsyncAction, typename StopCondition>
inline
void do_until_continued(StopCondition&& stop_cond, AsyncAction&& action, promise<> p) {
    while (!stop_cond()) {
        try {
            auto&& f = action();
            if (!f.available() || need_preempt()) {
                f.then_wrapped([action = std::forward<AsyncAction>(action),
                    stop_cond = std::forward<StopCondition>(stop_cond), p = std::move(p)](std::result_of_t<AsyncAction()> fut) mutable {
                    if (!fut.failed()) {
                        do_until_continued(stop_cond, std::forward<AsyncAction>(action), std::move(p));
                    } else {
                        p.set_exception(fut.get_exception());
                    }
                });
                return;
            }

            if (f.failed()) {
                f.forward_to(std::move(p));
                return;
            }
        } catch (...) {
            p.set_exception(std::current_exception());
            return;
        }
    }

    p.set_value();
}
/// \endcond

struct stop_iteration_tag { };
using stop_iteration = bool_class<stop_iteration_tag>;

/// Invokes given action until it fails or the function requests iteration to stop by returning
/// \c stop_iteration::yes.
///
/// \param action a callable taking no arguments, returning a future<stop_iteration>.  Will
///               be called again as soon as the future resolves, unless the
///               future fails, action throws, or it resolves with \c stop_iteration::yes.
///               If \c action is an r-value it can be moved in the middle of iteration.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action failed.
template<typename AsyncAction>
GCC6_CONCEPT( requires seastar::ApplyReturns<AsyncAction, stop_iteration> || seastar::ApplyReturns<AsyncAction, future<stop_iteration>> )
inline
future<> repeat(AsyncAction&& action) {
    using futurator = futurize<std::result_of_t<AsyncAction()>>;
    static_assert(std::is_same<future<stop_iteration>, typename futurator::type>::value, "bad AsyncAction signature");

    try {
        do {
            auto f = futurator::apply(action);

            if (!f.available()) {
                return f.then([action = std::forward<AsyncAction>(action)] (stop_iteration stop) mutable {
                    if (stop == stop_iteration::yes) {
                        return make_ready_future<>();
                    } else {
                        return repeat(std::forward<AsyncAction>(action));
                    }
                });
            }

            if (f.get0() == stop_iteration::yes) {
                return make_ready_future<>();
            }
        } while (!need_preempt());

        promise<> p;
        auto f = p.get_future();
        schedule(make_task([action = std::forward<AsyncAction>(action), p = std::move(p)]() mutable {
            repeat(std::forward<AsyncAction>(action)).forward_to(std::move(p));
        }));
        return f;
    } catch (...) {
        return make_exception_future(std::current_exception());
    }
}

/// \cond internal

template <typename T>
struct repeat_until_value_type_helper;

/// \endcond

/// Type helper for repeat_until_value()
template <typename T>
struct repeat_until_value_type_helper<future<std::experimental::optional<T>>> {
    /// The type of the value we are computing
    using value_type = T;
    /// Type used by \c AsyncAction while looping
    using optional_type = std::experimental::optional<T>;
    /// Return type of repeat_until_value()
    using future_type = future<value_type>;
    /// Return type of \c AsyncAction
    using future_optional_type = future<optional_type>;
};

/// Return value of repeat_until_value()
template <typename AsyncAction>
using repeat_until_value_return_type
        = typename repeat_until_value_type_helper<std::result_of_t<AsyncAction()>>::future_type;

/// Invokes given action until it fails or the function requests iteration to stop by returning
/// an engaged \c future<std::experimental::optional<T>>.  The value is extracted from the
/// \c optional, and returned, as a future, from repeat_until_value().
///
/// \param action a callable taking no arguments, returning a future<std::experimental::optional<T>>.
///               Will be called again as soon as the future resolves, unless the
///               future fails, action throws, or it resolves with an engaged \c optional.
///               If \c action is an r-value it can be moved in the middle of iteration.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action failed.  The \c optional's value is returned.
template<typename AsyncAction>
GCC6_CONCEPT( requires requires (AsyncAction aa) {
    requires is_future<decltype(aa())>::value;
    bool(aa().get0());
    aa().get0().value();
} )
repeat_until_value_return_type<AsyncAction>
repeat_until_value(AsyncAction&& action) {
    using type_helper = repeat_until_value_type_helper<std::result_of_t<AsyncAction()>>;
    // the "T" in the documentation
    using value_type = typename type_helper::value_type;
    using optional_type = typename type_helper::optional_type;
    using futurator = futurize<typename type_helper::future_optional_type>;
    do {
        auto f = futurator::apply(action);

        if (!f.available()) {
            return f.then([action = std::forward<AsyncAction>(action)] (auto&& optional) mutable {
                if (optional) {
                    return make_ready_future<value_type>(std::move(optional.value()));
                } else {
                    return repeat_until_value(std::forward<AsyncAction>(action));
                }
            });
        }

        if (f.failed()) {
            return make_exception_future<value_type>(f.get_exception());
        }

        optional_type&& optional = std::move(f).get0();
        if (optional) {
            return make_ready_future<value_type>(std::move(optional.value()));
        }
    } while (!need_preempt());

    try {
        promise<value_type> p;
        auto f = p.get_future();
        schedule(make_task([action = std::forward<AsyncAction>(action), p = std::move(p)] () mutable {
            repeat_until_value(std::forward<AsyncAction>(action)).forward_to(std::move(p));
        }));
        return f;
    } catch (...) {
        return make_exception_future<value_type>(std::current_exception());
    }
}

/// Invokes given action until it fails or given condition evaluates to true.
///
/// \param stop_cond a callable taking no arguments, returning a boolean that
///                  evalutes to true when you don't want to call \c action
///                  any longer
/// \param action a callable taking no arguments, returning a future<>.  Will
///               be called again as soon as the future resolves, unless the
///               future fails, or \c stop_cond returns \c true.
/// \return a ready future if we stopped successfully, or a failed future if
///         a call to to \c action failed.
template<typename AsyncAction, typename StopCondition>
GCC6_CONCEPT( requires seastar::ApplyReturns<StopCondition, bool> && seastar::ApplyReturns<AsyncAction, future<>> )
inline
future<> do_until(StopCondition&& stop_cond, AsyncAction&& action) {
    promise<> p;
    auto f = p.get_future();
    do_until_continued(std::forward<StopCondition>(stop_cond),
        std::forward<AsyncAction>(action), std::move(p));
    return f;
}

/// Invoke given action until it fails.
///
/// Calls \c action repeatedly until it returns a failed future.
///
/// \param action a callable taking no arguments, returning a \c future<>
///        that becomes ready when you wish it to be called again.
/// \return a future<> that will resolve to the first failure of \c action
template<typename AsyncAction>
GCC6_CONCEPT( requires seastar::ApplyReturns<AsyncAction, future<>> )
inline
future<> keep_doing(AsyncAction&& action) {
    return repeat([action = std::forward<AsyncAction>(action)] () mutable {
        return action().then([] {
            return stop_iteration::no;
        });
    });
}

/// Call a function for each item in a range, sequentially (iterator version).
///
/// For each item in a range, call a function, waiting for the previous
/// invocation to complete before calling the next one.
///
/// \param begin an \c InputIterator designating the beginning of the range
/// \param end an \c InputIterator designating the endof the range
/// \param action a callable, taking a reference to objects from the range
///               as a parameter, and returning a \c future<> that resolves
///               when it is acceptable to process the next item.
/// \return a ready future on success, or the first failed future if
///         \c action failed.
template<typename Iterator, typename AsyncAction>
GCC6_CONCEPT( requires requires (Iterator i, AsyncAction aa) { { aa(*i) } -> future<> } )
inline
future<> do_for_each(Iterator begin, Iterator end, AsyncAction&& action) {
    if (begin == end) {
        return make_ready_future<>();
    }
    while (true) {
        auto f = futurize<void>::apply(action, *begin);
        ++begin;
        if (begin == end) {
            return f;
        }
        if (!f.available() || need_preempt()) {
            return std::move(f).then([action = std::forward<AsyncAction>(action),
                    begin = std::move(begin), end = std::move(end)] () mutable {
                return do_for_each(std::move(begin), std::move(end), std::forward<AsyncAction>(action));
            });
        }
        if (f.failed()) {
            return std::move(f);
        }
    }
}

/// Call a function for each item in a range, sequentially (range version).
///
/// For each item in a range, call a function, waiting for the previous
/// invocation to complete before calling the next one.
///
/// \param range an \c Range object designating input values
/// \param action a callable, taking a reference to objects from the range
///               as a parameter, and returning a \c future<> that resolves
///               when it is acceptable to process the next item.
/// \return a ready future on success, or the first failed future if
///         \c action failed.
template<typename Container, typename AsyncAction>
GCC6_CONCEPT( requires requires (Container c, AsyncAction aa) { { aa(*c.begin()) } -> future<> } )
inline
future<> do_for_each(Container& c, AsyncAction&& action) {
    return do_for_each(std::begin(c), std::end(c), std::forward<AsyncAction>(action));
}

/// \cond internal
namespace internal {

template<typename... Futures>
struct identity_futures_tuple {
    using future_type = future<std::tuple<Futures...>>;
    using promise_type = typename future_type::promise_type;

    static void set_promise(promise_type& p, std::tuple<Futures...> futures) {
        p.set_value(std::move(futures));
    }
};

template<typename ResolvedTupleTransform, typename... Futures>
class when_all_state : public enable_lw_shared_from_this<when_all_state<ResolvedTupleTransform, Futures...>> {
    using type = std::tuple<Futures...>;
    type tuple;
public:
    typename ResolvedTupleTransform::promise_type p;
    when_all_state(Futures&&... t) : tuple(std::make_tuple(std::move(t)...)) {}
    ~when_all_state() {
        ResolvedTupleTransform::set_promise(p, std::move(tuple));
    }
private:
    template<size_t Idx>
    int wait() {
        auto& f = std::get<Idx>(tuple);
        static_assert(is_future<std::remove_reference_t<decltype(f)>>::value, "when_all parameter must be a future");
        if (!f.available()) {
            f = f.then_wrapped([s = this->shared_from_this()] (auto&& f) {
                return std::move(f);
            });
        }
        return 0;
    }
public:
    template <size_t... Idx>
    typename ResolvedTupleTransform::future_type wait_all(std::index_sequence<Idx...>) {
        [] (...) {} (this->template wait<Idx>()...);
        return p.get_future();
    }
};

}
/// \endcond

GCC6_CONCEPT(

/// \cond internal
namespace impl {


// Want: folds

template <typename T>
struct is_tuple_of_futures : std::false_type {
};

template <>
struct is_tuple_of_futures<std::tuple<>> : std::true_type {
};

template <typename... T, typename... Rest>
struct is_tuple_of_futures<std::tuple<future<T...>, Rest...>> : is_tuple_of_futures<std::tuple<Rest...>> {
};

}
/// \endcond

template <typename... Futs>
concept bool AllAreFutures = impl::is_tuple_of_futures<std::tuple<Futs...>>::value;

)


/// Wait for many futures to complete, capturing possible errors (variadic version).
///
/// Given a variable number of futures as input, wait for all of them
/// to resolve (either successfully or with an exception), and return
/// them as a tuple so individual values or exceptions can be examined.
///
/// \param futs futures to wait for
/// \return an \c std::tuple<> of all the futures in the input; when
///         ready, all contained futures will be ready as well.
template <typename... Futs>
GCC6_CONCEPT( requires seastar::AllAreFutures<Futs...> )
inline
future<std::tuple<Futs...>>
when_all(Futs&&... futs) {
    namespace si = internal;
    using state = si::when_all_state<si::identity_futures_tuple<Futs...>, Futs...>;
    auto s = make_lw_shared<state>(std::forward<Futs>(futs)...);
    return s->wait_all(std::make_index_sequence<sizeof...(Futs)>());
}

/// \cond internal
namespace internal {

template <typename Iterator, typename IteratorCategory>
inline
size_t
when_all_estimate_vector_capacity(Iterator begin, Iterator end, IteratorCategory category) {
    // For InputIterators we can't estimate needed capacity
    return 0;
}

template <typename Iterator>
inline
size_t
when_all_estimate_vector_capacity(Iterator begin, Iterator end, std::forward_iterator_tag category) {
    // May be linear time below random_access_iterator_tag, but still better than reallocation
    return std::distance(begin, end);
}

template<typename Future>
struct identity_futures_vector {
    using future_type = future<std::vector<Future>>;
    static future_type run(std::vector<Future> futures) {
        return make_ready_future<std::vector<Future>>(std::move(futures));
    }
};

// Internal function for when_all().
template <typename ResolvedVectorTransform, typename Future>
inline
typename ResolvedVectorTransform::future_type
complete_when_all(std::vector<Future>&& futures, typename std::vector<Future>::iterator pos) {
    // If any futures are already ready, skip them.
    while (pos != futures.end() && pos->available()) {
        ++pos;
    }
    // Done?
    if (pos == futures.end()) {
        return ResolvedVectorTransform::run(std::move(futures));
    }
    // Wait for unready future, store, and continue.
    return pos->then_wrapped([futures = std::move(futures), pos] (auto fut) mutable {
        *pos++ = std::move(fut);
        return complete_when_all<ResolvedVectorTransform>(std::move(futures), pos);
    });
}

template<typename ResolvedVectorTransform, typename FutureIterator>
inline auto
do_when_all(FutureIterator begin, FutureIterator end) {
    using itraits = std::iterator_traits<FutureIterator>;
    std::vector<typename itraits::value_type> ret;
    ret.reserve(when_all_estimate_vector_capacity(begin, end, typename itraits::iterator_category()));
    // Important to invoke the *begin here, in case it's a function iterator,
    // so we launch all computation in parallel.
    std::move(begin, end, std::back_inserter(ret));
    return complete_when_all<ResolvedVectorTransform>(std::move(ret), ret.begin());
}

}
/// \endcond

/// Wait for many futures to complete, capturing possible errors (iterator version).
///
/// Given a range of futures as input, wait for all of them
/// to resolve (either successfully or with an exception), and return
/// them as a \c std::vector so individual values or exceptions can be examined.
///
/// \param begin an \c InputIterator designating the beginning of the range of futures
/// \param end an \c InputIterator designating the end of the range of futures
/// \return an \c std::vector<> of all the futures in the input; when
///         ready, all contained futures will be ready as well.
template <typename FutureIterator>
GCC6_CONCEPT( requires requires (FutureIterator i) { { *i++ }; requires is_future<std::remove_reference_t<decltype(*i)>>::value; } )
inline
future<std::vector<typename std::iterator_traits<FutureIterator>::value_type>>
when_all(FutureIterator begin, FutureIterator end) {
    namespace si = internal;
    using itraits = std::iterator_traits<FutureIterator>;
    using result_transform = si::identity_futures_vector<typename itraits::value_type>;
    return si::do_when_all<result_transform>(std::move(begin), std::move(end));
}

template <typename T, bool IsFuture>
struct reducer_with_get_traits;

template <typename T>
struct reducer_with_get_traits<T, false> {
    using result_type = decltype(std::declval<T>().get());
    using future_type = future<result_type>;
    static future_type maybe_call_get(future<> f, lw_shared_ptr<T> r) {
        return f.then([r = std::move(r)] () mutable {
            return make_ready_future<result_type>(std::move(*r).get());
        });
    }
};

template <typename T>
struct reducer_with_get_traits<T, true> {
    using future_type = decltype(std::declval<T>().get());
    static future_type maybe_call_get(future<> f, lw_shared_ptr<T> r) {
        return f.then([r = std::move(r)] () mutable {
            return r->get();
        }).then_wrapped([r] (future_type f) {
            return f;
        });
    }
};

template <typename T, typename V = void>
struct reducer_traits {
    using future_type = future<>;
    static future_type maybe_call_get(future<> f, lw_shared_ptr<T> r) {
        return f.then([r = std::move(r)] {});
    }
};

template <typename T>
struct reducer_traits<T, decltype(std::declval<T>().get(), void())> : public reducer_with_get_traits<T, is_future<std::result_of_t<decltype(&T::get)(T)>>::value> {};

// @Mapper is a callable which transforms values from the iterator range
// into a future<T>. @Reducer is an object which can be called with T as
// parameter and yields a future<>. It may have a get() method which returns
// a value of type U which holds the result of reduction. This value is wrapped
// in a future and returned by this function. If the reducer has no get() method
// then this function returns future<>.
//
// TODO: specialize for non-deferring reducer
template <typename Iterator, typename Mapper, typename Reducer>
inline
auto
map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Reducer&& r)
    -> typename reducer_traits<Reducer>::future_type
{
    auto r_ptr = make_lw_shared(std::forward<Reducer>(r));
    future<> ret = make_ready_future<>();
    using futurator = futurize<decltype(mapper(*begin))>;
    while (begin != end) {
        ret = futurator::apply(mapper, *begin++).then_wrapped([ret = std::move(ret), r_ptr] (auto f) mutable {
            return ret.then_wrapped([f = std::move(f), r_ptr] (auto rf) mutable {
                if (rf.failed()) {
                    f.ignore_ready_future();
                    return std::move(rf);
                } else {
                    return futurize<void>::apply(*r_ptr, std::move(f.get()));
                }
            });
        });
    }
    return reducer_traits<Reducer>::maybe_call_get(std::move(ret), r_ptr);
}

/// Asynchronous map/reduce transformation.
///
/// Given a range of objects, an asynchronous unary function
/// operating on these objects, an initial value, and a
/// binary function for reducing, map_reduce() will
/// transform each object in the range, then apply
/// the the reducing function to the result.
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
template <typename Iterator, typename Mapper, typename Initial, typename Reduce>
GCC6_CONCEPT( requires requires (Iterator i, Mapper mapper, Initial initial, Reduce reduce) {
     *i++;
     { i != i} -> bool;
     mapper(*i);
     requires is_future<decltype(mapper(*i))>::value;
     { reduce(std::move(initial), mapper(*i).get0()) } -> Initial;
} )
inline
future<Initial>
map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Initial initial, Reduce reduce) {
    struct state {
        Initial result;
        Reduce reduce;
    };
    auto s = make_lw_shared(state{std::move(initial), std::move(reduce)});
    future<> ret = make_ready_future<>();
    using futurator = futurize<decltype(mapper(*begin))>;
    while (begin != end) {
        ret = futurator::apply(mapper, *begin++).then_wrapped([s = s.get(), ret = std::move(ret)] (auto f) mutable {
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
/// transform each object in the range, then apply
/// the the reducing function to the result.
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
template <typename Range, typename Mapper, typename Initial, typename Reduce>
GCC6_CONCEPT( requires requires (Range range, Mapper mapper, Initial initial, Reduce reduce) {
     std::begin(range);
     std::end(range);
     mapper(*std::begin(range));
     requires is_future<std::remove_reference_t<decltype(mapper(*std::begin(range)))>>::value;
     { reduce(std::move(initial), mapper(*std::begin(range)).get0()) } -> Initial;
} )
inline
future<Initial>
map_reduce(Range&& range, Mapper&& mapper, Initial initial, Reduce reduce) {
    return map_reduce(std::begin(range), std::end(range), std::forward<Mapper>(mapper),
            std::move(initial), std::move(reduce));
}

// Implements @Reducer concept. Calculates the result by
// adding elements to the accumulator.
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

inline
future<> now() {
    return make_ready_future<>();
}

// Returns a future which is not ready but is scheduled to resolve soon.
future<> later();

class timed_out_error : public std::exception {
public:
    virtual const char* what() const noexcept {
        return "timedout";
    }
};

struct default_timeout_exception_factory {
    static auto timeout() {
        return timed_out_error();
    }
};

/// \brief Wait for either a future, or a timeout, whichever comes first
///
/// When timeout is reached the returned future resolves with an exception
/// produced by ExceptionFactory::timeout(). By default it is \ref timed_out_error exception.
///
/// Note that timing out doesn't cancel any tasks associated with the original future.
/// It also doesn't cancel the callback registerred on it.
///
/// \param f future to wait for
/// \param timeout time point after which the returned future should be failed
///
/// \return a future which will be either resolved with f or a timeout exception
template<typename ExceptionFactory = default_timeout_exception_factory, typename Clock, typename Duration, typename... T>
future<T...> with_timeout(std::chrono::time_point<Clock, Duration> timeout, future<T...> f) {
    if (f.available()) {
        return f;
    }
    auto pr = std::make_unique<promise<T...>>();
    auto result = pr->get_future();
    timer<Clock> timer([&pr = *pr] {
        pr.set_exception(std::make_exception_ptr(ExceptionFactory::timeout()));
    });
    timer.arm(timeout);
    f.then_wrapped([pr = std::move(pr), timer = std::move(timer)] (auto&& f) mutable {
        if (timer.cancel()) {
            f.forward_to(std::move(*pr));
        } else {
            f.ignore_ready_future();
        }
    });
    return result;
}

namespace internal {

template<typename Future>
struct future_has_value {
    enum {
        value = !std::is_same<std::decay_t<Future>, future<>>::value
    };
};

template<typename Tuple>
struct tuple_to_future;

template<typename... Elements>
struct tuple_to_future<std::tuple<Elements...>> {
    using type = future<Elements...>;
    using promise_type = promise<Elements...>;

    static auto make_ready(std::tuple<Elements...> t) {
        auto create_future = [] (auto&&... args) {
            return make_ready_future<Elements...>(std::move(args)...);
        };
        return apply(create_future, std::move(t));
    }

    static auto make_failed(std::exception_ptr excp) {
        return make_exception_future<Elements...>(std::move(excp));
    }
};

template<typename... Futures>
class extract_values_from_futures_tuple {
    static auto transform(std::tuple<Futures...> futures) {
        auto prepare_result = [] (auto futures) {
            auto fs = tuple_filter_by_type<internal::future_has_value>(std::move(futures));
            return tuple_map(std::move(fs), [] (auto&& e) {
                return internal::untuple(e.get());
            });
        };

        using tuple_futurizer = internal::tuple_to_future<decltype(prepare_result(std::move(futures)))>;

        std::exception_ptr excp;
        tuple_for_each(futures, [&excp] (auto& f) {
            if (!excp) {
                if (f.failed()) {
                    excp = f.get_exception();
                }
            } else {
                f.ignore_ready_future();
            }
        });
        if (excp) {
            return tuple_futurizer::make_failed(std::move(excp));
        }

        return tuple_futurizer::make_ready(prepare_result(std::move(futures)));
    }
public:
    using future_type = decltype(transform(std::declval<std::tuple<Futures...>>()));
    using promise_type = typename future_type::promise_type;

    static void set_promise(promise_type& p, std::tuple<Futures...> tuple) {
        transform(std::move(tuple)).forward_to(std::move(p));
    }
};

template<typename Future>
struct extract_values_from_futures_vector {
    using value_type = decltype(untuple(std::declval<typename Future::value_type>()));

    using future_type = future<std::vector<value_type>>;

    static future_type run(std::vector<Future> futures) {
        std::vector<value_type> values;
        values.reserve(futures.size());

        std::exception_ptr excp;
        for (auto&& f : futures) {
            if (!excp) {
                if (f.failed()) {
                    excp = f.get_exception();
                } else {
                    values.emplace_back(untuple(f.get()));
                }
            } else {
                f.ignore_ready_future();
            }
        }
        if (excp) {
            return make_exception_future<std::vector<value_type>>(std::move(excp));
        }
        return make_ready_future<std::vector<value_type>>(std::move(values));
    }
};

template<>
struct extract_values_from_futures_vector<future<>> {
    using future_type = future<>;

    static future_type run(std::vector<future<>> futures) {
        std::exception_ptr excp;
        for (auto&& f : futures) {
            if (!excp) {
                if (f.failed()) {
                    excp = f.get_exception();
                }
            } else {
                f.ignore_ready_future();
            }
        }
        if (excp) {
            return make_exception_future<>(std::move(excp));
        }
        return make_ready_future<>();
    }
};

}

/// Wait for many futures to complete (variadic version).
///
/// Given a variable number of futures as input, wait for all of them
/// to resolve, and return a future containing the values of each individual
/// resolved future.
/// In case any of the given futures fails one of the exceptions is returned
/// by this function as a failed future.
///
/// \param futures futures to wait for
/// \return future containing values of input futures
template<typename... Futures>
GCC6_CONCEPT( requires seastar::AllAreFutures<Futures...> )
inline auto when_all_succeed(Futures&&... futures) {
    using state = internal::when_all_state<internal::extract_values_from_futures_tuple<Futures...>, Futures...>;
    auto s = make_lw_shared<state>(std::forward<Futures>(futures)...);
    return s->wait_all(std::make_index_sequence<sizeof...(Futures)>());
}

/// Wait for many futures to complete (iterator version).
///
/// Given a range of futures as input, wait for all of them
/// to resolve, and return a future containing a vector of values of the
/// original futures.
/// In case any of the given futures fails one of the exceptions is returned
/// by this function as a failed future.
/// \param begin an \c InputIterator designating the beginning of the range of futures
/// \param end an \c InputIterator designating the end of the range of futures
/// \return an \c std::vector<> of all the valus in the input
template <typename FutureIterator, typename = typename std::iterator_traits<FutureIterator>::value_type>
GCC6_CONCEPT( requires requires (FutureIterator i) {
     *i++;
     { i != i } -> bool;
     requires is_future<std::remove_reference_t<decltype(*i)>>::value;
} )
inline auto
when_all_succeed(FutureIterator begin, FutureIterator end) {
    using itraits = std::iterator_traits<FutureIterator>;
    using result_transform = internal::extract_values_from_futures_vector<typename itraits::value_type>;
    return internal::do_when_all<result_transform>(std::move(begin), std::move(end));
}

}

/// @}

#endif /* CORE_FUTURE_UTIL_HH_ */

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
 * Copyright (C) 2017 ScyllaDB.
 */

#pragma once

#include <tuple>
#include <utility>
#include <stddef.h>

namespace seastar {

/// \cond internal
namespace internal {

template<typename Tuple>
Tuple untuple(Tuple t) {
    return t;
}

template<typename T>
T untuple(std::tuple<T> t) {
    return std::get<0>(std::move(t));
}

template<typename Tuple, typename Function, size_t... I>
void tuple_for_each_helper(Tuple&& t, Function&& f, std::index_sequence<I...>&&) {
    auto ignore_me = { (f(std::get<I>(std::forward<Tuple>(t))), 1)... };
    (void)ignore_me;
}

template<typename Tuple, typename MapFunction, size_t... I>
auto tuple_map_helper(Tuple&& t, MapFunction&& f, std::index_sequence<I...>&&) {
    return std::make_tuple(f(std::get<I>(std::forward<Tuple>(t)))...);
}

template<size_t I, typename IndexSequence>
struct prepend;

template<size_t I, size_t... Is>
struct prepend<I, std::index_sequence<Is...>> {
    using type = std::index_sequence<I, Is...>;
};

template<template<typename> class Filter, typename Tuple, typename IndexSequence>
struct tuple_filter;

template<template<typename> class Filter, typename T, typename... Ts, size_t I, size_t... Is>
struct tuple_filter<Filter, std::tuple<T, Ts...>, std::index_sequence<I, Is...>> {
    using tail = typename tuple_filter<Filter, std::tuple<Ts...>, std::index_sequence<Is...>>::type;
    using type = std::conditional_t<Filter<T>::value, typename prepend<I, tail>::type, tail>;
};

template<template<typename> class Filter>
struct tuple_filter<Filter, std::tuple<>, std::index_sequence<>> {
    using type = std::index_sequence<>;
};

template<typename Tuple, size_t... I>
auto tuple_filter_helper(Tuple&& t, std::index_sequence<I...>&&) {
    return std::make_tuple(std::get<I>(std::forward<Tuple>(t))...);
}

}
/// \endcond

/// \addtogroup utilities
/// @{

/// Applies type transformation to all types in tuple
///
/// Member type `type` is set to a tuple type which is a result of applying
/// transformation `MapClass<T>::type` to each element `T` of the input tuple
/// type.
///
/// \tparam MapClass class template defining type transformation
/// \tparam Tuple input tuple type
template<template<typename> class MapClass, typename Tuple>
struct tuple_map_types;

/// @}

template<template<typename> class MapClass, typename... Elements>
struct tuple_map_types<MapClass, std::tuple<Elements...>> {
    using type = std::tuple<typename MapClass<Elements>::type...>;
};

/// \addtogroup utilities
/// @{

/// Filters elements in tuple by their type
///
/// Returns a tuple containing only those elements which type `T` caused
/// expression FilterClass<T>::value to be true.
///
/// \tparam FilterClass class template having an element value set to true for elements that
///                     should be present in the result
/// \param t tuple to filter
/// \return a tuple contaning elements which type passed the test
template<template<typename> class FilterClass, typename... Elements>
auto tuple_filter_by_type(const std::tuple<Elements...>& t) {
    using sequence = typename internal::tuple_filter<FilterClass, std::tuple<Elements...>,
                                                     std::index_sequence_for<Elements...>>::type;
    return internal::tuple_filter_helper(t, sequence());
}
template<template<typename> class FilterClass, typename... Elements>
auto tuple_filter_by_type(std::tuple<Elements...>&& t) {
    using sequence = typename internal::tuple_filter<FilterClass, std::tuple<Elements...>,
                                                     std::index_sequence_for<Elements...>>::type;
    return internal::tuple_filter_helper(std::move(t), sequence());
}

/// Applies function to all elements in tuple
///
/// Applies given function to all elements in the tuple and returns a tuple
/// of results.
///
/// \param t original tuple
/// \param f function to apply
/// \return tuple of results returned by f for each element in t
template<typename Function, typename... Elements>
auto tuple_map(const std::tuple<Elements...>& t, Function&& f) {
    return internal::tuple_map_helper(t, std::forward<Function>(f),
                                      std::index_sequence_for<Elements...>());
}
template<typename Function, typename... Elements>
auto tuple_map(std::tuple<Elements...>&& t, Function&& f) {
    return internal::tuple_map_helper(std::move(t), std::forward<Function>(f),
                                      std::index_sequence_for<Elements...>());
}

/// Iterate over all elements in tuple
///
/// Iterates over given tuple and calls the specified function for each of
/// it elements.
///
/// \param t a tuple to iterate over
/// \param f function to call for each tuple element
template<typename Function, typename... Elements>
void tuple_for_each(const std::tuple<Elements...>& t, Function&& f) {
    return internal::tuple_for_each_helper(t, std::forward<Function>(f),
                                           std::index_sequence_for<Elements...>());
}
template<typename Function, typename... Elements>
void tuple_for_each(std::tuple<Elements...>& t, Function&& f) {
    return internal::tuple_for_each_helper(t, std::forward<Function>(f),
                                           std::index_sequence_for<Elements...>());
}
template<typename Function, typename... Elements>
void tuple_for_each(std::tuple<Elements...>&& t, Function&& f) {
    return internal::tuple_for_each_helper(std::move(t), std::forward<Function>(f),
                                           std::index_sequence_for<Elements...>());
}

/// @}

}

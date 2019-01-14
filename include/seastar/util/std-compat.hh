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
 * Copyright (C) 2018 ScyllaDB
 */

#pragma once

#ifdef SEASTAR_USE_STD_OPTIONAL_VARIANT_STRINGVIEW
#include <optional>
#include <string_view>
#include <variant>
#else
#include <experimental/optional>
#include <experimental/string_view>
#include <boost/variant.hpp>
#endif

namespace seastar {

/// \cond internal

namespace compat {

#ifdef SEASTAR_USE_STD_OPTIONAL_VARIANT_STRINGVIEW

template <typename T>
using optional = std::optional<T>;

using nullopt_t = std::nullopt_t;

inline constexpr auto nullopt = std::nullopt;

template <typename T>
inline constexpr optional<std::decay_t<T>> make_optional(T&& value) {
    return std::make_optional(std::forward<T>(value));
}

template <typename CharT, typename Traits = std::char_traits<CharT>>
using basic_string_view = std::basic_string_view<CharT, Traits>;

template <typename CharT, typename Traits = std::char_traits<CharT>>
std::string string_view_to_string(const basic_string_view<CharT, Traits>& v) {
    return std::string(v);
}

template <typename... Types>
using variant = std::variant<Types...>;

template <std::size_t I, typename... Types>
constexpr std::variant_alternative_t<I, variant<Types...>>& get(variant<Types...>& v) {
    return std::get<I>(v);
}

template <std::size_t I, typename... Types>
constexpr const std::variant_alternative_t<I, variant<Types...>>& get(const variant<Types...>& v) {
    return std::get<I>(v);
}

template <std::size_t I, typename... Types>
constexpr std::variant_alternative_t<I, variant<Types...>>&& get(variant<Types...>&& v) {
    return std::get<I>(v);
}

template <std::size_t I, typename... Types>
constexpr const std::variant_alternative_t<I, variant<Types...>>&& get(const variant<Types...>&& v) {
    return std::get<I>(v);
}

template <typename U, typename... Types>
constexpr U& get(variant<Types...>& v) {
    return std::get<U>(v);
}

template <typename U, typename... Types>
constexpr const U& get(const variant<Types...>& v) {
    return std::get<U>(v);
}

template <typename U, typename... Types>
constexpr U&& get(variant<Types...>&& v) {
    return std::get<U>(v);
}

template <typename U, typename... Types>
constexpr const U&& get(const variant<Types...>&& v) {
    return std::get<U>(v);
}

template <typename U, typename... Types>
constexpr U* get_if(variant<Types...>* v) {
    return std::get_if<U>(v);
}

template <typename U, typename... Types>
constexpr const U* get_if(const variant<Types...>* v) {
    return std::get_if<U>(v);
}

#else

template <typename T>
using optional = std::experimental::optional<T>;

using nullopt_t = std::experimental::nullopt_t;

constexpr auto nullopt = std::experimental::nullopt;

template <typename T>
inline constexpr optional<std::decay_t<T>> make_optional(T&& value) {
    return std::experimental::make_optional(std::forward<T>(value));
}

template <typename CharT, typename Traits = std::char_traits<CharT>>
using basic_string_view = std::experimental::basic_string_view<CharT, Traits>;

template <typename CharT, typename Traits = std::char_traits<CharT>>
std::string string_view_to_string(const basic_string_view<CharT, Traits>& v) {
    return v.to_string();
}

template <typename... Types>
using variant = boost::variant<Types...>;

template<typename U, typename... Types>
U& get(variant<Types...>& v) {
    return boost::get<U, Types...>(v);
}

template<typename U, typename... Types>
U&& get(variant<Types...>&& v) {
    return boost::get<U, Types...>(v);
}

template<typename U, typename... Types>
const U& get(const variant<Types...>& v) {
    return boost::get<U, Types...>(v);
}

template<typename U, typename... Types>
const U&& get(const variant<Types...>&& v) {
    return boost::get<U, Types...>(v);
}

template<typename U, typename... Types>
U* get_if(variant<Types...>* v) {
    return boost::get<U, Types...>(v);
}

template<typename U, typename... Types>
const U* get_if(const variant<Types...>* v) {
    return boost::get<U, Types...>(v);
}

#endif

using string_view = basic_string_view<char>;

} // namespace compat

/// \endcond

} // namespace seastar

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

#if __cplusplus >= 201703L && __has_include(<filesystem>)
#include <filesystem>
#else
#include <experimental/filesystem>
#endif


#if __cplusplus >= 201703L && __has_include(<memory_resource>)

#define SEASTAR_HAS_POLYMORPHIC_ALLOCATOR

#include <memory_resource>

namespace seastar {

/// \cond internal

namespace compat {

using memory_resource = std::pmr::memory_resource;

template<typename T>
using polymorphic_allocator = std::pmr::polymorphic_allocator<T>;

static inline
memory_resource* pmr_get_default_resource() {
    return std::pmr::get_default_resource();
}

}

}

#elif __cplusplus >= 201703L && __has_include(<experimental/memory_resource>)

#define SEASTAR_HAS_POLYMORPHIC_ALLOCATOR

#include <experimental/memory_resource>

namespace seastar {

/// \cond internal

namespace compat {

using memory_resource = std::experimental::pmr::memory_resource;

template<typename T>
using polymorphic_allocator = std::experimental::pmr::polymorphic_allocator<T>;

static inline
memory_resource* pmr_get_default_resource() {
    return std::experimental::pmr::get_default_resource();
}

}

}

#else

namespace seastar {

/// \cond internal

namespace compat {

// In C++14 we do not intend to support custom allocator, but only to
// allow default allocator to work.
//
// This is done by defining a minimal polymorphic_allocator that always aborts
// and rely on the fact that if the memory allocator is the default allocator,
// we wouldn't actually use it, but would allocate memory ourselves.
//
// Hence C++14 users would be able to use seastar, but not to use a custom allocator

class memory_resource {};

static inline
memory_resource* pmr_get_default_resource() {
    static memory_resource stub;
    return &stub;
}

template <typename T>
class polymorphic_allocator {
public:
    explicit polymorphic_allocator(memory_resource*){}
    T* allocate( std::size_t n ) { __builtin_abort(); }
    void deallocate(T* p, std::size_t n ) { __builtin_abort(); }
};

}

}

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

#if defined(__cpp_lib_filesystem)
namespace filesystem = std::filesystem;
#elif defined(__cpp_lib_experimental_filesystem)
namespace filesystem = std::experimental::filesystem;
#else
#error No filesystem header detected.
#endif

using string_view = basic_string_view<char>;

} // namespace compat

/// \endcond

} // namespace seastar

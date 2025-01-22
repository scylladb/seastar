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
 * Copyright (C) 2016 Scylla DB Ltd
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <chrono>
#include <concepts>
#include <functional>
#include <typeindex>
#endif
#include <seastar/core/sstring.hh>
#include <seastar/core/function_traits.hh>
#include <seastar/util/modules.hh>

/// \file

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN
constexpr unsigned max_scheduling_groups() { return SEASTAR_SCHEDULING_GROUPS_COUNT; }

template <typename T = void>
class future;

class reactor;

class scheduling_group;
class scheduling_group_key;

using sched_clock = std::chrono::steady_clock;
SEASTAR_MODULE_EXPORT_END

namespace internal {

// Returns an index between 0 and max_scheduling_groups()
unsigned scheduling_group_index(scheduling_group sg) noexcept;
scheduling_group scheduling_group_from_index(unsigned index) noexcept;

unsigned long scheduling_group_key_id(scheduling_group_key) noexcept;

template<typename T>
T* scheduling_group_get_specific_ptr(scheduling_group sg, scheduling_group_key key) noexcept;

}

SEASTAR_MODULE_EXPORT_BEGIN

/// Creates a scheduling group with a specified number of shares.
///
/// The operation is global and affects all shards. The returned scheduling
/// group can then be used in any shard.
///
/// \param name A name that identifiers the group; will be used as a label
///             in the group's metrics
/// \param shares number of shares of the CPU time allotted to the group;
///              Use numbers in the 1-1000 range (but can go above).
/// \return a scheduling group that can be used on any shard
future<scheduling_group> create_scheduling_group(sstring name, float shares) noexcept;

/// Creates a scheduling group with a specified number of shares.
///
/// The operation is global and affects all shards. The returned scheduling
/// group can then be used in any shard.
///
/// \param name A name that identifies the group; will be used as a label
///             in the group's metrics
/// \param shortname A name that identifies the group; will be printed in the
///                  logging message aside of the shard id. please note, the
///                  \c shortname will be truncated to 4 characters.
/// \param shares number of shares of the CPU time allotted to the group;
///              Use numbers in the 1-1000 range (but can go above).
/// \return a scheduling group that can be used on any shard
future<scheduling_group> create_scheduling_group(sstring name, sstring shortname, float shares) noexcept;

/// Destroys a scheduling group.
///
/// Destroys a \ref scheduling_group previously created with create_scheduling_group().
/// The destroyed group must not be currently in use and must not be used later.
///
/// The operation is global and affects all shards.
///
/// \param sg The scheduling group to be destroyed
/// \return a future that is ready when the scheduling group has been torn down
future<> destroy_scheduling_group(scheduling_group sg) noexcept;

/// Rename scheduling group.
///
/// Renames a \ref scheduling_group previously created with create_scheduling_group().
///
/// The operation is global and affects all shards.
/// The operation affects the exported statistics labels.
///
/// \param sg The scheduling group to be renamed
/// \param new_name The new name for the scheduling group.
/// \return a future that is ready when the scheduling group has been renamed
future<> rename_scheduling_group(scheduling_group sg, sstring new_name) noexcept;
/// Rename scheduling group.
///
/// Renames a \ref scheduling_group previously created with create_scheduling_group().
///
/// The operation is global and affects all shards.
/// The operation affects the exported statistics labels.
///
/// \param sg The scheduling group to be renamed
/// \param new_name The new name for the scheduling group.
/// \param new_shortname The new shortname for the scheduling group.
/// \return a future that is ready when the scheduling group has been renamed
future<> rename_scheduling_group(scheduling_group sg, sstring new_name, sstring new_shortname) noexcept;


/**
 * Represents a configuration for a specific scheduling group value,
 * it contains all that is needed to maintain a scheduling group specific
 * value when it needs to be created, due to, for example, a new
 * \ref scheduling_group being created.
 *
 * @note is is recomended to use @ref make_scheduling_group_key_config in order to
 * create and configure this syructure. The only reason that one might want to not use
 * this method is because of a need for specific intervention in the construction or
 * destruction of the value. Even then, it is recommended to first create the configuration
 * with @ref make_scheduling_group_key_config and only the change it.
 *
 */
struct scheduling_group_key_config {
    /**
     * Constructs a default configuration
     */
    scheduling_group_key_config() :
        scheduling_group_key_config(typeid(void)) {}
    /**
     * Creates a configuration that is made for a specific type.
     * It does not contain the right alignment and allocation sizes
     * neither the correct construction or destruction logic, but only
     * the indication for the intended type which is used in debug mode
     * to make sure that the correct type is reffered to when accessing
     * the value.
     * @param type_info - the type information class (create with typeid(T)).
     */
    scheduling_group_key_config(const std::type_info& type_info) :
            type_index(type_info) {}
    /// The allocation size for the value (usually: sizeof(T))
    size_t allocation_size;
    /// The required alignment of the value (usually: alignof(T))
    size_t alignment;
    /// Holds the type information for debug mode runtime validation
    std::type_index type_index;
    /// A function that will be called for each newly allocated value
    std::function<void (void*)> constructor;
    /// A function that will be called for each value after the scheduling group is renamed.
    std::function<void (void*)> rename;
    /// A function that will be called for each element that is about
    /// to be dealocated.
    std::function<void (void*)> destructor;

};


/**
 * A class that is intended to encapsulate the scheduling group specific
 * key and "hide" it implementation concerns and details.
 *
 * @note this object can be copied accross shards and scheduling groups.
 */
class scheduling_group_key {
public:
    /// The only user allowed operation on a key is copying.
    scheduling_group_key(const scheduling_group_key&) noexcept = default;
    scheduling_group_key(scheduling_group_key&&) noexcept = default;
private:
    scheduling_group_key(unsigned long id) noexcept :
        _id(id) {}
    unsigned long _id;
    unsigned long id() const noexcept {
        return _id;
    }
    friend future<scheduling_group_key> scheduling_group_key_create(scheduling_group_key_config cfg) noexcept;
    template<typename T>
    friend T* internal::scheduling_group_get_specific_ptr(scheduling_group sg, scheduling_group_key key) noexcept;
    template<typename T>
    friend T& scheduling_group_get_specific(scheduling_group_key key) noexcept;

    friend unsigned long internal::scheduling_group_key_id(scheduling_group_key key) noexcept;
};

SEASTAR_MODULE_EXPORT_END
namespace internal {

inline unsigned long scheduling_group_key_id(scheduling_group_key key) noexcept {
    return key.id();
}

/**
 * @brief A function in the spirit of Cpp17 apply, but specifically for constructors.
 * This function is used in order to preserve support in Cpp14.

 * @tparam ConstructorType - the constructor type or in other words the type to be constructed
 * @tparam Tuple - T params tuple type (should be deduced)
 * @tparam size_t...Idx - a sequence of indexes in order to access the typpels members in compile time.
 * (should be deduced)
 *
 * @param pre_alocated_mem - a pointer to the pre allocated memory chunk that will hold the
 * the initialized object.
 * @param args - A tupple that holds the prarameters for the constructor
 * @param idx_seq - An index sequence that will be used to access the members of the tuple in compile
 * time.
 *
 * @note this function was not intended to be called by users and it is only a utility function
 * for suporting \ref make_scheduling_group_key_config
 */
template<typename ConstructorType, typename Tuple, size_t...Idx>
void apply_constructor(void* pre_alocated_mem, Tuple args, std::index_sequence<Idx...>) {
    new (pre_alocated_mem) ConstructorType(std::get<Idx>(args)...);
}
}
SEASTAR_MODULE_EXPORT_BEGIN

/**
 * A template function that builds a scheduling group specific value configuration.
 * This configuration is used by the infrastructure to allocate memory for the values
 * and initialize or deinitialize them when they are created or destroyed.
 *
 * If the type T has a member function T::rename()
 * then it will be called after the scheduling group is renamed.
 *
 * @tparam T - the type for the newly created value.
 * @tparam ...ConstructorArgs - the types for the constructor parameters (should be deduced)
 * @param args - The parameters for the constructor.
 * @return a fully initialized \ref scheduling_group_key_config object.
 */
template <typename T, typename... ConstructorArgs>
scheduling_group_key_config
make_scheduling_group_key_config(ConstructorArgs... args) {
    scheduling_group_key_config sgkc(typeid(T));
    sgkc.allocation_size = sizeof(T);
    sgkc.alignment = alignof(T);
    sgkc.constructor = [args = std::make_tuple(args...)] (void* p) {
        internal::apply_constructor<T>(p, args, std::make_index_sequence<sizeof...(ConstructorArgs)>());
    };
    sgkc.destructor = [] (void* p) {
        static_cast<T*>(p)->~T();
    };
    if constexpr (requires(T key) { key.rename(); }) {
        sgkc.rename = [] (void* p) {
            static_cast<T*>(p)->rename();
        };
    }
    return sgkc;
}

/**
 * Returns a future that holds a scheduling key and resolves when this key can be used
 * to access the scheduling group specific value it represents.
 * @param cfg - A \ref scheduling_group_key_config object (by recomendation: initialized with
 * \ref make_scheduling_group_key_config )
 * @return A future containing \ref scheduling_group_key for the newly created specific value.
 */
future<scheduling_group_key> scheduling_group_key_create(scheduling_group_key_config cfg) noexcept;

/**
 * Returnes a reference to the given scheduling group specific value
 * @tparam T - the type of the scheduling specific type (cannot be deduced)
 * @param sg - the scheduling group which it's specific value to retrieve
 * @param key - the key of the value to retrieve.
 * @return A reference to the scheduling specific value.
 */
template<typename T>
T& scheduling_group_get_specific(scheduling_group sg, scheduling_group_key key);


/// \brief Identifies function calls that are accounted as a group
///
/// A `scheduling_group` is a tag that can be used to mark a function call.
/// Executions of such tagged calls are accounted as a group.
class scheduling_group {
    unsigned _id;
private:
    explicit scheduling_group(unsigned id) noexcept : _id(id) {}
public:
    /// Creates a `scheduling_group` object denoting the default group
    constexpr scheduling_group() noexcept : _id(0) {} // must be constexpr for current_scheduling_group_holder
    bool active() const noexcept;
    const sstring& name() const noexcept;
    const sstring& short_name() const noexcept;
    bool operator==(scheduling_group x) const noexcept { return _id == x._id; }
    bool operator!=(scheduling_group x) const noexcept { return _id != x._id; }
    bool is_main() const noexcept { return _id == 0; }
    bool is_at_exit() const noexcept { return _id == 1; }
    template<typename T>
    /**
     * Returnes a reference to this scheduling group specific value
     * @tparam T - the type of the scheduling specific type (cannot be deduced)
     * @param key - the key of the value to retrieve.
     * @return A reference to this scheduling specific value.
     */
    T& get_specific(scheduling_group_key key) noexcept {
        return *internal::scheduling_group_get_specific_ptr<T>(*this, key);
    }
    /// Adjusts the number of shares allotted to the group.
    ///
    /// Dynamically adjust the number of shares allotted to the group, increasing or
    /// decreasing the amount of CPU bandwidth it gets. The adjustment is local to
    /// the shard.
    ///
    /// This can be used to reduce a background job's interference with a foreground
    /// load: the shares can be started at a low value, increased when the background
    /// job's backlog increases, and reduced again when the backlog decreases.
    ///
    /// \param shares number of shares allotted to the group. Use numbers
    ///               in the 1-1000 range.
    void set_shares(float shares) noexcept;

    /// Returns the number of shares the group has
    ///
    /// Similarly to the \ref set_shares, the returned value is only relevant to
    /// the calling shard
    float get_shares() const noexcept;

    /// \brief Updates the current IO bandwidth for a given scheduling group
    ///
    /// The bandwidth applied is NOT shard-local, instead it is applied so that
    /// all shards cannot consume more bytes-per-second altogether
    ///
    /// \param bandwidth the new bandwidth value in bytes/second
    /// \return a future that is ready when the bandwidth update is applied
    future<> update_io_bandwidth(uint64_t bandwidth) const;

    friend future<scheduling_group> create_scheduling_group(sstring name, sstring shortname, float shares) noexcept;
    friend future<> destroy_scheduling_group(scheduling_group sg) noexcept;
    friend future<> rename_scheduling_group(scheduling_group sg, sstring new_name, sstring new_shortname) noexcept;
    friend class reactor;
    friend unsigned internal::scheduling_group_index(scheduling_group sg) noexcept;
    friend scheduling_group internal::scheduling_group_from_index(unsigned index) noexcept;

    template<typename SpecificValType, typename Mapper, typename Reducer, typename Initial>
    requires requires(SpecificValType specific_val, Mapper mapper, Reducer reducer, Initial initial) {
        {reducer(initial, mapper(specific_val))} -> std::convertible_to<Initial>;
    }
    friend future<typename function_traits<Reducer>::return_type>
    map_reduce_scheduling_group_specific(Mapper mapper, Reducer reducer, Initial initial_val, scheduling_group_key key);

    template<typename SpecificValType, typename Reducer, typename Initial>
    requires requires(SpecificValType specific_val, Reducer reducer, Initial initial) {
        {reducer(initial, specific_val)} -> std::convertible_to<Initial>;
    }
    friend future<typename function_traits<Reducer>::return_type>
        reduce_scheduling_group_specific(Reducer reducer, Initial initial_val, scheduling_group_key key);


};

/// \cond internal
SEASTAR_MODULE_EXPORT_END
namespace internal {

inline
unsigned
scheduling_group_index(scheduling_group sg) noexcept {
    return sg._id;
}

inline
scheduling_group
scheduling_group_from_index(unsigned index) noexcept {
    return scheduling_group(index);
}

#ifdef SEASTAR_BUILD_SHARED_LIBS
scheduling_group*
current_scheduling_group_ptr() noexcept;
#else
inline
scheduling_group*
current_scheduling_group_ptr() noexcept {
    // Slow unless constructor is constexpr
    static thread_local scheduling_group sg;
    return &sg;
}
#endif
}
/// \endcond

SEASTAR_MODULE_EXPORT_BEGIN
/// Returns the current scheduling group
inline
scheduling_group
current_scheduling_group() noexcept {
    return *internal::current_scheduling_group_ptr();
}

inline
scheduling_group
default_scheduling_group() noexcept {
    return scheduling_group();
}

SEASTAR_MODULE_EXPORT_END

inline
bool
scheduling_group::active() const noexcept {
    return *this == current_scheduling_group();
}

}

namespace std {

SEASTAR_MODULE_EXPORT
template <>
struct hash<seastar::scheduling_group> {
    size_t operator()(seastar::scheduling_group sg) const noexcept {
        return seastar::internal::scheduling_group_index(sg);
    }
};

}

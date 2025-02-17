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
 * Copyright (C) 2019 Scylla DB Ltd
 */

#ifndef SEASTAR_MODULE
#include <seastar/core/scheduling.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/modules.hh>
#include <array>
#include <map>
#include <typeindex>
#include <vector>
#include <ranges>
#endif

#pragma once

namespace seastar {

namespace internal {

struct scheduling_group_specific_thread_local_data {
    using val_ptr = std::unique_ptr<void, void (*)(void*) noexcept>;
    using cfg_ptr = lw_shared_ptr<scheduling_group_key_config>;

    struct specific_val {
        val_ptr valp;
        cfg_ptr cfg;

        specific_val() : valp(nullptr, &free), cfg(nullptr) {}

        specific_val(val_ptr&& valp_, const cfg_ptr& cfg_) : valp(std::move(valp_)), cfg(cfg_) {
            if (valp && cfg->constructor) {
                cfg->constructor(valp.get());
            }
        }

        ~specific_val() {
            if (valp && cfg->destructor) {
                cfg->destructor(valp.get());
            }
        }

        specific_val(const specific_val& other) = delete;
        specific_val& operator=(const specific_val& other) = delete;

        specific_val(specific_val&& other) : valp(std::move(other.valp)), cfg(std::move(other.cfg)) {}

        specific_val& operator=(specific_val&& other) {
            if (this != &other) {
                valp = std::move(other.valp);
                cfg = std::move(other.cfg);
            }
            return *this;
        }

        void* get() { return valp.get(); }

        void rename() {
            if (valp && cfg->rename) {
                cfg->rename(valp.get());
            }
        }
    };

    struct per_scheduling_group {
        bool queue_is_initialized = false;
        /**
         * This array holds pointers to the scheduling group specific
         * data. The pointer is not use as is but is cast to a reference
         * to the appropriate type that is actually pointed to.
         */
        std::vector<specific_val> specific_vals;

        void rename() {
            for (auto& v : specific_vals) {
                v.rename();
            }
        }
    };
    std::array<per_scheduling_group, max_scheduling_groups()> per_scheduling_group_data;
    std::map<unsigned long, cfg_ptr> scheduling_group_key_configs;
};

#ifdef SEASTAR_BUILD_SHARED_LIBS
scheduling_group_specific_thread_local_data** get_scheduling_group_specific_thread_local_data_ptr() noexcept;
#else
inline
scheduling_group_specific_thread_local_data** get_scheduling_group_specific_thread_local_data_ptr() noexcept {
    static thread_local scheduling_group_specific_thread_local_data* data;
    return &data;
}
#endif
inline
scheduling_group_specific_thread_local_data& get_scheduling_group_specific_thread_local_data() noexcept {
    return **get_scheduling_group_specific_thread_local_data_ptr();
}

[[noreturn]] void no_such_scheduling_group(scheduling_group sg);

/**
 * Returns a pointer to the given scheduling group specific data.
 * @param sg - The scheduling group which it's data needs to be accessed
 * @param key - The scheduling group key that for the data to access
 * @return A pointer of type T* to the data, if sg is valid initialized.
 *
 * @note The parameter T has to be given since there is no way to deduce it.
 */
template<typename T>
T* scheduling_group_get_specific_ptr(scheduling_group sg, scheduling_group_key key) noexcept {
    auto& data = internal::get_scheduling_group_specific_thread_local_data();
#ifdef SEASTAR_DEBUG
    SEASTAR_ASSERT(std::type_index(typeid(T)) == data.scheduling_group_key_configs[key.id()]->type_index);
#endif
    auto sg_id = internal::scheduling_group_index(sg);
    if (__builtin_expect(sg_id < data.per_scheduling_group_data.size() &&
            data.per_scheduling_group_data[sg_id].queue_is_initialized, true)) {
        return reinterpret_cast<T*>(data.per_scheduling_group_data[sg_id].specific_vals[key.id()].get());
    }
    return nullptr;
}

}

SEASTAR_MODULE_EXPORT_BEGIN

/**
 * Returns a reference to the given scheduling group specific data.
 * @param sg - The scheduling group which it's data needs to be accessed
 * @param key - The scheduling group key that for the data to access
 * @return A reference of type T& to the data.
 *
 * @note The parameter T has to be given since there is no way to deduce it.
 *       May throw std::invalid_argument if sg does not exist or is uninitialized.
 */
template<typename T>
T& scheduling_group_get_specific(scheduling_group sg, scheduling_group_key key) {
    T* p = internal::scheduling_group_get_specific_ptr<T>(sg, std::move(key));
    if (!p) {
        internal::no_such_scheduling_group(sg);
    }
    return *p;
}

/**
 * Returns a reference to the current specific data.
 * @param key - The scheduling group key that for the data to access
 * @return A reference of type T& to the data.
 *
 * @note The parameter T has to be given since there is no way to deduce it.
 */
template<typename T>
T& scheduling_group_get_specific(scheduling_group_key key) noexcept {
    // Unlike internal::scheduling_group_get_specific_ptr, this can
    // return a reference to an element whose queue_is_initialized is
    // false.
    auto& data = internal::get_scheduling_group_specific_thread_local_data();
    SEASTAR_ASSERT(std::type_index(typeid(T)) == data.scheduling_group_key_configs[key.id()]->type_index);
    auto sg_id = internal::scheduling_group_index(current_scheduling_group());
    return *reinterpret_cast<T*>(data.per_scheduling_group_data[sg_id].specific_vals[key.id()].get());
}

/**
 * A map reduce over all values of a specific scheduling group data.
 * @param mapper -  A functor SomeType(SpecificValType&) or SomeType(SpecificValType) that maps
 * the specific data to a value of any type.
 * @param reducer - A functor of of type ConvetibleToInitial(Initial, MapperReurnType) that reduces
 * a value of type Initial and of the mapper return type to a value of type convertible to Initial.
 * @param initial_val - the initial value to pass in the first call to the reducer.
 * @param key - the key to the specific data that the mapper should act upon.
 * @return A future that resolves when the result of the map reduce is ready.
 * @note The type of SpecificValType must be given because there is no way to deduce it in a *consistent*
 * manner.
 * @note Theoretically the parameter type of Mapper can be deduced to be the type (function_traits<Mapper>::arg<0>)
 * but then there is a danger when the Mapper accepts a parameter type T where SpecificValType is convertible to
 * SpecificValType.
 */
template<typename SpecificValType, typename Mapper, typename Reducer, typename Initial>
requires requires(SpecificValType specific_val, Mapper mapper, Reducer reducer, Initial initial) {
    {reducer(initial, mapper(specific_val))} -> std::convertible_to<Initial>;
}
future<typename function_traits<Reducer>::return_type>
map_reduce_scheduling_group_specific(Mapper mapper, Reducer reducer,
        Initial initial_val, scheduling_group_key key) {
    using per_scheduling_group = internal::scheduling_group_specific_thread_local_data::per_scheduling_group;
    auto& data = internal::get_scheduling_group_specific_thread_local_data();
    auto wrapped_mapper = [key, mapper] (per_scheduling_group& psg) {
        auto id = internal::scheduling_group_key_id(key);
        return make_ready_future<typename function_traits<Mapper>::return_type>
            (mapper(*reinterpret_cast<SpecificValType*>(psg.specific_vals[id].get())));
    };

    return map_reduce(
            data.per_scheduling_group_data
            | std::views::filter(std::mem_fn(&per_scheduling_group::queue_is_initialized)),
            wrapped_mapper, std::move(initial_val), reducer);
}

/**
 * A reduce over all values of a specific scheduling group data.
 * @param reducer - A functor of of type ConvetibleToInitial(Initial, SpecificValType) that reduces
 * a value of type Initial and of the sg specific data type to a value of type convertible to Initial.
 * @param initial_val - the initial value to pass in the first call to the reducer.
 * @param key - the key to the specific data that the mapper should act upon.
 * @return A future that resolves when the result of the reduce is ready.
 * * @note The type of SpecificValType must be given because there is no way to deduce it in a *consistent*
 * manner.
 * @note Theoretically the parameter type of Reducer can be deduced to be the type (function_traits<Reducer>::arg<0>)
 * but then there is a danger when the Reducer accepts a parameter type T where SpecificValType is convertible to
 * SpecificValType.
 */
template<typename SpecificValType, typename Reducer, typename Initial>
requires requires(SpecificValType specific_val, Reducer reducer, Initial initial) {
    {reducer(initial, specific_val)} -> std::convertible_to<Initial>;
}
future<typename function_traits<Reducer>::return_type>
reduce_scheduling_group_specific(Reducer reducer, Initial initial_val, scheduling_group_key key) {
    using per_scheduling_group = internal::scheduling_group_specific_thread_local_data::per_scheduling_group;
    auto& data = internal::get_scheduling_group_specific_thread_local_data();

    auto mapper = [key] (per_scheduling_group& psg) {
        auto id = internal::scheduling_group_key_id(key);
        return make_ready_future<SpecificValType>(*reinterpret_cast<SpecificValType*>(psg.specific_vals[id].get()));
    };

    return map_reduce(
            data.per_scheduling_group_data
            | std::views::filter(std::mem_fn(&per_scheduling_group::queue_is_initialized)),
            mapper, std::move(initial_val), reducer);
}

SEASTAR_MODULE_EXPORT_END

}

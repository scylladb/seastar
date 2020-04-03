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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#pragma once

#include <seastar/core/smp.hh>
#include <seastar/core/future-util.hh>
#include <seastar/util/is_smart_ptr.hh>
#include <seastar/util/tuple_utils.hh>
#include <seastar/core/do_with.hh>
#include <boost/iterator/counting_iterator.hpp>
#include <functional>

/// \defgroup smp-module Multicore
///
/// \brief Support for exploiting multiple cores on a server.
///
/// Seastar supports multicore servers by using *sharding*.  Each logical
/// core (lcore) runs a separate event loop, with its own memory allocator,
/// TCP/IP stack, and other services.  Shards communicate by explicit message
/// passing, rather than using locks and condition variables as with traditional
/// threaded programming.

namespace seastar {

/// \addtogroup smp-module
/// @{

template <typename T>
class sharded;

/// If sharded service inherits from this class sharded::stop() will wait
/// until all references to a service on each shard will disappear before
/// returning. It is still service's own responsibility to track its references
/// in asynchronous code by calling shared_from_this() and keeping returned smart
/// pointer as long as object is in use.
template<typename T>
class async_sharded_service : public enable_shared_from_this<T> {
protected:
    std::function<void()> _delete_cb;
    virtual ~async_sharded_service() {
        if (_delete_cb) {
            _delete_cb();
        }
    }
    template <typename Service> friend class sharded;
};


/// \brief Provide a sharded service with access to its peers
///
/// If a service class inherits from this, it will gain a \code container()
/// \endcode method that provides access to the \ref sharded object, with which
/// it can call its peers.
template <typename Service>
class peering_sharded_service {
    sharded<Service>* _container = nullptr;
private:
    template <typename T> friend class sharded;
    void set_container(sharded<Service>* container) { _container = container; }
public:
    peering_sharded_service() = default;
    peering_sharded_service(peering_sharded_service<Service>&&) = default;
    peering_sharded_service(const peering_sharded_service<Service>&) = delete;
    peering_sharded_service& operator=(const peering_sharded_service<Service>&) = delete;
    sharded<Service>& container() { return *_container; }
    const sharded<Service>& container() const { return *_container; }
};


/// Exception thrown when a \ref sharded object does not exist
class no_sharded_instance_exception : public std::exception {
public:
    virtual const char* what() const noexcept override {
        return "sharded instance does not exist";
    }
};

/// Template helper to distribute a service across all logical cores.
///
/// The \c sharded template manages a sharded service, by creating
/// a copy of the service on each logical core, providing mechanisms to communicate
/// with each shard's copy, and a way to stop the service.
///
/// \tparam Service a class to be instantiated on each core.  Must expose
///         a \c stop() method that returns a \c future<>, to be called when
///         the service is stopped.
template <typename Service>
class sharded {
    struct entry {
        shared_ptr<Service> service;
        promise<> freed;
    };
    std::vector<entry> _instances;
private:
    using invoke_on_all_func_type = std::function<future<> (Service&)>;
private:
    void service_deleted() {
        _instances[this_shard_id()].freed.set_value();
    }
    template <typename U, bool async>
    friend struct shared_ptr_make_helper;

    template <typename T>
    std::enable_if_t<std::is_base_of<peering_sharded_service<T>, T>::value>
    set_container(T& service) {
        service.set_container(this);
    }

    template <typename T>
    std::enable_if_t<!std::is_base_of<peering_sharded_service<T>, T>::value>
    set_container(T& service) {
    }
public:
    /// Constructs an empty \c sharded object.  No instances of the service are
    /// created.
    sharded() {}
    sharded(const sharded& other) = delete;
    sharded& operator=(const sharded& other) = delete;
    /// Sharded object with T that inherits from peering_sharded_service
    /// cannot be moved safely, so disable move operations.
    sharded(sharded&& other) noexcept = delete;
    sharded& operator=(sharded&& other) = delete;
    /// Destroyes a \c sharded object.  Must not be in a started state.
    ~sharded();

    /// Starts \c Service by constructing an instance on every logical core
    /// with a copy of \c args passed to the constructor.
    ///
    /// \param args Arguments to be forwarded to \c Service constructor
    /// \return a \ref future<> that becomes ready when all instances have been
    ///         constructed.
    template <typename... Args>
    future<> start(Args&&... args);

    /// Starts \c Service by constructing an instance on a single logical core
    /// with a copy of \c args passed to the constructor.
    ///
    /// \param args Arguments to be forwarded to \c Service constructor
    /// \return a \ref future<> that becomes ready when the instance has been
    ///         constructed.
    template <typename... Args>
    future<> start_single(Args&&... args);

    /// Stops all started instances and destroys them.
    ///
    /// For every started instance, its \c stop() method is called, and then
    /// it is destroyed.
    future<> stop();

    /// Invoke a type-erased function on all instances of @Service.
    /// The return value becomes ready when all instances have processed
    /// the message.
    ///
    /// \param options the options to forward to the \ref smp::submit_to()
    ///         called behind the scenes.
    /// \param func Function to be invoked on all shards
    /// \return Future that becomes ready once all calls have completed
    future<> invoke_on_all(smp_submit_to_options options, std::function<future<> (Service&)> func);

    /// Invoke a type-erased function on all instances of @Service.
    /// The return value becomes ready when all instances have processed
    /// the message.
    /// Passes the default \ref smp_submit_to_options to the
    /// \ref smp::submit_to() called behind the scenes.
    future<> invoke_on_all(std::function<future<> (Service&)> func) {
        return invoke_on_all(smp_submit_to_options{}, std::move(func));
    }

    /// Invoke a method on all instances of @Service.
    /// The return value becomes ready when all instances have processed
    /// the message.
    ///
    /// \param options the options to forward to the \ref smp::submit_to()
    ///         called behind the scenes.
    /// \param func Member function of \c Service to be invoked on all shards
    /// \return Future that becomes ready once all calls have completed
    template <typename... Args>
    future<> invoke_on_all(smp_submit_to_options options, future<> (Service::*func)(Args...), Args... args);

    /// Invoke a method on all instances of @Service.
    /// The return value becomes ready when all instances have processed
    /// the message.
    /// Passes the default \ref smp_submit_to_options to the
    /// \ref smp::submit_to() called behind the scenes.
    template <typename... Args>
    future<> invoke_on_all(future<> (Service::*func)(Args...), Args... args) {
        return invoke_on_all(smp_submit_to_options{}, func, std::move(args)...);
    }

    /// Invoke a method on all \c Service instances in parallel.
    ///
    /// \param options the options to forward to the \ref smp::submit_to()
    ///         called behind the scenes.
    /// \param func member function to be called.  Must return \c void or
    ///             \c future<>.
    /// \param args arguments to be passed to \c func.
    /// \return future that becomes ready when the method has been invoked
    ///         on all instances.
    template <typename... Args>
    future<> invoke_on_all(smp_submit_to_options options, void (Service::*func)(Args...), Args... args);

    /// Invoke a method on all \c Service instances in parallel.
    ///
    /// \param func member function to be called.  Must return \c void or
    ///             \c future<>.
    /// \param args arguments to be passed to \c func.
    /// \return future that becomes ready when the method has been invoked
    ///         on all instances.
    ///
    /// Passes the default \ref smp_submit_to_options to the
    /// \ref smp::submit_to() called behind the scenes.
    template <typename... Args>
    future<> invoke_on_all(void (Service::*func)(Args...), Args... args) {
        return invoke_on_all(smp_submit_to_options{}, func, std::move(args)...);
    }

    /// Invoke a callable on all instances of  \c Service.
    ///
    /// \param options the options to forward to the \ref smp::submit_to()
    ///         called behind the scenes.
    /// \param func a callable with the signature `void (Service&)`
    ///             or `future<> (Service&)`, to be called on each core
    ///             with the local instance as an argument.
    /// \return a `future<>` that becomes ready when all cores have
    ///         processed the message.
    template <typename Func>
    future<> invoke_on_all(smp_submit_to_options options, Func&& func);

    /// Invoke a callable on all instances of  \c Service.
    ///
    /// \param func a callable with the signature `void (Service&)`
    ///             or `future<> (Service&)`, to be called on each core
    ///             with the local instance as an argument.
    /// \return a `future<>` that becomes ready when all cores have
    ///         processed the message.
    ///
    /// Passes the default \ref smp_submit_to_options to the
    /// \ref smp::submit_to() called behind the scenes.
    template <typename Func>
    future<> invoke_on_all(Func&& func) {
        return invoke_on_all(smp_submit_to_options{}, std::forward<Func>(func));
    }

    /// Invoke a callable on all instances of  \c Service except the instance
    /// which is allocated on current shard.
    ///
    /// \param options the options to forward to the \ref smp::submit_to()
    ///         called behind the scenes.
    /// \param func a callable with the signature `void (Service&)`
    ///             or `future<> (Service&)`, to be called on each core
    ///             with the local instance as an argument.
    /// \return a `future<>` that becomes ready when all cores but the current one have
    ///         processed the message.
    template <typename Func>
    future<> invoke_on_others(smp_submit_to_options options, Func&& func);

    /// Invoke a callable on all instances of  \c Service except the instance
    /// which is allocated on current shard.
    ///
    /// \param func a callable with the signature `void (Service&)`
    ///             or `future<> (Service&)`, to be called on each core
    ///             with the local instance as an argument.
    /// \return a `future<>` that becomes ready when all cores but the current one have
    ///         processed the message.
    ///
    /// Passes the default \ref smp_submit_to_options to the
    /// \ref smp::submit_to() called behind the scenes.
    template <typename Func>
    future<> invoke_on_others(Func&& func) {
        return invoke_on_others(smp_submit_to_options{}, std::forward<Func>(func));
    }

    /// Invoke a method on all instances of `Service` and reduce the results using
    /// `Reducer`.
    ///
    /// \see map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Reducer&& r)
    template <typename Reducer, typename Ret, typename... FuncArgs, typename... Args>
    inline
    auto
    map_reduce(Reducer&& r, Ret (Service::*func)(FuncArgs...), Args&&... args)
        -> typename reducer_traits<Reducer>::future_type
    {
        return ::seastar::map_reduce(boost::make_counting_iterator<unsigned>(0),
                            boost::make_counting_iterator<unsigned>(_instances.size()),
            [this, func, args = std::make_tuple(std::forward<Args>(args)...)] (unsigned c) mutable {
                return smp::submit_to(c, [this, func, args] () mutable {
                    return apply([this, func] (Args&&... args) mutable {
                        auto inst = _instances[this_shard_id()].service;
                        if (inst) {
                            return ((*inst).*func)(std::forward<Args>(args)...);
                        } else {
                            throw no_sharded_instance_exception();
                        }
                    }, std::move(args));
                });
            }, std::forward<Reducer>(r));
    }

    /// Invoke a callable on all instances of `Service` and reduce the results using
    /// `Reducer`.
    ///
    /// \see map_reduce(Iterator begin, Iterator end, Mapper&& mapper, Reducer&& r)
    template <typename Reducer, typename Func>
    inline
    auto map_reduce(Reducer&& r, Func&& func) -> typename reducer_traits<Reducer>::future_type
    {
        return ::seastar::map_reduce(boost::make_counting_iterator<unsigned>(0),
                            boost::make_counting_iterator<unsigned>(_instances.size()),
            [this, &func] (unsigned c) mutable {
                return smp::submit_to(c, [this, func] () mutable {
                    auto inst = get_local_service();
                    return func(*inst);
                });
            }, std::forward<Reducer>(r));
    }

    /// Applies a map function to all shards, then reduces the output by calling a reducer function.
    ///
    /// \param map callable with the signature `Value (Service&)` or
    ///               `future<Value> (Service&)` (for some `Value` type).
    ///               used as the second input to \c reduce
    /// \param initial initial value used as the first input to \c reduce.
    /// \param reduce binary function used to left-fold the return values of \c map
    ///               into \c initial .
    ///
    /// Each \c map invocation runs on the shard associated with the service.
    ///
    /// \tparam  Mapper unary function taking `Service&` and producing some result.
    /// \tparam  Initial any value type
    /// \tparam  Reduce a binary function taking two Initial values and returning an Initial
    /// \return  Result of invoking `map` with each instance in parallel, reduced by calling
    ///          `reduce()` on each adjacent pair of results.
    template <typename Mapper, typename Initial, typename Reduce>
    inline
    future<Initial>
    map_reduce0(Mapper map, Initial initial, Reduce reduce) {
        auto wrapped_map = [this, map] (unsigned c) {
            return smp::submit_to(c, [this, map] {
                auto inst = get_local_service();
                return map(*inst);
            });
        };
        return ::seastar::map_reduce(smp::all_cpus().begin(), smp::all_cpus().end(),
                            std::move(wrapped_map),
                            std::move(initial),
                            std::move(reduce));
    }

    /// Applies a map function to all shards, and return a vector of the result.
    ///
    /// \param mapper callable with the signature `Value (Service&)` or
    ///               `future<Value> (Service&)` (for some `Value` type).
    ///
    /// Each \c map invocation runs on the shard associated with the service.
    ///
    /// \tparam  Mapper unary function taking `Service&` and producing some result.
    /// \return  Result vector of invoking `map` with each instance in parallel
    template <typename Mapper, typename Future = futurize_t<std::result_of_t<Mapper(Service&)>>, typename return_type = decltype(internal::untuple(std::declval<typename Future::value_type>()))>
    inline future<std::vector<return_type>> map(Mapper mapper) {
        return do_with(std::vector<return_type>(),
                [&mapper, this] (std::vector<return_type>& vec) mutable {
            vec.resize(smp::count);
            return parallel_for_each(boost::irange<unsigned>(0, _instances.size()), [this, &vec, mapper] (unsigned c) {
                return smp::submit_to(c, [this, mapper] {
                    auto inst = get_local_service();
                    return mapper(*inst);
                }).then([&vec, c] (auto res) {
                    vec[c] = res;
                });
            }).then([&vec] {
                return make_ready_future<std::vector<return_type>>(std::move(vec));
            });
        });
    }

    /// Invoke a method on a specific instance of `Service`.
    ///
    /// \param id shard id to call
    /// \param options the options to forward to the \ref smp::submit_to()
    ///         called behind the scenes.
    /// \param func a method of `Service`
    /// \param args arguments to be passed to `func`
    /// \return result of calling `func(args)` on the designated instance
    template <typename Ret, typename... FuncArgs, typename... Args, typename FutureRet = futurize_t<Ret>>
    FutureRet
    invoke_on(unsigned id, smp_submit_to_options options, Ret (Service::*func)(FuncArgs...), Args&&... args) {
        return smp::submit_to(id, options, [this, func, args = std::make_tuple(std::forward<Args>(args)...)] () mutable {
            auto inst = get_local_service();
            return futurize_apply(std::mem_fn(func), std::tuple_cat(std::make_tuple<>(inst), std::move(args)));
        });
    }

    /// Invoke a method on a specific instance of `Service`.
    ///
    /// \param id shard id to call
    /// \param func a method of `Service`
    /// \param args arguments to be passed to `func`
    /// \return result of calling `func(args)` on the designated instance
    template <typename Ret, typename... FuncArgs, typename... Args, typename FutureRet = futurize_t<Ret>>
    FutureRet
    invoke_on(unsigned id, Ret (Service::*func)(FuncArgs...), Args&&... args) {
        return invoke_on(id, smp_submit_to_options{}, func, std::forward<Args>(args)...);
    }

    /// Invoke a callable on a specific instance of `Service`.
    ///
    /// \param id shard id to call
    /// \param options the options to forward to the \ref smp::submit_to()
    ///         called behind the scenes.
    /// \param func a callable with signature `Value (Service&)` or
    ///        `future<Value> (Service&)` (for some `Value` type)
    /// \return result of calling `func(instance)` on the designated instance
    template <typename Func, typename Ret = futurize_t<std::result_of_t<Func(Service&)>>>
    Ret
    invoke_on(unsigned id, smp_submit_to_options options, Func&& func) {
        return smp::submit_to(id, options, [this, func = std::forward<Func>(func)] () mutable {
            auto inst = get_local_service();
            return func(*inst);
        });
    }

    /// Invoke a callable on a specific instance of `Service`.
    ///
    /// \param id shard id to call
    /// \param func a callable with signature `Value (Service&)` or
    ///        `future<Value> (Service&)` (for some `Value` type)
    /// \return result of calling `func(instance)` on the designated instance
    template <typename Func, typename Ret = futurize_t<std::result_of_t<Func(Service&)>>>
    Ret
    invoke_on(unsigned id, Func&& func) {
        return invoke_on(id, smp_submit_to_options{}, std::forward<Func>(func));
    }

    /// Gets a reference to the local instance.
    const Service& local() const;

    /// Gets a reference to the local instance.
    Service& local();

    /// Gets a shared pointer to the local instance.
    shared_ptr<Service> local_shared();

    /// Checks whether the local instance has been initialized.
    bool local_is_initialized() const;

private:
    void track_deletion(shared_ptr<Service>& s, std::false_type) {
        // do not wait for instance to be deleted since it is not going to notify us
        service_deleted();
    }

    void track_deletion(shared_ptr<Service>& s, std::true_type) {
        s->_delete_cb = std::bind(std::mem_fn(&sharded<Service>::service_deleted), this);
    }

    template <typename... Args>
    shared_ptr<Service> create_local_service(Args&&... args) {
        auto s = ::seastar::make_shared<Service>(std::forward<Args>(args)...);
        set_container(*s);
        track_deletion(s, std::is_base_of<async_sharded_service<Service>, Service>());
        return s;
    }

    shared_ptr<Service> get_local_service() {
        auto inst = _instances[this_shard_id()].service;
        if (!inst) {
            throw no_sharded_instance_exception();
        }
        return inst;
    }
};

/// @}

template <typename Service>
sharded<Service>::~sharded() {
	assert(_instances.empty());
}

namespace internal {

template <typename Service>
class either_sharded_or_local {
    sharded<Service>& _sharded;
public:
    either_sharded_or_local(sharded<Service>& s) : _sharded(s) {}
    operator sharded<Service>& () { return _sharded; }
    operator Service& () { return _sharded.local(); }
};

template <typename T>
inline
T&&
unwrap_sharded_arg(T&& arg) {
    return std::forward<T>(arg);
}

template <typename Service>
either_sharded_or_local<Service>
unwrap_sharded_arg(std::reference_wrapper<sharded<Service>> arg) {
    return either_sharded_or_local<Service>(arg);
}

}

namespace internal {

using on_each_shard_func = std::function<future<> (unsigned shard)>;

future<> sharded_parallel_for_each(unsigned nr_shards, on_each_shard_func on_each_shard);

}

template <typename Service>
template <typename... Args>
future<>
sharded<Service>::start(Args&&... args) {
    _instances.resize(smp::count);
    return internal::sharded_parallel_for_each(_instances.size(),
        [this, args = std::make_tuple(std::forward<Args>(args)...)] (unsigned c) mutable {
            return smp::submit_to(c, [this, args] () mutable {
                _instances[this_shard_id()].service = apply([this] (Args... args) {
                    return create_local_service(internal::unwrap_sharded_arg(std::forward<Args>(args))...);
                }, args);
            });
    }).then_wrapped([this] (future<> f) {
        try {
            f.get();
            return make_ready_future<>();
        } catch (...) {
            return this->stop().then([e = std::current_exception()] () mutable {
                std::rethrow_exception(e);
            });
        }
    });
}

template <typename Service>
template <typename... Args>
future<>
sharded<Service>::start_single(Args&&... args) {
    assert(_instances.empty());
    _instances.resize(1);
    return smp::submit_to(0, [this, args = std::make_tuple(std::forward<Args>(args)...)] () mutable {
        _instances[0].service = apply([this] (Args... args) {
            return create_local_service(internal::unwrap_sharded_arg(std::forward<Args>(args))...);
        }, args);
    }).then_wrapped([this] (future<> f) {
        try {
            f.get();
            return make_ready_future<>();
        } catch (...) {
            return this->stop().then([e = std::current_exception()] () mutable {
                std::rethrow_exception(e);
            });
        }
    });
}

namespace internal {

// Helper check if Service::stop exists

struct sharded_has_stop {
    // If a member names "stop" exists, try to call it, even if it doesn't
    // have the correct signature. This is so that we don't ignore a function
    // named stop() just because the signature is incorrect, and instead
    // force the user to resolve the ambiguity.
    template <typename Service>
    constexpr static auto check(int) -> std::enable_if_t<(sizeof(&Service::stop) >= 0), bool> {
        return true;
    }

    // Fallback in case Service::stop doesn't exist.
    template<typename>
    static constexpr auto check(...) -> bool {
        return false;
    }
};

template <bool stop_exists>
struct sharded_call_stop {
    template <typename Service>
    static future<> call(Service& instance);
};

template <>
template <typename Service>
inline
future<> sharded_call_stop<true>::call(Service& instance) {
    return instance.stop();
}

template <>
template <typename Service>
inline
future<> sharded_call_stop<false>::call(Service& instance) {
    return make_ready_future<>();
}

template <typename Service>
inline
future<>
stop_sharded_instance(Service& instance) {
    constexpr bool has_stop = internal::sharded_has_stop::check<Service>(0);
    return internal::sharded_call_stop<has_stop>::call(instance);
}

}

template <typename Service>
future<>
sharded<Service>::stop() {
    return internal::sharded_parallel_for_each(_instances.size(), [this] (unsigned c) mutable {
        return smp::submit_to(c, [this] () mutable {
            auto inst = _instances[this_shard_id()].service;
            if (!inst) {
                return make_ready_future<>();
            }
            return internal::stop_sharded_instance(*inst);
        });
    }).then([this] {
        return internal::sharded_parallel_for_each(_instances.size(), [this] (unsigned c) {
            return smp::submit_to(c, [this] {
                _instances[this_shard_id()].service = nullptr;
                return _instances[this_shard_id()].freed.get_future();
            });
        });
    }).then([this] {
        _instances.clear();
        _instances = std::vector<sharded<Service>::entry>();
    });
}

template <typename Service>
future<>
sharded<Service>::invoke_on_all(smp_submit_to_options options, std::function<future<> (Service&)> func) {
    return internal::sharded_parallel_for_each(_instances.size(), [this, options, func = std::move(func)] (unsigned c) {
        return smp::submit_to(c, options, [this, func] {
            return func(*get_local_service());
        });
    });
}

template <typename Service>
template <typename... Args>
inline
future<>
sharded<Service>::invoke_on_all(smp_submit_to_options options, future<> (Service::*func)(Args...), Args... args) {
    return invoke_on_all(options, invoke_on_all_func_type([func, args...] (Service& service) mutable {
        return (service.*func)(args...);
    }));
}

template <typename Service>
template <typename... Args>
inline
future<>
sharded<Service>::invoke_on_all(smp_submit_to_options options, void (Service::*func)(Args...), Args... args) {
    return invoke_on_all(options, invoke_on_all_func_type([func, args...] (Service& service) mutable {
        (service.*func)(args...);
        return make_ready_future<>();
    }));
}

template <typename Service>
template <typename Func>
inline
future<>
sharded<Service>::invoke_on_all(smp_submit_to_options options, Func&& func) {
    static_assert(std::is_same<futurize_t<std::result_of_t<Func(Service&)>>, future<>>::value,
                  "invoke_on_all()'s func must return void or future<>");
    return invoke_on_all(options, invoke_on_all_func_type([func] (Service& service) mutable {
        return futurize_invoke(func, service);
    }));
}

template <typename Service>
template <typename Func>
inline
future<>
sharded<Service>::invoke_on_others(smp_submit_to_options options, Func&& func) {
    static_assert(std::is_same<futurize_t<std::result_of_t<Func(Service&)>>, future<>>::value,
                  "invoke_on_others()'s func must return void or future<>");
    return invoke_on_all(options, [orig = this_shard_id(), func = std::forward<Func>(func)] (auto& s) -> future<> {
        return this_shard_id() == orig ? make_ready_future<>() : futurize_invoke(func, s);
    });
}

template <typename Service>
const Service& sharded<Service>::local() const {
    assert(local_is_initialized());
    return *_instances[this_shard_id()].service;
}

template <typename Service>
Service& sharded<Service>::local() {
    assert(local_is_initialized());
    return *_instances[this_shard_id()].service;
}

template <typename Service>
shared_ptr<Service> sharded<Service>::local_shared() {
    assert(local_is_initialized());
    return _instances[this_shard_id()].service;
}

template <typename Service>
inline bool sharded<Service>::local_is_initialized() const {
    return _instances.size() > this_shard_id() &&
           _instances[this_shard_id()].service;
}

/// \addtogroup smp-module
/// @{

/// Smart pointer wrapper which makes it safe to move across CPUs.
///
/// \c foreign_ptr<> is a smart pointer wrapper which, unlike
/// \ref shared_ptr and \ref lw_shared_ptr, is safe to move to a
/// different core.
///
/// As seastar avoids locking, any but the most trivial objects must
/// be destroyed on the same core they were created on, so that,
/// for example, their destructors can unlink references to the
/// object from various containers.  In addition, for performance
/// reasons, the shared pointer types do not use atomic operations
/// to manage their reference counts.  As a result they cannot be
/// used on multiple cores in parallel.
///
/// \c foreign_ptr<> provides a solution to that problem.
/// \c foreign_ptr<> wraps any pointer type -- raw pointer,
/// \ref shared_ptr<>, or similar, and remembers on what core this
/// happened.  When the \c foreign_ptr<> object is destroyed, it
/// sends a message to the original core so that the wrapped object
/// can be safely destroyed.
///
/// \c foreign_ptr<> is a move-only object; it cannot be copied.
///
template <typename PtrType>
class foreign_ptr {
private:
    PtrType _value;
    unsigned _cpu;
private:
    void destroy(PtrType p, unsigned cpu) {
        if (p && this_shard_id() != cpu) {
            // `destroy()` is called from the destructor and other
            // synchronous methods (like `reset()`), that have no way to
            // wait for this future.
            (void)smp::submit_to(cpu, [v = std::move(p)] () mutable {
                // Destroy the contained pointer. We do this explicitly
                // in the current shard, because the lambda is destroyed
                // in the shard that submitted the task.
                v = {};
            });
        }
    }
public:
    using element_type = typename std::pointer_traits<PtrType>::element_type;
    using pointer = element_type*;

    /// Constructs a null \c foreign_ptr<>.
    foreign_ptr()
        : _value(PtrType())
        , _cpu(this_shard_id()) {
    }
    /// Constructs a null \c foreign_ptr<>.
    foreign_ptr(std::nullptr_t) : foreign_ptr() {}
    /// Wraps a pointer object and remembers the current core.
    foreign_ptr(PtrType value)
        : _value(std::move(value))
        , _cpu(this_shard_id()) {
    }
    // The type is intentionally non-copyable because copies
    // are expensive because each copy requires across-CPU call.
    foreign_ptr(const foreign_ptr&) = delete;
    /// Moves a \c foreign_ptr<> to another object.
    foreign_ptr(foreign_ptr&& other) = default;
    /// Destroys the wrapped object on its original cpu.
    ~foreign_ptr() {
        destroy(std::move(_value), _cpu);
    }
    /// Creates a copy of this foreign ptr. Only works if the stored ptr is copyable.
    future<foreign_ptr> copy() const {
        return smp::submit_to(_cpu, [this] () mutable {
            auto v = _value;
            return make_foreign(std::move(v));
        });
    }
    /// Accesses the wrapped object.
    element_type& operator*() const { return *_value; }
    /// Accesses the wrapped object.
    element_type* operator->() const { return &*_value; }
    /// Access the raw pointer to the wrapped object.
    pointer get() const { return &*_value; }
    /// Return the owner-shard of this pointer.
    ///
    /// The owner shard of the pointer can change as a result of
    /// move-assigment or a call to reset().
    unsigned get_owner_shard() const { return _cpu; }
    /// Checks whether the wrapped pointer is non-null.
    operator bool() const { return static_cast<bool>(_value); }
    /// Move-assigns a \c foreign_ptr<>.
    foreign_ptr& operator=(foreign_ptr&& other) noexcept(std::is_nothrow_move_constructible<PtrType>::value) {
         destroy(std::move(_value), _cpu);
        _value = std::move(other._value);
        _cpu = other._cpu;
        return *this;
    }
    /// Releases the owned pointer
    ///
    /// Warning: the caller is now responsible for destroying the
    /// pointer on its owner shard. This method is best called on the
    /// owner shard to avoid accidents.
    PtrType release() {
        return std::exchange(_value, {});
    }
    /// Replace the managed pointer with new_ptr.
    ///
    /// The previous managed pointer is destroyed on its owner shard.
    void reset(PtrType new_ptr) {
        auto old_ptr = std::move(_value);
        auto old_cpu = _cpu;

        _value = std::move(new_ptr);
        _cpu = this_shard_id();

        destroy(std::move(old_ptr), old_cpu);
    }
    /// Replace the managed pointer with a null value.
    ///
    /// The previous managed pointer is destroyed on its owner shard.
    void reset(std::nullptr_t = nullptr) {
        reset(PtrType());
    }
};

/// Wraps a raw or smart pointer object in a \ref foreign_ptr<>.
///
/// \relates foreign_ptr
template <typename T>
foreign_ptr<T> make_foreign(T ptr) {
    return foreign_ptr<T>(std::move(ptr));
}

/// @}

template<typename T>
struct is_smart_ptr<foreign_ptr<T>> : std::true_type {};

}

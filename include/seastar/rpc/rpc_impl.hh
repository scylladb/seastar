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

#include <seastar/core/format.hh>
#include <seastar/core/function_traits.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/assert.hh>
#include <seastar/util/is_smart_ptr.hh>
#include <seastar/core/simple-stream.hh>
#include <seastar/net/packet-data-source.hh>

#include <boost/type.hpp> // for compatibility

namespace seastar {

namespace rpc {

enum class exception_type : uint32_t {
    USER = 0,
    UNKNOWN_VERB = 1,
};

template<typename T>
struct remove_optional {
    using type = T;
};

template<typename T>
struct remove_optional<optional<T>> {
    using type = T;
};

struct wait_type {}; // opposite of no_wait_type

// tags to tell whether we want a const client_info& parameter
struct do_want_client_info {};
struct dont_want_client_info {};

// tags to tell whether we want a opt_time_point parameter
struct do_want_time_point {};
struct dont_want_time_point {};

// General case
template <typename Ret, typename... In>
struct signature<Ret (In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature;
    using want_client_info = dont_want_client_info;
    using want_time_point = dont_want_time_point;
};

// Specialize 'clean' for handlers that receive client_info
template <typename Ret, typename... In>
struct signature<Ret (const client_info&, In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature<Ret (In...)>;
    using want_client_info = do_want_client_info;
    using want_time_point = dont_want_time_point;
};

template <typename Ret, typename... In>
struct signature<Ret (client_info&, In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature<Ret (In...)>;
    using want_client_info = do_want_client_info;
    using want_time_point = dont_want_time_point;
};

// Specialize 'clean' for handlers that receive client_info and opt_time_point
template <typename Ret, typename... In>
struct signature<Ret (const client_info&, opt_time_point, In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature<Ret (In...)>;
    using want_client_info = do_want_client_info;
    using want_time_point = do_want_time_point;
};

template <typename Ret, typename... In>
struct signature<Ret (client_info&, opt_time_point, In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature<Ret (In...)>;
    using want_client_info = do_want_client_info;
    using want_time_point = do_want_time_point;
};

// Specialize 'clean' for handlers that receive opt_time_point
template <typename Ret, typename... In>
struct signature<Ret (opt_time_point, In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature<Ret (In...)>;
    using want_client_info = dont_want_client_info;
    using want_time_point = do_want_time_point;
};

template <typename T>
struct wait_signature {
    using type = wait_type;
    using cleaned_type = T;
};

template <typename... T>
struct wait_signature<future<T...>> {
    using type = wait_type;
    using cleaned_type = future<T...>;
};

template <>
struct wait_signature<no_wait_type> {
    using type = no_wait_type;
    using cleaned_type = void;
};

template <>
struct wait_signature<future<no_wait_type>> {
    using type = no_wait_type;
    using cleaned_type = future<>;
};

template <typename T>
using wait_signature_t = typename wait_signature<T>::type;

template <typename... In>
inline
std::tuple<In...>
maybe_add_client_info(dont_want_client_info, client_info&, std::tuple<In...>&& args) {
    return std::move(args);
}

template <typename... In>
inline
std::tuple<std::reference_wrapper<client_info>, In...>
maybe_add_client_info(do_want_client_info, client_info& ci, std::tuple<In...>&& args) {
    return std::tuple_cat(std::make_tuple(std::ref(ci)), std::move(args));
}

template <typename... In>
inline
std::tuple<In...>
maybe_add_time_point(dont_want_time_point, opt_time_point&, std::tuple<In...>&& args) {
    return std::move(args);
}

template <typename... In>
inline
std::tuple<opt_time_point, In...>
maybe_add_time_point(do_want_time_point, opt_time_point& otp, std::tuple<In...>&& args) {
    return std::tuple_cat(std::make_tuple(otp), std::move(args));
}

inline sstring serialize_connection_id(const connection_id& id) {
    sstring p = uninitialized_string(sizeof(id));
    auto c = p.data();
    write_le(c, id.id());
    return p;
}

inline connection_id deserialize_connection_id(const sstring& s) {
    using id_type = decltype(connection_id{0}.id());
    auto p = s.c_str();
    auto id = read_le<id_type>(p);
    return connection_id{id};
}

template <bool IsSmartPtr>
struct serialize_helper;

template <>
struct serialize_helper<false> {
    template <typename Serializer, typename Output, typename T>
    static inline void serialize(Serializer& serializer, Output& out, const T& t) {
        return write(serializer, out, t);
    }
};

template <>
struct serialize_helper<true> {
    template <typename Serializer, typename Output, typename T>
    static inline void serialize(Serializer& serializer, Output& out, const T& t) {
        return write(serializer, out, *t);
    }
};

template <typename Serializer, typename Output, typename... T>
inline void do_marshall(Serializer& serializer, Output& out, const T&... args);

template <typename Serializer, typename Output>
struct marshall_one {
    template <typename T> struct helper {
        static void doit(Serializer& serializer, Output& out, const T& arg) {
            using serialize_helper_type = serialize_helper<is_smart_ptr<typename std::remove_reference_t<T>>::value>;
            serialize_helper_type::serialize(serializer, out, arg);
        }
    };
    template<typename T> struct helper<std::reference_wrapper<const T>> {
        static void doit(Serializer& serializer, Output& out, const std::reference_wrapper<const T>& arg) {
            helper<T>::doit(serializer, out, arg.get());
        }
    };
    static void put_connection_id(const connection_id& cid, Output& out) {
        sstring id = serialize_connection_id(cid);
        out.write(id.c_str(), id.size());
    }
    template <typename... T> struct helper<sink<T...>> {
        static void doit(Serializer&, Output& out, const sink<T...>& arg) {
            put_connection_id(arg.get_id(), out);
        }
    };
    template <typename... T> struct helper<source<T...>> {
        static void doit(Serializer&, Output& out, const source<T...>& arg) {
            put_connection_id(arg.get_id(), out);
        }
    };
    template <typename... T> struct helper<tuple<T...>> {
        static void doit(Serializer& serializer, Output& out, const tuple<T...>& arg) {
            auto do_do_marshall = [&serializer, &out] (const auto&... args) {
                do_marshall(serializer, out, args...);
            };
            // since C++23, std::apply() only accepts tuple-like types, while
            // rpc::tuple is not a tuple-like type from the tuple-like C++
            // concept's perspective. so we have to cast it to std::tuple to
            // appease std::apply()
            std::apply(do_do_marshall, static_cast<const std::tuple<T...>&>(arg));
        }
    };
};

template <typename Serializer, typename Output, typename... T>
inline void do_marshall(Serializer& serializer, Output& out, const T&... args) {
    // C++ guarantees that brace-initialization expressions are evaluted in order
    (void)std::initializer_list<int>{(marshall_one<Serializer, Output>::template helper<T>::doit(serializer, out, args), 1)...};
}

static inline memory_output_stream<snd_buf::iterator> make_serializer_stream(snd_buf& output) {
    auto* b = std::get_if<temporary_buffer<char>>(&output.bufs);
    if (b) {
        return memory_output_stream<snd_buf::iterator>(memory_output_stream<snd_buf::iterator>::simple(b->get_write(), b->size()));
    } else {
        auto& ar = std::get<std::vector<temporary_buffer<char>>>(output.bufs);
        return memory_output_stream<snd_buf::iterator>(memory_output_stream<snd_buf::iterator>::fragmented(ar.begin(), output.size));
    }
}

template <typename Serializer, typename... T>
inline snd_buf marshall(Serializer& serializer, size_t head_space, const T&... args) {
    measuring_output_stream measure;
    do_marshall(serializer, measure, args...);
    snd_buf ret(measure.size() + head_space);
    auto out = make_serializer_stream(ret);
    out.skip(head_space);
    do_marshall(serializer, out, args...);
    return ret;
}

template <typename Serializer, typename Input, typename... T>
std::tuple<T...> do_unmarshall(connection& c, Input& in);

// The protocol to call the serializer is read(serializer, stream, rpc::type<T>).
// However, some users (ahem) used boost::type instead of rpc::type when the two
// types were aliased, preventing us from moving to the newer std::type_identity.
// To preserve compatibility, calls to read() are routed through
// read_via_type_marker(), of which there are two variants, one for
// boost::type (marked as deprecated) and one for std::type_identity.

template <typename T, typename... Args>
requires requires (Args... args, type<T> t) { read(std::forward<Args>(args)..., t); }
auto
read_via_type_marker(Args&&... args) {
    return read(std::forward<Args>(args)..., type<T>());
}

template <typename T, typename... Args>
requires requires (Args... args, boost::type<T> t) { read(std::forward<Args>(args)..., t); }
[[deprecated("Use rpc::type<> instead of boost::type<>")]]
auto
read_via_type_marker(Args&&... args) {
    return read(std::forward<Args>(args)..., boost::type<T>());
}

template<typename Serializer, typename Input>
struct unmarshal_one {
    template<typename T> struct helper {
        static T doit(connection& c, Input& in) {
            return read_via_type_marker<T>(c.serializer<Serializer>(), in);
        }
    };
    template<typename T> struct helper<optional<T>> {
        static optional<T> doit(connection& c, Input& in) {
            if (in.size()) {
                return optional<T>(read_via_type_marker<typename remove_optional<T>::type>(c.serializer<Serializer>(), in));
            } else {
                return optional<T>();
            }
        }
    };
    template<typename T> struct helper<std::reference_wrapper<const T>> {
        static T doit(connection& c, Input& in) {
            return helper<T>::doit(c, in);
        }
    };
    static connection_id get_connection_id(Input& in) {
        sstring id = uninitialized_string(sizeof(connection_id));
        in.read(id.data(), sizeof(connection_id));
        return deserialize_connection_id(id);
    }
    template<typename... T> struct helper<sink<T...>> {
        static sink<T...> doit(connection& c, Input& in) {
            return sink<T...>(make_shared<sink_impl<Serializer, T...>>(c.get_stream(get_connection_id(in))));
        }
    };
    template<typename... T> struct helper<source<T...>> {
        static source<T...> doit(connection& c, Input& in) {
            return source<T...>(make_shared<source_impl<Serializer, T...>>(c.get_stream(get_connection_id(in))));
        }
    };
    template <typename... T> struct helper<tuple<T...>> {
        static tuple<T...> doit(connection& c, Input& in) {
            return do_unmarshall<Serializer, Input, T...>(c, in);
        }
    };
};

template <typename... T>
struct default_constructible_tuple_except_first;

template <>
struct default_constructible_tuple_except_first<> {
    using type = std::tuple<>;
};

template <typename T0, typename... T>
struct default_constructible_tuple_except_first<T0, T...> {
    using type = std::tuple<
            T0,
            std::conditional_t<
                    std::is_default_constructible_v<T>,
                    T,
                    std::optional<T>
            >...
        >;
};

template <typename... T>
using default_constructible_tuple_except_first_t = typename default_constructible_tuple_except_first<T...>::type;

// Where Tin != Tout, apply std:optional::value()
template <typename... Tout, typename... Tin>
auto
unwrap_optional_if_needed(std::tuple<Tin...>&& tuple_in) {
    using tuple_in_t = std::tuple<Tin...>;
    using tuple_out_t = std::tuple<Tout...>;
    return std::invoke([&] <size_t... Idx> (std::index_sequence<Idx...>) {
        return tuple_out_t(
            std::invoke([&] () {
                if constexpr (std::same_as<std::tuple_element_t<Idx, tuple_in_t>, std::tuple_element_t<Idx, tuple_out_t>>) {
                    return std::move(std::get<Idx>(tuple_in));
                } else {
                    return std::move(std::get<Idx>(tuple_in).value());
                }
            })...);
    }, std::make_index_sequence<sizeof...(Tout)>());
}

template <typename Serializer, typename Input, typename... T>
inline std::tuple<T...> do_unmarshall(connection& c, Input& in) {
    // Argument order processing is unspecified, but we need to deserialize
    // left-to-right. So we deserialize into something that can be lazily
    // constructed (and can conditionally destroy itself if we only constructed some
    // of the arguments).
    //
    // The first element of the tuple has no ordering
    // problem, and we can deserialize directly into a std::tuple<T...>.
    //
    // For the rest of the elements, if they are default-constructible, we leave
    // them as is, and if not, we deserialize into std::optional<T>, and later
    // unwrap them. If we're lucky and nothing was wrapped, we can return without
    // any data movement.
    using ret_type = std::tuple<T...>;
    using temporary_type = default_constructible_tuple_except_first_t<T...>;
    return std::invoke([&] <size_t... Idx> (std::index_sequence<Idx...>) {
        auto tmp = temporary_type(
            std::invoke([&] () -> std::tuple_element_t<Idx, temporary_type> {
                if constexpr (Idx == 0) {
                    // The first T has no ordering problem, so we can deserialize it directly into the tuple
                    return unmarshal_one<Serializer, Input>::template helper<std::tuple_element_t<Idx, ret_type>>::doit(c, in);
                } else {
                    // Use default constructor for the rest of the Ts
                    return {};
                }
            })...
        );
        // Deserialize the other Ts, comma-expression preserves left-to-right order.
        (void)(...,  ((Idx == 0
            ? 0
            : ((std::get<Idx>(tmp) = unmarshal_one<Serializer, Input>::template helper<std::tuple_element_t<Idx, ret_type>>::doit(c, in), 0)))));
        if constexpr (std::same_as<ret_type, temporary_type>) {
            // Use Named Return Vale Optimization (NVRO) if we didn't have to wrap anything
            return tmp;
        } else {
            return unwrap_optional_if_needed<T...>(std::move(tmp));
        }
    }, std::index_sequence_for<T...>());
}

template <typename Serializer, typename... T>
inline std::tuple<T...> unmarshall(connection& c, rcv_buf input) {
    auto in = make_deserializer_stream(input);
    return do_unmarshall<Serializer, decltype(in), T...>(c, in);
}

inline std::exception_ptr unmarshal_exception(rcv_buf& d) {
    std::exception_ptr ex;
    auto data = make_deserializer_stream(d);

    uint32_t v32;
    data.read(reinterpret_cast<char*>(&v32), 4);
    exception_type ex_type = exception_type(le_to_cpu(v32));
    data.read(reinterpret_cast<char*>(&v32), 4);
    uint32_t ex_len = le_to_cpu(v32);

    switch (ex_type) {
    case exception_type::USER: {
        std::string s(ex_len, '\0');
        data.read(&*s.begin(), ex_len);
        ex = std::make_exception_ptr(remote_verb_error(std::move(s)));
        break;
    }
    case exception_type::UNKNOWN_VERB: {
        uint64_t v64;
        data.read(reinterpret_cast<char*>(&v64), 8);
        ex = std::make_exception_ptr(unknown_verb_error(le_to_cpu(v64)));
        break;
    }
    default:
        ex = std::make_exception_ptr(unknown_exception_error());
        break;
    }
    return ex;
}

template <typename Payload, typename... T>
struct rcv_reply_base  {
    bool done = false;
    promise<T...> p;
    template<typename... V>
    void set_value(V&&... v) {
        done = true;
        p.set_value(internal::untuple(std::forward<V>(v))...);
    }
    ~rcv_reply_base() {
        if (!done) {
            p.set_exception(closed_error());
        }
    }
};

template<typename Serializer, typename T>
struct rcv_reply : rcv_reply_base<T, T> {
    inline void get_reply(rpc::client& dst, rcv_buf input) {
        this->set_value(unmarshall<Serializer, T>(dst, std::move(input)));
    }
};

template<typename Serializer, typename... T>
struct rcv_reply<Serializer, future<T...>> : rcv_reply_base<std::tuple<T...>, T...> {
    inline void get_reply(rpc::client& dst, rcv_buf input) {
        this->set_value(unmarshall<Serializer, T...>(dst, std::move(input)));
    }
};

template<typename Serializer>
struct rcv_reply<Serializer, void> : rcv_reply_base<void, void> {
    inline void get_reply(rpc::client&, rcv_buf) {
        this->set_value();
    }
};

template<typename Serializer>
struct rcv_reply<Serializer, future<>> : rcv_reply<Serializer, void> {};

template <typename Serializer, typename Ret, typename... InArgs>
inline auto wait_for_reply(wait_type, std::optional<rpc_clock_type::time_point> timeout, rpc_clock_type::time_point start, cancellable* cancel, rpc::client& dst, id_type msg_id,
        signature<Ret (InArgs...)>) {
    using reply_type = rcv_reply<Serializer, Ret>;
    auto lambda = [] (reply_type& r, rpc::client& dst, id_type msg_id, rcv_buf data) mutable {
        if (msg_id >= 0) {
            dst.get_stats_internal().replied++;
            return r.get_reply(dst, std::move(data));
        } else {
            dst.get_stats_internal().exception_received++;
            r.done = true;
            r.p.set_exception(unmarshal_exception(data));
        }
    };
    using handler_type = typename rpc::client::template reply_handler<reply_type, decltype(lambda)>;
    auto r = std::make_unique<handler_type>(std::move(lambda));
    r->start = start;
    auto fut = r->reply.p.get_future();
    dst.wait_for_reply(msg_id, std::move(r), timeout, cancel);
    return fut;
}

template<typename Serializer, typename... InArgs>
inline auto wait_for_reply(no_wait_type, std::optional<rpc_clock_type::time_point>, rpc_clock_type::time_point start, cancellable*, rpc::client&, id_type,
        signature<no_wait_type (InArgs...)>) {  // no_wait overload
    return make_ready_future<>();
}

template<typename Serializer, typename... InArgs>
inline auto wait_for_reply(no_wait_type, std::optional<rpc_clock_type::time_point>, rpc_clock_type::time_point, cancellable*, rpc::client&, id_type,
        signature<future<no_wait_type> (InArgs...)>) {  // future<no_wait> overload
    return make_ready_future<>();
}

// Convert a relative timeout (a duration) to an absolute one (time_point).
// Do the calculation safely so that a very large duration will be capped by
// time_point::max, instead of wrapping around to ancient history.
inline rpc_clock_type::time_point
relative_timeout_to_absolute(rpc_clock_type::duration relative) {
    rpc_clock_type::time_point now = rpc_clock_type::now();
    return now + std::min(relative, rpc_clock_type::time_point::max() - now);
}

// Refer to struct request_frame for more details
static constexpr size_t request_frame_headroom = 28;

// Returns lambda that can be used to send rpc messages.
// The lambda gets client connection and rpc parameters as arguments, marshalls them sends
// to a server and waits for a reply. After receiving reply it unmarshalls it and signal completion
// to a caller.
template<typename Serializer, typename MsgType, typename Ret, typename... InArgs>
auto send_helper(MsgType xt, signature<Ret (InArgs...)> xsig) {
    struct shelper {
        MsgType t;
        signature<Ret (InArgs...)> sig;
        auto send(rpc::client& dst, std::optional<rpc_clock_type::time_point> timeout, cancellable* cancel, const InArgs&... args) {
            if (dst.error()) {
                using cleaned_ret_type = typename wait_signature<Ret>::cleaned_type;
                return futurize<cleaned_ret_type>::make_exception_future(closed_error());
            }

            auto start = rpc_clock_type::now();
            // send message
            auto msg_id = dst.next_message_id();
            snd_buf data = marshall(dst.template serializer<Serializer>(), request_frame_headroom, args...);

            // prepare reply handler, if return type is now_wait_type this does nothing, since no reply will be sent
            using wait = wait_signature_t<Ret>;
            return when_all(dst.request(uint64_t(t), msg_id, std::move(data), timeout, cancel), wait_for_reply<Serializer>(wait(), timeout, start, cancel, dst, msg_id, sig)).then([] (auto r) {
                    std::get<0>(r).ignore_ready_future();
                    return std::move(std::get<1>(r)); // return future of wait_for_reply
            });
        }
        auto operator()(rpc::client& dst, const InArgs&... args) {
            return send(dst, {}, nullptr, args...);
        }
        auto operator()(rpc::client& dst, rpc_clock_type::time_point timeout, const InArgs&... args) {
            return send(dst, timeout, nullptr, args...);
        }
        auto operator()(rpc::client& dst, rpc_clock_type::duration timeout, const InArgs&... args) {
            return send(dst, relative_timeout_to_absolute(timeout), nullptr, args...);
        }
        auto operator()(rpc::client& dst, cancellable& cancel, const InArgs&... args) {
            return send(dst, {}, &cancel, args...);
        }

    };
    return shelper{xt, xsig};
}

// Refer to struct response_frame for more details
static constexpr size_t response_frame_headroom = 16;

template<typename Serializer, typename RetTypes>
inline future<> reply(wait_type, future<RetTypes>&& ret, int64_t msg_id, shared_ptr<server::connection> client,
        std::optional<rpc_clock_type::time_point> timeout, std::optional<rpc_clock_type::duration> handler_duration) {
    if (!client->error()) {
        snd_buf data;
        try {
            if constexpr (std::is_void_v<RetTypes>) {
                ret.get();
                data = std::invoke(marshall<Serializer>, std::ref(client->template serializer<Serializer>()), response_frame_headroom);
            } else {
                data = std::invoke(marshall<Serializer, const RetTypes&>, std::ref(client->template serializer<Serializer>()), response_frame_headroom, std::move(ret.get()));
            }
        } catch (std::exception& ex) {
            uint32_t len = std::strlen(ex.what());
            data = snd_buf(response_frame_headroom + 2 * sizeof(uint32_t) + len);
            auto os = make_serializer_stream(data);
            os.skip(response_frame_headroom);
            uint32_t v32 = cpu_to_le(uint32_t(exception_type::USER));
            os.write(reinterpret_cast<char*>(&v32), sizeof(v32));
            v32 = cpu_to_le(len);
            os.write(reinterpret_cast<char*>(&v32), sizeof(v32));
            os.write(ex.what(), len);
            msg_id = -msg_id;
        }

        return client->respond(msg_id, std::move(data), timeout, handler_duration);
    } else {
        ret.ignore_ready_future();
        return make_ready_future<>();
    }
}

// specialization for no_wait_type which does not send a reply
template<typename Serializer>
inline future<> reply(no_wait_type, future<no_wait_type>&& r, int64_t msgid, shared_ptr<server::connection> client,
        std::optional<rpc_clock_type::time_point>, std::optional<rpc_clock_type::duration>) {
    try {
        r.get();
    } catch (std::exception& ex) {
        client->get_logger()(client->info(), msgid, to_sstring("exception \"") + ex.what() + "\" in no_wait handler ignored");
    }
    return make_ready_future<>();
}

template<typename Ret, typename... InArgs, typename WantClientInfo, typename WantTimePoint, typename Func, typename ArgsTuple>
inline futurize_t<Ret> apply(Func& func, client_info& info, opt_time_point time_point, WantClientInfo wci, WantTimePoint wtp, signature<Ret (InArgs...)>, ArgsTuple&& args) {
    using futurator = futurize<Ret>;
    return futurator::apply(func, maybe_add_client_info(wci, info, maybe_add_time_point(wtp, time_point, std::forward<ArgsTuple>(args))));
}

// lref_to_cref is a helper that encapsulates lvalue reference in std::ref() or does nothing otherwise
template<typename T>
auto lref_to_cref(T&& x) {
    return std::move(x);
}

template<typename T>
auto lref_to_cref(T& x) {
    return std::ref(x);
}

// Creates lambda to handle RPC message on a server.
// The lambda unmarshalls all parameters, calls a handler, marshall return values and sends them back to a client
template <typename Serializer, typename Func, typename Ret, typename... InArgs, typename WantClientInfo, typename WantTimePoint>
auto recv_helper(signature<Ret (InArgs...)> sig, Func&& func, WantClientInfo, WantTimePoint) {
    using signature = decltype(sig);
    using wait_style = wait_signature_t<Ret>;
    return [func = lref_to_cref(std::forward<Func>(func))](shared_ptr<server::connection> client,
                                                           std::optional<rpc_clock_type::time_point> timeout,
                                                           int64_t msg_id,
                                                           rcv_buf data,
                                                           gate::holder guard) mutable {
        auto memory_consumed = client->estimate_request_size(data.size);
        if (memory_consumed > client->max_request_size()) {
            auto err = format("request size {:d} large than memory limit {:d}", memory_consumed, client->max_request_size());
            client->get_logger()(client->peer_address(), err);
            // FIXME: future is discarded
            (void)try_with_gate(client->get_server().reply_gate(), [client, timeout, msg_id, err = std::move(err)] {
                return reply<Serializer>(wait_style(), futurize<Ret>::make_exception_future(std::runtime_error(err.c_str())), msg_id, client, timeout, std::nullopt).handle_exception([client, msg_id] (std::exception_ptr eptr) {
                    client->get_logger()(client->info(), msg_id, format("got exception while processing an oversized message: {}", eptr));
                });
            }).handle_exception_type([] (gate_closed_exception&) {/* ignore */});
            return make_ready_future();
        }
        // note: apply is executed asynchronously with regards to networking so we cannot chain futures here by doing "return apply()"
        auto f = client->wait_for_resources(memory_consumed, timeout).then([client, timeout, msg_id, data = std::move(data), &func, g = std::move(guard)] (auto permit) mutable {
                // FIXME: future is discarded
                (void)try_with_gate(client->get_server().reply_gate(), [client, timeout, msg_id, data = std::move(data), permit = std::move(permit), &func] () mutable {
                    try {
                        auto args = unmarshall<Serializer, InArgs...>(*client, std::move(data));
                        auto start = rpc_clock_type::now();
                        return apply(func, client->info(), timeout, WantClientInfo(), WantTimePoint(), signature(), std::move(args)).then_wrapped([client, timeout, msg_id, permit = std::move(permit), start] (futurize_t<Ret> ret) mutable {
                            return reply<Serializer>(wait_style(), std::move(ret), msg_id, client, timeout, rpc_clock_type::now() - start).handle_exception([permit = std::move(permit), client, msg_id] (std::exception_ptr eptr) {
                                client->get_logger()(client->info(), msg_id, format("got exception while processing a message: {}", eptr));
                            });
                        });
                    } catch (...) {
                        client->get_logger()(client->info(), msg_id, format("caught exception while processing a message: {}", std::current_exception()));
                        return make_ready_future();
                    }
                }).handle_exception_type([g = std::move(g)] (gate_closed_exception&) {/* ignore */});
        });

        if (timeout) {
            f = f.handle_exception_type([] (semaphore_timed_out&) { /* ignore */ });
        }

        return f;
    };
}

// helper to create copy constructible lambda from non copy constructible one. std::function<> works only with former kind.
template<typename Func>
auto make_copyable_function(Func&& func, std::enable_if_t<!std::is_copy_constructible_v<std::decay_t<Func>>, void*> = nullptr) {
  auto p = make_lw_shared<typename std::decay_t<Func>>(std::forward<Func>(func));
  return [p] (auto&&... args) { return (*p)( std::forward<decltype(args)>(args)... ); };
}

template<typename Func>
auto make_copyable_function(Func&& func, std::enable_if_t<std::is_copy_constructible_v<std::decay_t<Func>>, void*> = nullptr) {
    return std::forward<Func>(func);
}

// This class is used to calculate client side rpc function signature.
// Return type is converted from a smart pointer to a type it points to.
// rpc::optional are converted to non optional type.
//
// Examples:
// std::unique_ptr<int>(int, rpc::optional<long>) -> int(int, long)
// double(float) -> double(float)
template<typename Ret, typename... In>
class client_function_type {
    template<typename T, bool IsSmartPtr>
    struct drop_smart_ptr_impl;
    template<typename T>
    struct drop_smart_ptr_impl<T, true> {
        using type = typename T::element_type;
    };
    template<typename T>
    struct drop_smart_ptr_impl<T, false> {
        using type = T;
    };
    template<typename T>
    using drop_smart_ptr = drop_smart_ptr_impl<T, is_smart_ptr<T>::value>;

    // if return type is smart ptr take a type it points to instead
    using return_type = typename drop_smart_ptr<Ret>::type;
public:
    using type = return_type(typename remove_optional<In>::type...);
};

template<typename Serializer, typename MsgType>
template<typename Ret, typename... In>
auto protocol<Serializer, MsgType>::make_client(signature<Ret(In...)>, MsgType t) {
    using sig_type = signature<typename client_function_type<Ret, In...>::type>;
    return send_helper<Serializer>(t, sig_type());
}

template<typename Serializer, typename MsgType>
template<typename Func>
auto protocol<Serializer, MsgType>::make_client(MsgType t) {
    return make_client(typename signature<typename function_traits<Func>::signature>::clean(), t);
}

template<typename Serializer, typename MsgType>
template<typename Func>
auto protocol<Serializer, MsgType>::register_handler(MsgType t, scheduling_group sg, Func&& func) {
    using sig_type = signature<typename function_traits<Func>::signature>;
    using clean_sig_type = typename sig_type::clean;
    using want_client_info = typename sig_type::want_client_info;
    using want_time_point = typename sig_type::want_time_point;
    auto recv = recv_helper<Serializer>(clean_sig_type(), std::forward<Func>(func),
            want_client_info(), want_time_point());
    register_receiver(t, rpc_handler{sg, make_copyable_function(std::move(recv)), {}});
    return make_client(clean_sig_type(), t);
}

template<typename Serializer, typename MsgType>
template<typename Func>
auto protocol<Serializer, MsgType>::register_handler(MsgType t, Func&& func) {
    return register_handler(t, scheduling_group(), std::forward<Func>(func));
}

template<typename Serializer, typename MsgType>
future<> protocol<Serializer, MsgType>::unregister_handler(MsgType t) {
    auto it = _handlers.find(t);
    if (it != _handlers.end()) {
        return it->second.use_gate.close().finally([this, t] {
            _handlers.erase(t);
        });
    }
    return make_ready_future<>();
}

template<typename Serializer, typename MsgType>
bool protocol<Serializer, MsgType>::has_handler(MsgType msg_id) {
    auto it = _handlers.find(msg_id);
    if (it == _handlers.end()) {
        return false;
    }
    return !it->second.use_gate.is_closed();
}

template<typename Serializer, typename MsgType>
std::optional<protocol_base::handler_with_holder> protocol<Serializer, MsgType>::get_handler(uint64_t msg_id) {
    const auto it = _handlers.find(MsgType(msg_id));
    if (it != _handlers.end()) {
        try {
            return handler_with_holder{it->second, it->second.use_gate.hold()};
        } catch (gate_closed_exception&) {
            // unregistered, just ignore
        }
    }
    return std::nullopt;
}

template<typename T> T make_shard_local_buffer_copy(foreign_ptr<std::unique_ptr<T>> org);

template<typename Serializer, typename... Out>
future<> sink_impl<Serializer, Out...>::operator()(const Out&... args) {
    // note that we use remote serializer pointer, so if serailizer needs a state
    // it should have per-cpu one
    snd_buf data = marshall(this->_con->get()->template serializer<Serializer>(), 4, args...);
    static_assert(snd_buf::chunk_size >= 4, "send buffer chunk size is too small");
    auto p = data.front().get_write();
    write_le<uint32_t>(p, data.size - 4);
    // we do not want to dead lock on huge packets, so let them in
    // but only one at a time
    auto size = std::min(size_t(data.size), max_stream_buffers_memory);
    const auto seq_num = _next_seq_num++;
    return get_units(this->_sem, size).then([this, data = make_foreign(std::make_unique<snd_buf>(std::move(data))), seq_num] (semaphore_units<> su) mutable {
        if (this->_ex) {
            return make_exception_future(this->_ex);
        }
        // It is OK to discard this future. The user is required to
        // wait for it when closing.
        (void)smp::submit_to(this->_con->get_owner_shard(), [this, data = std::move(data), seq_num] () mutable {
            connection* con = this->_con->get();
            if (con->error()) {
                return make_exception_future(closed_error());
            }
            if(con->sink_closed()) {
                return make_exception_future(stream_closed());
            }

            auto& last_seq_num = _remote_state.last_seq_num;
            auto& out_of_order_bufs = _remote_state.out_of_order_bufs;

            auto local_data = make_shard_local_buffer_copy(std::move(data));
            const auto seq_num_diff = seq_num - last_seq_num;
            if (seq_num_diff > 1) {
                auto [it, _] = out_of_order_bufs.emplace(seq_num, deferred_snd_buf{promise<>{}, std::move(local_data)});
                return it->second.pr.get_future();
            }

            last_seq_num = seq_num;
            auto ret_fut = con->send(std::move(local_data), {}, nullptr);
            while (!out_of_order_bufs.empty() && out_of_order_bufs.begin()->first == (last_seq_num + 1)) {
                auto it = out_of_order_bufs.begin();
                last_seq_num = it->first;
                auto fut = con->send(std::move(it->second.data), {}, nullptr);
                fut.forward_to(std::move(it->second.pr));
                out_of_order_bufs.erase(it);
            }
            return ret_fut;
        }).then_wrapped([su = std::move(su), this] (future<> f) {
            if (f.failed() && !this->_ex) { // first error is the interesting one
                this->_ex = f.get_exception();
            } else {
                f.ignore_ready_future();
            }
        });
        return make_ready_future<>();
    });
}

template<typename Serializer, typename... Out>
future<> sink_impl<Serializer, Out...>::flush() {
    // wait until everything is sent out before returning.
    return with_semaphore(this->_sem, max_stream_buffers_memory, [this] {
        if (this->_ex) {
            return make_exception_future(this->_ex);
        }
        return make_ready_future();
    });
}

template<typename Serializer, typename... Out>
future<> sink_impl<Serializer, Out...>::close() {
    return with_semaphore(this->_sem, max_stream_buffers_memory, [this] {
        return smp::submit_to(this->_con->get_owner_shard(), [this] {
            connection* con = this->_con->get();
            if (con->sink_closed()) { // double close, should not happen!
                return make_exception_future(stream_closed());
            }
            future<> f = make_ready_future<>();
            if (!con->error() && !this->_ex) {
                snd_buf data = marshall(con->template serializer<Serializer>(), 4);
                static_assert(snd_buf::chunk_size >= 4, "send buffer chunk size is too small");
                auto p = data.front().get_write();
                write_le<uint32_t>(p, -1U); // max len fragment marks an end of a stream
                f = con->send(std::move(data), {}, nullptr);
            } else {
                f = this->_ex ? make_exception_future(this->_ex) : make_exception_future(closed_error());
            }
            return f.finally([con] { return con->close_sink(); });
        });
    });
}

template<typename Serializer, typename... Out>
sink_impl<Serializer, Out...>::~sink_impl() {
    // A failure to close might leave some continuations running after
    // this is destroyed, leading to use-after-free bugs.
    SEASTAR_ASSERT(this->_con->get()->sink_closed());
}

template<typename Serializer, typename... In>
future<std::optional<std::tuple<In...>>> source_impl<Serializer, In...>::operator()() {
    auto process_one_buffer = [this] {
        foreign_ptr<std::unique_ptr<rcv_buf>> buf = std::move(this->_bufs.front());
        this->_bufs.pop_front();
        return std::apply([] (In&&... args) {
            auto ret = std::make_optional(std::make_tuple(std::move(args)...));
            return make_ready_future<std::optional<std::tuple<In...>>>(std::move(ret));
        }, unmarshall<Serializer, In...>(*this->_con->get(), make_shard_local_buffer_copy(std::move(buf))));
    };

    if (!this->_bufs.empty()) {
        return process_one_buffer();
    }

    // refill buffers from remote cpu
    return smp::submit_to(this->_con->get_owner_shard(), [this] () -> future<> {
        connection* con = this->_con->get();
        if (con->_source_closed) {
            return make_exception_future<>(stream_closed());
        }
        return con->stream_receive(this->_bufs).then_wrapped([this, con] (future<>&& f) {
            if (f.failed()) {
                return con->close_source().then_wrapped([ex = f.get_exception()] (future<> f){
                    f.ignore_ready_future();
                    return make_exception_future<>(ex);
                });
            }
            if (this->_bufs.empty()) { // nothing to read -> eof
                return con->close_source().then_wrapped([] (future<> f) {
                    f.ignore_ready_future();
                    return make_ready_future<>();
                });
            }
            return make_ready_future<>();
        });
    }).then([this, process_one_buffer] () {
        if (this->_bufs.empty()) {
            return make_ready_future<std::optional<std::tuple<In...>>>(std::nullopt);
        } else {
            return process_one_buffer();
        }
    });
}

template<typename... Out>
connection_id sink<Out...>::get_id() const {
    return _impl->_con->get()->get_connection_id();
}

template<typename... In>
connection_id source<In...>::get_id() const {
    return _impl->_con->get()->get_connection_id();
}

template<typename... In>
template<typename Serializer, typename... Out>
sink<Out...> source<In...>::make_sink() {
    return sink<Out...>(make_shared<sink_impl<Serializer, Out...>>(_impl->_con));
}

}

}

namespace std {
template<>
struct hash<seastar::rpc::streaming_domain_type> {
    size_t operator()(const seastar::rpc::streaming_domain_type& domain) const {
        size_t h = 0;
        boost::hash_combine(h, std::hash<uint64_t>{}(domain._id));
        return h;
    }
};
}



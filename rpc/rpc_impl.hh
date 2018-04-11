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

#include <iostream>
#include "core/function_traits.hh"
#include "core/apply.hh"
#include "core/shared_ptr.hh"
#include "core/sstring.hh"
#include "core/future-util.hh"
#include "util/is_smart_ptr.hh"
#include "core/simple-stream.hh"
#include <boost/range/numeric.hpp>
#include <boost/range/adaptor/transformed.hpp>
#include "net/packet-data-source.hh"

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
maybe_add_client_info(dont_want_client_info, client_info& ci, std::tuple<In...>&& args) {
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
maybe_add_time_point(dont_want_time_point, opt_time_point& otp, std::tuple<In...>&& args) {
    return std::move(args);
}

template <typename... In>
inline
std::tuple<opt_time_point, In...>
maybe_add_time_point(do_want_time_point, opt_time_point& otp, std::tuple<In...>&& args) {
    return std::tuple_cat(std::make_tuple(otp), std::move(args));
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

template <typename Serializer, typename Output, typename T>
inline void marshall_one(Serializer& serializer, Output& out, const T& arg) {
    using serialize_helper_type = serialize_helper<is_smart_ptr<typename std::remove_reference<T>::type>::value>;
    serialize_helper_type::serialize(serializer, out, arg);
}

template <typename Serializer, typename Output, typename... T>
inline void do_marshall(Serializer& serializer, Output& out, const T&... args) {
    // C++ guarantees that brace-initialization expressions are evaluted in order
    (void)std::initializer_list<int>{(marshall_one(serializer, out, args), 1)...};
}

static inline memory_output_stream<snd_buf::iterator> make_serializer_stream(snd_buf& output) {
    auto* b = boost::get<temporary_buffer<char>>(&output.bufs);
    if (b) {
        return memory_output_stream<snd_buf::iterator>(memory_output_stream<snd_buf::iterator>::simple(b->get_write(), b->size()));
    } else {
        auto& ar = boost::get<std::vector<temporary_buffer<char>>>(output.bufs);
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

template <typename Serializer, typename Input>
inline std::tuple<> do_unmarshall(Serializer& serializer, Input& in) {
    return std::make_tuple();
}

template<typename Serializer, typename Input, typename T>
struct unmarshal_one {
    static T doit(Serializer& serializer, Input& in) {
        return read(serializer, in, type<T>());
    }
};

template<typename Serializer, typename Input, typename T>
struct unmarshal_one<Serializer, Input, optional<T>> {
    static optional<T> doit(Serializer& serializer, Input& in) {
        if (in.size()) {
            return optional<T>(read(serializer, in, type<typename remove_optional<T>::type>()));
        } else {
            return optional<T>();
        }
    }
};

template <typename Serializer, typename Input, typename T0, typename... Trest>
inline std::tuple<T0, Trest...> do_unmarshall(Serializer& serializer, Input& in) {
    // FIXME: something less recursive
    auto first = std::make_tuple(unmarshal_one<Serializer, Input, T0>::doit(serializer, in));
    auto rest = do_unmarshall<Serializer, Input, Trest...>(serializer, in);
    return std::tuple_cat(std::move(first), std::move(rest));
}

template <typename Serializer, typename... T>
inline std::tuple<T...> unmarshall(Serializer& serializer, rcv_buf input) {
    auto in = make_deserializer_stream(input);
    return do_unmarshall<Serializer, decltype(in), T...>(serializer, in);
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
        ex = std::make_exception_ptr(std::runtime_error(std::move(s)));
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
        p.set_value(std::forward<V>(v)...);
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
        this->set_value(unmarshall<Serializer, T>(dst.template serializer<Serializer>(), std::move(input)));
    }
};

template<typename Serializer, typename... T>
struct rcv_reply<Serializer, future<T...>> : rcv_reply_base<std::tuple<T...>, T...> {
    inline void get_reply(rpc::client& dst, rcv_buf input) {
        this->set_value(unmarshall<Serializer, T...>(dst.template serializer<Serializer>(), std::move(input)));
    }
};

template<typename Serializer>
struct rcv_reply<Serializer, void> : rcv_reply_base<void, void> {
    inline void get_reply(rpc::client& dst, rcv_buf input) {
        this->set_value();
    }
};

template<typename Serializer>
struct rcv_reply<Serializer, future<>> : rcv_reply<Serializer, void> {};

template <typename Serializer, typename Ret, typename... InArgs>
inline auto wait_for_reply(wait_type, std::experimental::optional<rpc_clock_type::time_point> timeout, cancellable* cancel, rpc::client& dst, id_type msg_id,
        signature<Ret (InArgs...)> sig) {
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
    auto fut = r->reply.p.get_future();
    dst.wait_for_reply(msg_id, std::move(r), timeout, cancel);
    return fut;
}

template<typename Serializer, typename... InArgs>
inline auto wait_for_reply(no_wait_type, std::experimental::optional<rpc_clock_type::time_point>, cancellable* cancel, rpc::client& dst, id_type msg_id,
        signature<no_wait_type (InArgs...)> sig) {  // no_wait overload
    return make_ready_future<>();
}

template<typename Serializer, typename... InArgs>
inline auto wait_for_reply(no_wait_type, std::experimental::optional<rpc_clock_type::time_point>, cancellable* cancel, rpc::client& dst, id_type msg_id,
        signature<future<no_wait_type> (InArgs...)> sig) {  // future<no_wait> overload
    return make_ready_future<>();
}

// Returns lambda that can be used to send rpc messages.
// The lambda gets client connection and rpc parameters as arguments, marshalls them sends
// to a server and waits for a reply. After receiving reply it unmarshalls it and signal completion
// to a caller.
template<typename Serializer, typename MsgType, typename Ret, typename... InArgs>
auto send_helper(MsgType xt, signature<Ret (InArgs...)> xsig) {
    struct shelper {
        MsgType t;
        signature<Ret (InArgs...)> sig;
        auto send(rpc::client& dst, std::experimental::optional<rpc_clock_type::time_point> timeout, cancellable* cancel, const InArgs&... args) {
            if (dst.error()) {
                using cleaned_ret_type = typename wait_signature<Ret>::cleaned_type;
                return futurize<cleaned_ret_type>::make_exception_future(closed_error());
            }

            // send message
            auto msg_id = dst.next_message_id();
            snd_buf data = marshall(dst.template serializer<Serializer>(), 28, args...);
            static_assert(snd_buf::chunk_size >= 28, "send buffer chunk size is too small");
            auto p = data.front().get_write() + 8; // 8 extra bytes for expiration timer
            write_le<uint64_t>(p, uint64_t(t));
            write_le<int64_t>(p + 8, msg_id);
            write_le<uint32_t>(p + 16, data.size - 28);

            // prepare reply handler, if return type is now_wait_type this does nothing, since no reply will be sent
            using wait = wait_signature_t<Ret>;
            return when_all(dst.send(std::move(data), timeout, cancel), wait_for_reply<Serializer>(wait(), timeout, cancel, dst, msg_id, sig)).then([] (auto r) {
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
            return send(dst, rpc_clock_type::now() + timeout, nullptr, args...);
        }
        auto operator()(rpc::client& dst, cancellable& cancel, const InArgs&... args) {
            return send(dst, {}, &cancel, args...);
        }

    };
    return shelper{xt, xsig};
}

template<typename Serializer, typename... RetTypes>
inline future<> reply(wait_type, future<RetTypes...>&& ret, int64_t msg_id, lw_shared_ptr<server::connection> client,
        std::experimental::optional<rpc_clock_type::time_point> timeout) {
    if (!client->error()) {
        snd_buf data;
        try {
            data = apply(marshall<Serializer, const RetTypes&...>,
                    std::tuple_cat(std::make_tuple(std::ref(client->template serializer<Serializer>()), 12), std::move(ret.get())));
        } catch (std::exception& ex) {
            uint32_t len = std::strlen(ex.what());
            data = snd_buf(20 + len);
            auto os = make_serializer_stream(data);
            os.skip(12);
            uint32_t v32 = cpu_to_le(uint32_t(exception_type::USER));
            os.write(reinterpret_cast<char*>(&v32), sizeof(v32));
            v32 = cpu_to_le(len);
            os.write(reinterpret_cast<char*>(&v32), sizeof(v32));
            os.write(ex.what(), len);
            msg_id = -msg_id;
        }

        return client->respond(msg_id, std::move(data), timeout);
    } else {
        ret.ignore_ready_future();
        return make_ready_future<>();
    }
}

// specialization for no_wait_type which does not send a reply
template<typename Serializer>
inline future<> reply(no_wait_type, future<no_wait_type>&& r, int64_t msgid, lw_shared_ptr<server::connection> client, std::experimental::optional<rpc_clock_type::time_point> timeout) {
    try {
        r.get();
    } catch (std::exception& ex) {
        client->get_logger()(client->info(), msgid, to_sstring("exception \"") + ex.what() + "\" in no_wait handler ignored");
    }
    return make_ready_future<>();
}

template<typename Ret, typename... InArgs, typename WantClientInfo, typename WantTimePoint, typename Func, typename ArgsTuple>
inline futurize_t<Ret> apply(Func& func, client_info& info, opt_time_point time_point, WantClientInfo wci, WantTimePoint wtp, signature<Ret (InArgs...)> sig, ArgsTuple&& args) {
    using futurator = futurize<Ret>;
    try {
        return futurator::apply(func, maybe_add_client_info(wci, info, maybe_add_time_point(wtp, time_point, std::forward<ArgsTuple>(args))));
    } catch (std::runtime_error& ex) {
        return futurator::make_exception_future(std::current_exception());
    }
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
auto recv_helper(signature<Ret (InArgs...)> sig, Func&& func, WantClientInfo wci, WantTimePoint wtp) {
    using signature = decltype(sig);
    using wait_style = wait_signature_t<Ret>;
    return [func = lref_to_cref(std::forward<Func>(func))](lw_shared_ptr<server::connection> client,
                                                           std::experimental::optional<rpc_clock_type::time_point> timeout,
                                                           int64_t msg_id,
                                                           rcv_buf data) mutable {
        auto memory_consumed = client->estimate_request_size(data.size);
        if (memory_consumed > client->max_request_size()) {
            auto err = sprint("request size %d large than memory limit %d", memory_consumed, client->max_request_size());
            client->get_logger()(client->peer_address(), err);
            with_gate(client->get_server().reply_gate(), [client, timeout, msg_id, err = std::move(err)] {
                return reply<Serializer>(wait_style(), futurize<Ret>::make_exception_future(std::runtime_error(err.c_str())), msg_id, client, timeout);
            });
            return make_ready_future();
        }
        // note: apply is executed asynchronously with regards to networking so we cannot chain futures here by doing "return apply()"
        auto f = client->wait_for_resources(memory_consumed, timeout).then([client, timeout, msg_id, data = std::move(data), &func] (auto permit) mutable {
            try {
                with_gate(client->get_server().reply_gate(), [client, timeout, msg_id, data = std::move(data), permit = std::move(permit), &func] () mutable {
                    auto args = unmarshall<Serializer, InArgs...>(client->template serializer<Serializer>(), std::move(data));
                    return apply(func, client->info(), timeout, WantClientInfo(), WantTimePoint(), signature(), std::move(args)).then_wrapped([client, timeout, msg_id, permit = std::move(permit)] (futurize_t<Ret> ret) mutable {
                        return reply<Serializer>(wait_style(), std::move(ret), msg_id, client, timeout).then([permit = std::move(permit)] {});
                    });
                });
            } catch (gate_closed_exception&) {/* ignore */ }
        });

        if (timeout) {
            f = f.handle_exception_type([] (semaphore_timed_out&) { /* ignore */ });
        }

        return std::move(f);
    };
}

// helper to create copy constructible lambda from non copy constructible one. std::function<> works only with former kind.
template<typename Func>
auto make_copyable_function(Func&& func, std::enable_if_t<!std::is_copy_constructible<std::decay_t<Func>>::value, void*> = nullptr) {
  auto p = make_lw_shared<typename std::decay_t<Func>>(std::forward<Func>(func));
  return [p] (auto&&... args) { return (*p)( std::forward<decltype(args)>(args)... ); };
}

template<typename Func>
auto make_copyable_function(Func&& func, std::enable_if_t<std::is_copy_constructible<std::decay_t<Func>>::value, void*> = nullptr) {
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
auto protocol<Serializer, MsgType>::make_client(signature<Ret(In...)> clear_sig, MsgType t) {
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
auto protocol<Serializer, MsgType>::register_handler(MsgType t, Func&& func) {
    using sig_type = signature<typename function_traits<Func>::signature>;
    using clean_sig_type = typename sig_type::clean;
    using want_client_info = typename sig_type::want_client_info;
    using want_time_point = typename sig_type::want_time_point;
    auto recv = recv_helper<Serializer>(clean_sig_type(), std::forward<Func>(func),
            want_client_info(), want_time_point());
    register_receiver(t, make_copyable_function(std::move(recv)));
    return make_client(clean_sig_type(), t);
}

}

}

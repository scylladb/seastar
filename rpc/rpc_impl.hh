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

#include <iostream>
#include "core/function_traits.hh"
#include "core/apply.hh"
#include "core/shared_ptr.hh"
#include "core/sstring.hh"
#include "core/future-util.hh"
#include "util/is_smart_ptr.hh"
#include "core/byteorder.hh"

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

// An rpc signature, in the form signature<Ret (In0, In1, In2)>.
template <typename Function>
struct signature;

// General case
template <typename Ret, typename... In>
struct signature<Ret (In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature;
    using want_client_info = dont_want_client_info;
};

// Specialize 'clean' for handlers that receive client_info
template <typename Ret, typename... In>
struct signature<Ret (const client_info&, In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature<Ret (In...)>;
    using want_client_info = do_want_client_info;
};

template <typename Ret, typename... In>
struct signature<Ret (client_info&, In...)> {
    using ret_type = Ret;
    using arg_types = std::tuple<In...>;
    using clean = signature<Ret (In...)>;
    using want_client_info = do_want_client_info;
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
    using cleaned_type = future<void>;
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

struct ignorer {
    template <typename... T>
    ignorer(T&&...) {}
};

template <typename Serializer, typename Output, typename... T>
inline void do_marshall(Serializer& serializer, Output& out, const T&... args) {
    // C++ guarantees that brace-initialization expressions are evaluted in order
    ignorer ignore{(marshall_one(serializer, out, args), 1)...};
}

class measuring_output_stream {
    size_t _size = 0;
public:
    void write(const char* data, size_t size) {
        _size += size;
    }
    size_t size() const {
        return _size;
    }
};

class simple_output_stream {
    char* _p;
public:
    simple_output_stream(sstring& s, size_t start) : _p(s.begin() + start) {}
    void write(const char* data, size_t size) {
        _p = std::copy_n(data, size, _p);
    }
};

template <typename Serializer, typename... T>
inline sstring marshall(Serializer& serializer, size_t head_space, const T&... args) {
    measuring_output_stream measure;
    do_marshall(serializer, measure, args...);
    sstring ret(sstring::initialized_later(), measure.size() + head_space);
    simple_output_stream out(ret, head_space);
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

class simple_input_stream {
    const char* _p;
    size_t _size;
public:
    simple_input_stream(const char* p, size_t size) : _p(p), _size(size) {}
    void read(char* p, size_t size) {
        if (size > _size) {
            throw error("buffer overflow");
        }
        std::copy_n(_p, size, p);
        _p += size;
        _size -= size;
    }
    const size_t size() const {
        return _size;
    }
};

template <typename Serializer, typename... T>
inline std::tuple<T...> unmarshall(Serializer& serializer, temporary_buffer<char> input) {
    simple_input_stream in(input.get(), input.size());
    return do_unmarshall<Serializer, simple_input_stream, T...>(serializer, in);
}

static std::exception_ptr unmarshal_exception(temporary_buffer<char>& data) {
    std::exception_ptr ex;
    auto get = [&data] (size_t size) {
        if (data.size() < size) {
            throw rpc_protocol_error();
        }
        const char *p = data.get();
        data.trim_front(size);
        return p;
    };

    exception_type ex_type = exception_type(le_to_cpu(*unaligned_cast<uint32_t*>(get(4))));
    uint32_t ex_len = le_to_cpu(*unaligned_cast<uint32_t*>(get(4)));
    switch (ex_type) {
    case exception_type::USER:
        ex = std::make_exception_ptr(std::runtime_error(std::string(get(ex_len), ex_len)));
        break;
    case exception_type::UNKNOWN_VERB:
        ex = std::make_exception_ptr(unknown_verb_error(le_to_cpu(*unaligned_cast<uint64_t*>(get(8)))));
        break;
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

template<typename Serializer, typename MsgType, typename T>
struct rcv_reply : rcv_reply_base<T, T> {
    inline void get_reply(typename protocol<Serializer, MsgType>::client& dst, temporary_buffer<char> input) {
        this->set_value(unmarshall<Serializer, T>(dst.serializer(), std::move(input)));
    }
};

template<typename Serializer, typename MsgType, typename... T>
struct rcv_reply<Serializer, MsgType, future<T...>> : rcv_reply_base<std::tuple<T...>, T...> {
    inline void get_reply(typename protocol<Serializer, MsgType>::client& dst, temporary_buffer<char> input) {
        this->set_value(unmarshall<Serializer, T...>(dst.serializer(), std::move(input)));
    }
};

template<typename Serializer, typename MsgType>
struct rcv_reply<Serializer, MsgType, void> : rcv_reply_base<void, void> {
    inline void get_reply(typename protocol<Serializer, MsgType>::client& dst, temporary_buffer<char> input) {
        this->set_value();
    }
};

template<typename Serializer, typename MsgType>
struct rcv_reply<Serializer, MsgType, future<>> : rcv_reply<Serializer, MsgType, void> {};

template <typename Serializer, typename MsgType, typename Ret, typename... InArgs>
inline auto wait_for_reply(wait_type, std::experimental::optional<steady_clock_type::time_point> timeout, typename protocol<Serializer, MsgType>::client& dst, id_type msg_id,
        signature<Ret (InArgs...)> sig) {
    using reply_type = rcv_reply<Serializer, MsgType, Ret>;
    auto lambda = [] (reply_type& r, typename protocol<Serializer, MsgType>::client& dst, id_type msg_id, temporary_buffer<char> data) mutable {
        if (msg_id >= 0) {
            dst.get_stats_internal().replied++;
            return r.get_reply(dst, std::move(data));
        } else {
            dst.get_stats_internal().exception_received++;
            r.done = true;
            r.p.set_exception(unmarshal_exception(data));
        }
    };
    using handler_type = typename protocol<Serializer, MsgType>::client::template reply_handler<reply_type, decltype(lambda)>;
    auto r = std::make_unique<handler_type>(std::move(lambda));
    auto fut = r->reply.p.get_future();
    dst.wait_for_reply(msg_id, std::move(r), timeout);
    return fut;
}

template<typename Serializer, typename MsgType, typename... InArgs>
inline auto wait_for_reply(no_wait_type, std::experimental::optional<steady_clock_type::time_point>, typename protocol<Serializer, MsgType>::client& dst, id_type msg_id,
        signature<no_wait_type (InArgs...)> sig) {  // no_wait overload
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
        auto send(typename protocol<Serializer, MsgType>::client& dst, std::experimental::optional<steady_clock_type::time_point> timeout, const InArgs&... args) {
            if (dst.error()) {
                using cleaned_ret_type = typename wait_signature<Ret>::cleaned_type;
                return futurize<cleaned_ret_type>::make_exception_future(closed_error());
            }

            // send message
            auto msg_id = dst.next_message_id();
            dst.get_stats_internal().pending++;
            sstring data = marshall(dst.serializer(), 20, args...);
            auto p = data.begin();
            *unaligned_cast<uint64_t*>(p) = cpu_to_le(uint64_t(t));
            *unaligned_cast<int64_t*>(p + 8) = cpu_to_le(msg_id);
            *unaligned_cast<uint32_t*>(p + 16) = cpu_to_le(data.size() - 20);
            promise<> sentp;
            future<> sent = sentp.get_future();
            dst.out_ready() = dst.out_ready().then([&dst, data = std::move(data), timeout] () {
                if (timeout && steady_clock_type::now() >= timeout.value()) {
                    return make_ready_future<>(); // if message timed outed drop it without sending
                } else {
                    return dst.out().write(data).then([&dst] {
                        return dst.out().flush();
                    });
                }
            }).finally([&dst, sentp = std::move(sentp)] () mutable {
                sentp.set_value();
                dst.get_stats_internal().pending--;
                dst.get_stats_internal().sent_messages++;
            });

            // prepare reply handler, if return type is now_wait_type this does nothing, since no reply will be sent
            using wait = wait_signature_t<Ret>;
            return when_all(std::move(sent), wait_for_reply<Serializer, MsgType>(wait(), timeout, dst, msg_id, sig)).then([] (auto r) {
                return std::get<1>(std::move(r)); // return result of wait_for_reply
            });
        }
        auto operator()(typename protocol<Serializer, MsgType>::client& dst, const InArgs&... args) {
            return send(dst, {}, args...);
        }
        auto operator()(typename protocol<Serializer, MsgType>::client& dst, steady_clock_type::time_point timeout, const InArgs&... args) {
            return send(dst, timeout, args...);
        }
        auto operator()(typename protocol<Serializer, MsgType>::client& dst, steady_clock_type::duration timeout, const InArgs&... args) {
            return send(dst, steady_clock_type::now() + timeout, args...);
        }
    };
    return shelper{xt, xsig};
}

template <typename Serializer, typename MsgType>
inline
future<>
protocol<Serializer, MsgType>::server::connection::respond(int64_t msg_id, sstring&& data) {
    auto p = data.begin();
    *unaligned_cast<int64_t*>(p) = cpu_to_le(msg_id);
    *unaligned_cast<uint32_t*>(p + 8) = cpu_to_le(data.size() - 12);
    return this->out().write(data.begin(), data.size()).then([conn = this->shared_from_this()] {
        return conn->out().flush();
    });
}

template<typename Serializer, typename MsgType, typename... RetTypes>
inline void reply(wait_type, future<RetTypes...>&& ret, int64_t msg_id, lw_shared_ptr<typename protocol<Serializer, MsgType>::server::connection> client,
        size_t memory_consumed) {
    if (!client->error()) {
        client->out_ready() = client->out_ready().then([&client = *client, msg_id, ret = std::move(ret)] () mutable {
            sstring data;
            client.get_stats_internal().pending++;
            client.get_stats_internal().sent_messages++;
            try {
                data = ::apply(marshall<Serializer, const RetTypes&...>,
                        std::tuple_cat(std::make_tuple(std::ref(client.serializer()), 12), std::move(ret.get())));
            } catch (std::exception& ex) {
                uint32_t len = std::strlen(ex.what());
                data = sstring(sstring::initialized_later(), 20 + len);
                auto p = data.begin() + 12;
                *unaligned_cast<uint32_t*>(p) = le_to_cpu(uint32_t(exception_type::USER));
                *unaligned_cast<uint32_t*>(p + 4) = le_to_cpu(len);
                std::copy_n(ex.what(), len, p + 8);
                msg_id = -msg_id;
            }

            return client.respond(msg_id, std::move(data)).finally([&client]() {
                client.get_stats_internal().pending--;
            });
        }).finally([client, memory_consumed] {
            client->release_resources(memory_consumed);
        });
    } else {
        client->release_resources(memory_consumed);
    }
}

// specialization for no_wait_type which does not send a reply
template<typename Serializer, typename MsgType>
inline void reply(no_wait_type, future<no_wait_type>&& r, int64_t msgid, lw_shared_ptr<typename protocol<Serializer, MsgType>::server::connection> client,
        size_t memory_consumed) {
    client->release_resources(memory_consumed);
    try {
        r.get();
    } catch (std::exception& ex) {
        client->get_protocol().log(client->info(), msgid, to_sstring("exception \"") + ex.what() + "\" in no_wait handler ignored");
    }
}

template<typename Ret, typename... InArgs, typename WantClientInfo, typename Func, typename ArgsTuple>
inline futurize_t<Ret> apply(Func& func, client_info& info, WantClientInfo wci, signature<Ret (InArgs...)> sig, ArgsTuple&& args) {
    using futurator = futurize<Ret>;
    try {
        return futurator::apply(func, maybe_add_client_info(wci, info, std::forward<ArgsTuple>(args)));
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
template <typename Serializer, typename MsgType, typename Func, typename Ret, typename... InArgs, typename WantClientInfo>
auto recv_helper(signature<Ret (InArgs...)> sig, Func&& func, WantClientInfo wci) {
    using signature = decltype(sig);
    using wait_style = wait_signature_t<Ret>;
    return [func = lref_to_cref(std::forward<Func>(func))](lw_shared_ptr<typename protocol<Serializer, MsgType>::server::connection> client,
                                                           int64_t msg_id,
                                                           temporary_buffer<char> data) mutable {
        auto memory_consumed = client->estimate_request_size(data.size());
        auto args = unmarshall<Serializer, InArgs...>(client->serializer(), std::move(data));
        // note: apply is executed asynchronously with regards to networking so we cannot chain futures here by doing "return apply()"
        return client->wait_for_resources(memory_consumed).then([client, msg_id, memory_consumed, args = std::move(args), &func] () mutable {
          apply(func, client->info(), WantClientInfo(), signature(), std::move(args)).then_wrapped(
                [client, msg_id, memory_consumed] (futurize_t<typename signature::ret_type> ret) mutable {
            reply<Serializer, MsgType>(wait_style(), std::move(ret), msg_id, std::move(client), memory_consumed);
          });
        });
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

template<typename Ret, typename... Args>
struct handler_type_helper {
    using type = Ret(Args...);
    static constexpr bool info = false;
};

template<typename Ret, typename First, typename... Args>
struct handler_type_helper<Ret, First, Args...> {
    using type = Ret(First, Args...);
    static constexpr bool info = false;
};

template<typename Ret, typename... Args>
struct handler_type_helper<Ret, const client_info&, Args...> {
    using type = Ret(Args...);
    static constexpr bool info = true;
};

template<typename Ret, typename... Args>
struct handler_type_helper<Ret, client_info&, Args...> {
    using type = Ret(Args...);
    static constexpr bool info = true;
};

template<typename Ret, typename... Args>
struct handler_type_helper<Ret, client_info, Args...> {
    using type = Ret(Args...);
    static constexpr bool info = true;
};

template<typename Ret, typename F, typename I>
struct handler_type_impl;

template<typename Ret, typename F, std::size_t... I>
struct handler_type_impl<Ret, F, std::integer_sequence<std::size_t, I...>> {
    using type = handler_type_helper<Ret, typename remove_optional<typename F::template arg<I>::type>::type...>;
};

// this class is used to calculate client side rpc function signature
// if rpc callback receives client_info as a first parameter it is dropped
// from an argument list and if return type is a smart pointer it is converted to be
// a type it points to, otherwise signature is identical to what was passed to
// make_client().
//
// Examples:
// std::unique_ptr<int>(client_info, int, long) -> int(int, long)
// double(client_info, float) -> double(float)
template<typename Func>
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

    using trait = function_traits<Func>;
    // if return type is smart ptr take a type it points to instead
    using return_type = typename drop_smart_ptr<typename trait::return_type>::type;
public:
    using type = typename handler_type_impl<return_type, trait, std::make_index_sequence<trait::arity>>::type::type;
};

template<typename Serializer, typename MsgType>
template<typename Func>
auto protocol<Serializer, MsgType>::make_client(MsgType t) {
    using trait = function_traits<typename client_function_type<Func>::type>;
    using sig_type = signature<typename trait::signature>;
    return send_helper<Serializer>(t, sig_type());
}

template<typename Serializer, typename MsgType>
template<typename Func>
auto protocol<Serializer, MsgType>::register_handler(MsgType t, Func&& func) {
    using sig_type = signature<typename function_traits<Func>::signature>;
    using clean_sig_type = typename sig_type::clean;
    using want_client_info = typename sig_type::want_client_info;
    auto recv = recv_helper<Serializer, MsgType>(clean_sig_type(), std::forward<Func>(func),
            want_client_info());
    register_receiver(t, make_copyable_function(std::move(recv)));
    return make_client<Func>(t);
}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::server::server(protocol<Serializer, MsgType>& proto, ipv4_addr addr, resource_limits limits)
    : server(proto, engine().listen(addr, listen_options(true)), limits)
{}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::server::server(protocol<Serializer, MsgType>& proto, server_socket ss, resource_limits limits)
        : _proto(proto), _ss(std::move(ss)), _limits(limits), _resources_available(limits.max_memory)
{
    accept();
}

template<typename Serializer, typename MsgType>
void protocol<Serializer, MsgType>::server::accept() {
    keep_doing([this] () mutable {
        return _ss.accept().then([this] (connected_socket fd, socket_address addr) mutable {
            fd.set_nodelay(true);
            auto conn = make_lw_shared<connection>(*this, std::move(fd), std::move(addr), _proto);
            _conns.insert(conn.get());
            conn->process();
        });
    }).then_wrapped([this] (future<>&& f){
        try {
            f.get();
            assert(false);
        } catch (...) {
            _ss_stopped.set_value();
        }
    });
}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::server::connection::connection(protocol<Serializer, MsgType>::server& s, connected_socket&& fd, socket_address&& addr, protocol<Serializer, MsgType>& proto)
    : protocol<Serializer, MsgType>::connection(std::move(fd), proto), _server(s) {
    _info.addr = std::move(addr);
}

template<typename Connection>
static bool verify_frame(Connection& c, temporary_buffer<char>& buf, size_t expected, const char* log) {
    if (buf.size() != expected) {
        if (buf.size() != 0) {
            c.get_protocol().log(c.peer_address(), log);
        }
        return false;
    }
    return true;
}

template<typename Connection>
static void send_negotiation_frame(Connection& c, const negotiation_frame& nf) {
    sstring reply(sstring::initialized_later(), sizeof(negotiation_frame));
    auto p = reply.begin();
    p = std::copy_n(rpc_magic, sizeof(nf.magic), p);
    *unaligned_cast<uint32_t*>(p ) = cpu_to_le(nf.required_features_mask);
    *unaligned_cast<uint32_t*>(p + 4) = cpu_to_le(nf.optional_features_mask);
    *unaligned_cast<uint32_t*>(p + 8) = cpu_to_le(nf.len);
    c.out_ready() = c.out_ready().then([&c, reply = std::move(reply)] () mutable {
        return c.out().write(reply);
    }).then([&c] {
        return c.out().flush();
    });
}

template<typename Connection>
static
future<negotiation_frame> receive_negotiation_frame(Connection& c, input_stream<char>& in) {
    return in.read_exactly(sizeof(negotiation_frame)).then([&c, &in] (temporary_buffer<char> neg) {
        if (!verify_frame(c, neg, sizeof(negotiation_frame), "unexpected eof during negotiation frame")) {
            return make_exception_future<negotiation_frame>(closed_error());
        }
        negotiation_frame* frame = reinterpret_cast<negotiation_frame*>(neg.get_write());
        if (std::memcmp(frame->magic, rpc_magic, sizeof(frame->magic)) != 0) {
            c.get_protocol().log(c.peer_address(), "wrong protocol magic");
            return make_exception_future<negotiation_frame>(closed_error());
        }
        frame->required_features_mask = le_to_cpu(frame->required_features_mask);
        frame->optional_features_mask = le_to_cpu(frame->required_features_mask);
        frame->len = le_to_cpu(frame->len);
        return make_ready_future<negotiation_frame>(*frame);
    });
}

template<typename Connection>
static
future<temporary_buffer<char>> receive_additional_negotiation_data(Connection& c, input_stream<char>& in, size_t len) {
    if (len == 0) {
        return make_ready_future<temporary_buffer<char>>(temporary_buffer<char>());
    } else {
        return in.read_exactly(len).then([&c, &in, len] (temporary_buffer<char> neg) {
            if (!verify_frame(c, neg, len, "unexpected eof during negotiation frame")) {
                return make_exception_future<temporary_buffer<char>>(closed_error());
            }
            return make_ready_future<temporary_buffer<char>>(std::move(neg));
        });
    }
}

template<typename Connection>
static
future<negotiation_frame> verify_negotiation_data(Connection& c, input_stream<char>& in, negotiation_frame nf) {
    return receive_additional_negotiation_data(c, in, nf.len).then([&c, nf] (temporary_buffer<char> buf) mutable {
        if (nf.required_features_mask != 0) {
            c.get_protocol().log(c.peer_address(), "negotiation failed: unsupported required features");
            return make_exception_future<negotiation_frame>(closed_error());
        }
        return make_ready_future<negotiation_frame>(nf);
    });
}

template<typename Serializer, typename MsgType>
future<negotiation_frame>
protocol<Serializer, MsgType>::server::connection::negotiate_protocol(input_stream<char>& in) {
    return receive_negotiation_frame(*this, in).then([this, &in] (negotiation_frame nf) {
        negotiation_frame mine = {{}, 0, 0, 0};
        send_negotiation_frame(*this, mine);
        return verify_negotiation_data(*this, in, nf);
    });
}

template <typename Serializer, typename MsgType>
future<MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>
protocol<Serializer, MsgType>::server::connection::read_request_frame(input_stream<char>& in) {
    return in.read_exactly(20).then([this, &in] (temporary_buffer<char> header) {
        if (header.size() != 20) {
            if (header.size() != 0) {
                this->_server._proto.log(_info, "unexpected eof");
            }
            return make_ready_future<MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>(MsgType(0), 0, std::experimental::optional<temporary_buffer<char>>());
        }
        auto ptr = header.get();
        auto type = MsgType(le_to_cpu(*unaligned_cast<uint64_t>(ptr)));
        auto msgid = le_to_cpu(*unaligned_cast<int64_t*>(ptr + 8));
        auto size = le_to_cpu(*unaligned_cast<uint32_t*>(ptr + 16));
        return in.read_exactly(size).then([this, type, msgid, size] (temporary_buffer<char> data) {
            if (data.size() != size) {
                this->_server._proto.log(_info, "unexpected eof");
                return make_ready_future<MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>(MsgType(0), 0, std::experimental::optional<temporary_buffer<char>>());
            }
            return make_ready_future<MsgType, int64_t, std::experimental::optional<temporary_buffer<char>>>(type, msgid, std::experimental::optional<temporary_buffer<char>>(std::move(data)));
        });
    });
}

template<typename Serializer, typename MsgType>
future<> protocol<Serializer, MsgType>::server::connection::process() {
    return this->negotiate_protocol(this->_read_buf).then([this] (negotiation_frame frame) mutable {
        return do_until([this] { return this->_read_buf.eof() || this->_error; }, [this] () mutable {
            return this->read_request_frame(this->_read_buf).then([this] (MsgType type, int64_t msg_id, std::experimental::optional<temporary_buffer<char>> data) {
                if (!data) {
                    this->_error = true;
                } else {
                    auto it = _server._proto._handlers.find(type);
                    if (it != _server._proto._handlers.end()) {
                        it->second(this->shared_from_this(), msg_id, std::move(data.value()));
                    } else {
                        // send unknown_verb exception back
                        auto data = sstring(sstring::initialized_later(), 28);
                        auto p = data.begin() + 12;
                        *unaligned_cast<uint32_t*>(p) = cpu_to_le(uint32_t(exception_type::UNKNOWN_VERB));
                        *unaligned_cast<uint32_t*>(p + 4) = cpu_to_le(uint32_t(8));
                        *unaligned_cast<uint64_t*>(p + 8) = cpu_to_le(uint64_t(type));
                        this->get_stats_internal().pending++;
                        this->respond(-msg_id, std::move(data)).finally([this]() {
                            this->get_stats_internal().pending--;
                        });

                    }
                }
            });
        });
    }).then_wrapped([this] (future<> f) {
        f.ignore_ready_future();
        this->_error = true;
        return this->out_ready().then_wrapped([this] (future<> f) {
            f.ignore_ready_future();
            return this->_write_buf.close();
        }).then_wrapped([this] (future<> f) {
            f.ignore_ready_future();
            if (!this->_server._stopping) {
                // if server is stopping do not remove connection
                // since it may invalidate _conns iterators
                this->_server._conns.erase(this);
            }
            this->_stopped.set_value();
        });
    }).finally([conn_ptr = this->shared_from_this()] {
        // hold onto connection pointer until do_until() exists
    });
}

template<typename Serializer, typename MsgType>
future<negotiation_frame>
protocol<Serializer, MsgType>::client::negotiate_protocol(input_stream<char>& in) {
    return receive_negotiation_frame(*this, in).then([this, &in] (negotiation_frame nf) {
        return verify_negotiation_data(*this, in, nf);
    });
}

// FIXME: take out-of-line?
template<typename Serializer, typename MsgType>
inline
future<int64_t, std::experimental::optional<temporary_buffer<char>>>
protocol<Serializer, MsgType>::client::read_response_frame(input_stream<char>& in) {
    return in.read_exactly(12).then([this, &in] (temporary_buffer<char> header) {
        if (header.size() != 12) {
            if (header.size() != 0) {
                this->_proto.log(this->_server_addr, "unexpected eof");
            }
            return make_ready_future<int64_t, std::experimental::optional<temporary_buffer<char>>>(0, std::experimental::optional<temporary_buffer<char>>());
        }

        auto ptr = header.get();
        auto msgid = le_to_cpu(*unaligned_cast<int64_t*>(ptr));
        auto size = le_to_cpu(*unaligned_cast<uint32_t*>(ptr + 8));
        return in.read_exactly(size).then([this, msgid, size] (temporary_buffer<char> data) {
            if (data.size() != size) {
                this->_proto.log(this->_server_addr, "unexpected eof");
                return make_ready_future<int64_t, std::experimental::optional<temporary_buffer<char>>>(0, std::experimental::optional<temporary_buffer<char>>());
            }
            return make_ready_future<int64_t, std::experimental::optional<temporary_buffer<char>>>(msgid, std::experimental::optional<temporary_buffer<char>>(std::move(data)));
        });
    });
}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::client::client(protocol& proto, ipv4_addr addr, future<connected_socket> f) : protocol<Serializer, MsgType>::connection(proto), _server_addr(addr) {
    this->_output_ready = _connected_promise.get_future();
    negotiation_frame nf = {{}, 0, 0, 0};
    send_negotiation_frame(*this, nf);
    f.then([this] (connected_socket fd) {
        fd.set_nodelay(true);
        this->_fd = std::move(fd);
        this->_read_buf = this->_fd.input();
        this->_write_buf = this->_fd.output();
        this->_connected_promise.set_value();
        this->_connected = true;
        return this->negotiate_protocol(this->_read_buf).then([this] (negotiation_frame frame) {
            return do_until([this] { return this->_read_buf.eof() || this->_error; }, [this] () mutable {
                return this->read_response_frame(this->_read_buf).then([this] (int64_t msg_id, std::experimental::optional<temporary_buffer<char>> data) {
                    auto it = _outstanding.find(::abs(msg_id));
                    if (!data) {
                        this->_error = true;
                    } else if (it != _outstanding.end()) {
                        auto handler = std::move(it->second);
                        _outstanding.erase(it);
                        (*handler)(*this, msg_id, std::move(data.value()));
                    } else if (msg_id < 0) {
                        try {
                            std::rethrow_exception(unmarshal_exception(data.value()));
                        } catch(const unknown_verb_error& ex) {
                            // if this is unknown verb exception with unknown id ignore it
                            // can happen if unknown verb was used by no_wait client
                            this->get_protocol().log(this->peer_address(), sprint("unknown verb exception %d ignored", ex.type));
                        } catch(...) {
                            this->_error = true;
                        }
                    } else {
                        this->_error = true;
                    }
                });
            });
        });
    }).then_wrapped([this] (future<> f){
        f.ignore_ready_future();
        this->_error = true;
        auto need_close = _connected;
        if (!_connected) {
            this->_connected_promise.set_exception(closed_error());
        }
        _connected = false; // prevent running shutdown() on this
        this->_output_ready.then_wrapped([this, need_close] (future<> f) {
            f.ignore_ready_future();
            return need_close ? this->_write_buf.close() : make_ready_future<>();
        }).then_wrapped([this] (future<> f) {
            f.ignore_ready_future();
            this->_stopped.set_value();
            this->_outstanding.clear();
        });
    });
}

template<typename Serializer, typename MsgType>
protocol<Serializer, MsgType>::client::client(protocol<Serializer, MsgType>& proto, ipv4_addr addr, ipv4_addr local)
    : client(proto, addr, ::connect(addr, local))
{}

}

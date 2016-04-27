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
 * Copyright (C) 2016 ScyllaDB
 */


#include "loopback_socket.hh"
#include "rpc/rpc.hh"
#include "test-utils.hh"
#include "core/thread.hh"

struct serializer {
};

template <typename T, typename Output>
inline
void write_arithmetic_type(Output& out, T v) {
    static_assert(std::is_arithmetic<T>::value, "must be arithmetic type");
    return out.write(reinterpret_cast<const char*>(&v), sizeof(T));
}

template <typename T, typename Input>
inline
T read_arithmetic_type(Input& in) {
    static_assert(std::is_arithmetic<T>::value, "must be arithmetic type");
    T v;
    in.read(reinterpret_cast<char*>(&v), sizeof(T));
    return v;
}

template <typename Output>
inline void write(serializer, Output& output, int32_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, uint32_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, int64_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, uint64_t v) { return write_arithmetic_type(output, v); }
template <typename Output>
inline void write(serializer, Output& output, double v) { return write_arithmetic_type(output, v); }
template <typename Input>
inline int32_t read(serializer, Input& input, rpc::type<int32_t>) { return read_arithmetic_type<int32_t>(input); }
template <typename Input>
inline uint32_t read(serializer, Input& input, rpc::type<uint32_t>) { return read_arithmetic_type<uint32_t>(input); }
template <typename Input>
inline uint64_t read(serializer, Input& input, rpc::type<uint64_t>) { return read_arithmetic_type<uint64_t>(input); }
template <typename Input>
inline uint64_t read(serializer, Input& input, rpc::type<int64_t>) { return read_arithmetic_type<int64_t>(input); }
template <typename Input>
inline double read(serializer, Input& input, rpc::type<double>) { return read_arithmetic_type<double>(input); }

template <typename Output>
inline void write(serializer, Output& out, const sstring& v) {
    write_arithmetic_type(out, uint32_t(v.size()));
    out.write(v.c_str(), v.size());
}

template <typename Input>
inline sstring read(serializer, Input& in, rpc::type<sstring>) {
    auto size = read_arithmetic_type<uint32_t>(in);
    sstring ret(sstring::initialized_later(), size);
    in.read(ret.begin(), size);
    return ret;
}

using test_rpc_proto = rpc::protocol<serializer>;
using connect_fn = std::function<test_rpc_proto::client (ipv4_addr addr)>;

future<>
with_rpc_env(rpc::resource_limits resource_limits,
        std::function<future<> (test_rpc_proto& proto, test_rpc_proto::server& server, connect_fn connect)> test_fn) {
    struct state {
        test_rpc_proto proto{serializer()};
        loopback_connection_factory lcf;
        std::unique_ptr<test_rpc_proto::server> server;
    };
    return do_with(state(), [=] (state& s) {
        s.server = std::make_unique<test_rpc_proto::server>(s.proto, s.lcf.get_server_socket(), resource_limits);
        auto make_client = [&s] (ipv4_addr addr) {
            return test_rpc_proto::client(s.proto, addr, s.lcf.make_new_connection());
        };
        return test_fn(s.proto, *s.server, make_client).finally([&] {
            return s.server->stop();
        });
    });
}


SEASTAR_TEST_CASE(test_rpc_connect) {
    return with_rpc_env({}, [] (test_rpc_proto& proto, test_rpc_proto::server& s, connect_fn connect) {
        return seastar::async([&proto, &s, connect] {
            auto c1 = connect(ipv4_addr());
            auto sum = proto.register_handler(1, [](int a, int b) {
                return make_ready_future<int>(a+b);
            });
            auto result = sum(c1, 2, 3).get0();
            BOOST_REQUIRE_EQUAL(result, 2 + 3);
            c1.stop().get();
        });
    });
}

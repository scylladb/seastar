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
 * Copyright (C) 2022 ScyllaDB
 */

#include <yaml-cpp/yaml.h>
#include <fmt/core.h>
#include <seastar/core/app-template.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/sharded.hh>
#include <seastar/rpc/rpc.hh>

using namespace seastar;

struct serializer {};

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
    sstring ret = uninitialized_string(size);
    in.read(ret.data(), size);
    return ret;
}

struct client_config {
    bool nodelay = true;
};

struct server_config {
    bool nodelay = true;
};

struct config {
    client_config client;
    server_config server;
};

namespace YAML {

template<>
struct convert<client_config> {
    static bool decode(const Node& node, client_config& cfg) {
        if (node["nodelay"]) {
            cfg.nodelay = node["nodelay"].as<bool>();
        }
        return true;
    }
};

template<>
struct convert<server_config> {
    static bool decode(const Node& node, server_config& cfg) {
        if (node["nodelay"]) {
            cfg.nodelay = node["nodelay"].as<bool>();
        }
        return true;
    }
};

template<>
struct convert<config> {
    static bool decode(const Node& node, config& cfg) {
        if (node["client"]) {
            cfg.client = node["client"].as<client_config>();
        }
        if (node["server"]) {
            cfg.server = node["server"].as<server_config>();
        }
        return true;
    }
};

} // YAML namespace

enum class rpc_verb : int32_t {
    HELLO = 0,
    BYE = 1,
};

using rpc_protocol = rpc::protocol<serializer, rpc_verb>;

class context {
    std::unique_ptr<rpc_protocol> _rpc;
    std::unique_ptr<rpc_protocol::server> _server;
    std::unique_ptr<rpc_protocol::client> _client;
    promise<> _bye;
    config _cfg;

public:
    context(std::optional<ipv4_addr> laddr, std::optional<ipv4_addr> caddr, uint16_t port, config cfg)
            : _rpc(std::make_unique<rpc_protocol>(serializer{}))
            , _cfg(cfg)
    {
        _rpc->register_handler(rpc_verb::HELLO, [] {
            fmt::print("Got HELLO message from client\n");
        });
        _rpc->register_handler(rpc_verb::BYE, [this] {
            fmt::print("Got BYE message from client, exiting\n");
            _bye.set_value();
        });

        if (laddr) {
            rpc::server_options so;
            so.tcp_nodelay = _cfg.server.nodelay;
            rpc::resource_limits limits;
            _server = std::make_unique<rpc_protocol::server>(*_rpc, so, *laddr, limits);
        }

        if (caddr) {
            rpc::client_options co;
            co.tcp_nodelay = _cfg.client.nodelay;
            _client = std::make_unique<rpc_protocol::client>(*_rpc, co, *caddr);
        }
    }

    future<> start() {
        if (_client) {
            return _rpc->make_client<void()>(rpc_verb::HELLO)(*_client);
        }

        return make_ready_future<>();
    }

    future<> stop() {
        if (_client) {
            return _rpc->make_client<void()>(rpc_verb::BYE)(*_client).finally([this] {
                return _client->stop();
            });
        }

        if (_server) {
            return _server->stop();
        }

        return make_ready_future<>();
    }

    future<> run() {
        if (_client) {
            return make_ready_future<>();
        }

        if (_server) {
            return _bye.get_future();
        }

        return make_ready_future<>();
    }
};

int main(int ac, char** av) {
    namespace bpo = boost::program_options;

    app_template app;
    auto opt_add = app.add_options();
    opt_add
        ("listen", bpo::value<sstring>()->default_value(""), "address to start server on")
        ("connect", bpo::value<sstring>()->default_value(""), "address to connect client to")
        ("port", bpo::value<int>()->default_value(9123), "port to listen on or connect to")
        ("conf", bpo::value<sstring>()->default_value("./conf.yaml"), "config with jobs and options")
    ;

    sharded<context> ctx;
    return app.run(ac, av, [&] {
        return seastar::async([&] {
            auto& opts = app.configuration();
            auto& listen = opts["listen"].as<sstring>();
            auto& connect = opts["connect"].as<sstring>();
            auto& port = opts["port"].as<int>();
            auto& conf = opts["conf"].as<sstring>();

            std::optional<ipv4_addr> laddr;
            if (listen != "") {
                laddr.emplace(listen, port);
            }
            std::optional<ipv4_addr> caddr;
            if (connect != "") {
                caddr.emplace(connect, port);
            }

            YAML::Node doc = YAML::LoadFile(conf);
            auto cfg = doc.as<config>();

            ctx.start(laddr, caddr, port, cfg).get();
            ctx.invoke_on_all(&context::start).get();
            ctx.invoke_on_all(&context::run).get();
            ctx.stop().get();
        });
    });
}

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
 * Copyright 2015 Cloudius Systems
 */

#pragma once

#ifndef SEASTAR_MODULE
#include <limits>
#include <cctype>
#include <vector>
#include <boost/intrusive/list.hpp>
#endif
#include <seastar/http/request_parser.hh>
#include <seastar/http/request.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#include <seastar/http/routes.hh>
#include <seastar/net/tls.hh>
#include <seastar/core/shared_ptr.hh>

namespace seastar {

namespace http {
SEASTAR_MODULE_EXPORT
struct reply;
}

namespace httpd {

SEASTAR_MODULE_EXPORT
class http_server;
SEASTAR_MODULE_EXPORT
class http_stats;

using namespace std::chrono_literals;

SEASTAR_MODULE_EXPORT_BEGIN
class http_stats {
    metrics::metric_groups _metric_groups;
public:
    http_stats(http_server& server, const sstring& name);
};

class connection : public boost::intrusive::list_base_hook<> {
    http_server& _server;
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    socket_address _client_addr;
    socket_address _server_addr;
    static constexpr size_t limit = 4096;
    using tmp_buf = temporary_buffer<char>;
    http_request_parser _parser;
    std::unique_ptr<http::request> _req;
    std::unique_ptr<http::reply> _resp;
    // null element marks eof
    queue<std::unique_ptr<http::reply>> _replies { 10 };
    bool _done = false;
    const bool _tls;
public:
    [[deprecated("use connection(http_server&, connected_socket&&, bool tls)")]]
    connection(http_server& server, connected_socket&& fd, socket_address, bool tls)
            : connection(server, std::move(fd), tls) {}
    connection(http_server& server, connected_socket&& fd, bool tls)
            : _server(server)
            , _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output())
            , _client_addr(_fd.remote_address())
            , _server_addr(_fd.local_address())
            , _tls(tls) {
        on_new_connection();
    }
    connection(http_server& server, connected_socket&& fd,
            socket_address client_addr, socket_address server_addr, bool tls)
            : _server(server)
            , _fd(std::move(fd))
            , _read_buf(_fd.input())
            , _write_buf(_fd.output())
            , _client_addr(std::move(client_addr))
            , _server_addr(std::move(server_addr))
            , _tls(tls) {
        on_new_connection();
    }
    ~connection();
    void on_new_connection();

    future<> process();
    void shutdown();
    future<> read();
    future<> read_one();
    future<> respond();
    future<> do_response_loop();

    void set_headers(http::reply& resp);

    future<> start_response();

    future<bool> generate_reply(std::unique_ptr<http::request> req);
    void generate_error_reply_and_close(std::unique_ptr<http::request> req, http::reply::status_type status, const sstring& msg);

    future<> write_body();

    output_stream<char>& out();
};

class http_server_tester;

class http_server {
    std::vector<server_socket> _listeners;
    http_stats _stats;
    uint64_t _total_connections = 0;
    uint64_t _current_connections = 0;
    uint64_t _requests_served = 0;
    uint64_t _read_errors = 0;
    uint64_t _respond_errors = 0;
    shared_ptr<seastar::tls::server_credentials> _credentials;
    sstring _date = http_date();
    timer<> _date_format_timer { [this] {_date = http_date();} };
    size_t _content_length_limit = std::numeric_limits<size_t>::max();
    bool _content_streaming = false;
    gate _task_gate;
public:
    routes _routes;
    using connection = seastar::httpd::connection;
    using server_credentials_ptr = shared_ptr<seastar::tls::server_credentials>;
    explicit http_server(const sstring& name) : _stats(*this, name) {
        _date_format_timer.arm_periodic(1s);
    }
    /*!
     * \brief set tls credentials for the server
     * Setting the tls credentials will set the http-server to work in https mode.
     *
     * To use the https, create server credentials and pass it to the server before it starts.
     *
     * Use case example using seastar threads for clarity:

        distributed<http_server> server; // typical server

        seastar::shared_ptr<seastar::tls::credentials_builder> creds = seastar::make_shared<seastar::tls::credentials_builder>();
        sstring ms_cert = "MyCertificate.crt";
        sstring ms_key = "MyKey.key";

        creds->set_dh_level(seastar::tls::dh_params::level::MEDIUM);

        creds->set_x509_key_file(ms_cert, ms_key, seastar::tls::x509_crt_format::PEM).get();
        creds->set_system_trust().get();


        server.invoke_on_all([creds](http_server& server) {
            server.set_tls_credentials(creds->build_server_credentials());
            return make_ready_future<>();
        }).get();
     *
     */
    [[deprecated("use listen(socket_address addr, server_credentials_ptr credentials)")]]
    void set_tls_credentials(server_credentials_ptr credentials);

    size_t get_content_length_limit() const;

    void set_content_length_limit(size_t limit);

    bool get_content_streaming() const;

    void set_content_streaming(bool b);

    future<> listen(socket_address addr, server_credentials_ptr credentials);
    future<> listen(socket_address addr, listen_options lo, server_credentials_ptr credentials);
    future<> listen(socket_address addr, listen_options lo);
    future<> listen(socket_address addr);
    future<> stop();

    future<> do_accepts(int which);
    future<> do_accepts(int which, bool with_tls);

    uint64_t total_connections() const;
    uint64_t current_connections() const;
    uint64_t requests_served() const;
    uint64_t read_errors() const;
    uint64_t reply_errors() const;
    // Write the current date in the specific "preferred format" defined in
    // RFC 7231, Section 7.1.1.1.
    static sstring http_date();
private:
    future<> do_accept_one(int which, bool with_tls);
    boost::intrusive::list<connection> _connections;
    friend class seastar::httpd::connection;
    friend class http_server_tester;
};

class http_server_tester {
public:
    static std::vector<server_socket>& listeners(http_server& server) {
        return server._listeners;
    }
};

/*
 * A helper class to start, set and listen an http server
 * typical use would be:
 *
 * auto server = new http_server_control();
 *                 server->start().then([server] {
 *                 server->set_routes(set_routes);
 *              }).then([server, port] {
 *                  server->listen(port);
 *              }).then([port] {
 *                  std::cout << "Seastar HTTP server listening on port " << port << " ...\n";
 *              });
 */
class http_server_control {
    std::unique_ptr<distributed<http_server>> _server_dist;
private:
    static sstring generate_server_name();
public:
    http_server_control() : _server_dist(new distributed<http_server>) {
    }

    future<> start(const sstring& name = generate_server_name());
    future<> stop() noexcept;
    future<> set_routes(std::function<void(routes& r)> fun);
    future<> listen(socket_address addr);
    future<> listen(socket_address addr, http_server::server_credentials_ptr credentials);
    future<> listen(socket_address addr, listen_options lo);
    future<> listen(socket_address addr, listen_options lo, http_server::server_credentials_ptr credentials);
    distributed<http_server>& server();
};
SEASTAR_MODULE_EXPORT_END
}

}

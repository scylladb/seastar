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

#include <functional>
#include <limits>
#include <cctype>
#include <vector>
#include <optional>
#include <boost/intrusive/list.hpp>
#include <seastar/http/request_parser.hh>
#include <seastar/http/request.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/http/routes.hh>
#include <seastar/net/tls.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/scheduling.hh>

namespace seastar {

namespace http {
struct reply;
}

namespace httpd {

class http_server;
class http_stats;

using namespace std::chrono_literals;

class http_stats {
    metrics::metric_groups _metric_groups;
public:
    http_stats(http_server& server, const sstring& name);
};

class connection : public boost::intrusive::list_base_hook<> {
    http_server& _server;
    connected_socket _fd;
    std::optional<session_dn> _tls_dn;
    std::optional<std::vector<tls::subject_alt_name>> _tls_san;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    socket_address _client_addr;
    socket_address _server_addr;
    static constexpr size_t limit = 4096;
    using tmp_buf = temporary_buffer<char>;
    http_request_parser _parser;
    std::unique_ptr<http::reply> _resp;
    // null element marks eof
    queue<std::unique_ptr<http::reply>> _replies { 10 };
    bool _done = false;
    const bool _tls;
public:
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

    future<> prepare();
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
    uint64_t _tls_handshake_errors = 0;
    shared_ptr<seastar::tls::server_credentials> _credentials;
    sstring _date = http_date();
    timer<> _date_format_timer { [this] {_date = http_date();} };
    size_t _content_length_limit = std::numeric_limits<size_t>::max();
    bool _content_streaming = false;
    std::optional<sstring> _server_header = sstring("Seastar httpd");
    bool _generate_date_header = true;
    gate _task_gate;
    std::optional<net::keepalive_params> _keepalive_params;
    std::optional<scheduling_group> _request_scheduling_group;
public:
    routes _routes;
    using connection = seastar::httpd::connection;
    using server_credentials_ptr = shared_ptr<seastar::tls::server_credentials>;
    explicit http_server(const sstring& name) : _stats(*this, name) {
        _date_format_timer.arm_periodic(1s);
    }

    void set_keepalive_parameters(std::optional<net::keepalive_params> params) {
        _keepalive_params = std::move(params);
    }

    size_t get_content_length_limit() const;

    void set_content_length_limit(size_t limit);

    bool get_content_streaming() const;

    void set_content_streaming(bool b);

    /// Returns the value of the "Server" header that will be added to each response.
    /// std::nullopt means the header will not be added.
    const std::optional<sstring>& get_server_header() const;

    /// Sets the value of the "Server" header added to each response.
    /// Pass std::nullopt to suppress the header entirely.
    void set_server_header(std::optional<sstring> value);

    /// Returns whether the server adds a "Date" header to each response.
    bool get_generate_date_header() const;

    /// Controls whether the server adds a "Date" header to each response.
    /// When set to false the periodic date-update timer is also stopped.
    void set_generate_date_header(bool b);

    /// Sets the scheduling group used for request processing.
    ///
    /// Connection setup (including TLS handshake when enabled) runs in the
    /// scheduling group of the accept loop. After setup completes, the
    /// connection switches to the configured group before processing requests.
    /// Without this setting, request processing continues in the accept loop
    /// scheduling group.
    void set_request_scheduling_group(scheduling_group sg);

    future<> listen(socket_address addr, server_credentials_ptr credentials);
    future<> listen(socket_address addr, listen_options lo, server_credentials_ptr credentials);
    future<> listen(socket_address addr, listen_options lo);
    future<> listen(socket_address addr);

    /// Serve a socket the caller created, rather than one this server binds itself.
    ///
    /// Useful for serving something that is not a TCP socket at all - a loopback pair in
    /// a test, say - and for a caller that wants to bind with options this class does not
    /// expose. \c tls says whether the socket is already wrapped in TLS: a socket that
    /// arrives ready-made carries no sign of it, and a connection accepted on it has to
    /// know in order to report the client's identity to a handler.
    future<> listen(server_socket&& ss, bool tls = false);

    /// The addresses this server is listening on, in no particular order. A listener that
    /// asked for port 0 appears here under the port it was actually given.
    std::vector<socket_address> listening_addresses() const;

    // Starting the accept loop is not something a caller has to do: listen() starts one
    // for the socket it creates, and listen(server_socket&&) for the socket it is given,
    // so calling these on top of that starts a second loop on a socket that already has
    // one. They are kept for callers that relied on them.
    [[deprecated("listen() starts accepting by itself; to serve a socket you created, use listen(server_socket&&)")]]
    future<> do_accepts(int which);
    [[deprecated("listen() starts accepting by itself; to serve a socket you created, use listen(server_socket&&)")]]
    future<> do_accepts(int which, bool with_tls);
    [[deprecated("listen() starts accepting by itself; to serve a socket you created, use listen(server_socket&&)")]]
    future<> accept_loop(int which, bool tls);

    future<> stop();

    uint64_t total_connections() const;
    uint64_t current_connections() const;
    uint64_t requests_served() const;
    uint64_t read_errors() const;
    uint64_t reply_errors() const;
    uint64_t tls_handshake_errors() const;
    // Write the current date in the specific "preferred format" defined in
    // RFC 7231, Section 7.1.1.1.
    static sstring http_date();
private:
    future<> start_accepting(int which, bool with_tls);
    future<> run_accept_loop(int which, bool tls);
    future<> do_accept_one(int which, bool with_tls);
    future<> do_process_connection(connected_socket conn_fd, socket_address remote_address, bool tls);
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
    std::unique_ptr<sharded<http_server>> _server_dist;
private:
    static sstring generate_server_name();
public:
    http_server_control() : _server_dist(new sharded<http_server>) {
    }

    future<> start(const sstring& name = generate_server_name());
    future<> stop() noexcept;
    future<> set_routes(std::function<void(routes& r)> fun);
    future<> listen(socket_address addr);
    future<> listen(socket_address addr, http_server::server_credentials_ptr credentials);
    future<> listen(socket_address addr, listen_options lo);
    future<> listen(socket_address addr, listen_options lo, http_server::server_credentials_ptr credentials);
    sharded<http_server>& server();
};
}

}

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
#include <list>

#include <seastar/core/abort_source.hh>
#include <seastar/core/shared_future.hh>
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
class listener;
class http_stats;

using namespace std::chrono_literals;

class http_stats {
    metrics::metric_groups _metric_groups;
public:
    http_stats(http_server& server, const sstring& name);
};

/// One listening socket of an http_server, and the accept loop that feeds it.
///
/// A listener is identified by this object rather than by the address it is bound to: a
/// wildcard bind shares its address with nothing and yet accepts connections whose own
/// local address is a concrete one, and the proxy protocol replaces an accepted socket's
/// local address outright, so an address identifies a listener only by accident.
struct listener_entry {
    server_socket socket;
    socket_address addr;
    bool tls;
    // The position this listener was added at. Only the deprecated do_accepts(int) and
    // accept_loop(int) still name a listener this way; nothing renumbers.
    size_t index;
    future<> accept_loop = make_ready_future<>();
    // Held by every connection accepted on this listener, so that removing it can
    // wait for them.
    gate connections_gate;
    // Raised before the socket is aborted, so that the accept loop reads the abort as
    // being asked to stop rather than as a failure. It otherwise runs until the
    // server's task gate closes, which is how stopping the whole server stops it;
    // taking one listener away leaves that gate open.
    bool stopping = false;
    // Resolved once this listener has stopped accepting and its connections have
    // drained, whether that came from its own abort_source or from stopping the
    // whole server. What serve() hands back to its caller. The flag is what keeps the
    // two paths from resolving it twice.
    shared_promise<> removed;
    bool removal_reported = false;

    listener_entry(server_socket socket_, socket_address addr_, bool tls_, size_t index_)
        : socket(std::move(socket_)), addr(addr_), tls(tls_), index(index_) {}
};

/// A bound listening socket that is not yet accepting anything.
///
/// bind() hands one back once the address is live - the kernel queues connections from
/// that moment, so a caller can tell its clients where to find it - and serve() is what
/// starts accepting. A listener that is destroyed without being served gives the address
/// up again.
class listener {
    http_server* _server = nullptr;
    listener_entry* _entry = nullptr;

    listener(http_server& server, listener_entry& entry) noexcept
        : _server(&server), _entry(&entry) {}
    friend class http_server;
public:
    listener() = default;
    listener(listener&& other) noexcept
        : _server(std::exchange(other._server, nullptr)), _entry(std::exchange(other._entry, nullptr)) {}
    listener& operator=(listener&& other) noexcept;
    ~listener();

    /// The address this listener is bound to, which for a listener that asked for port 0
    /// is the port it was given.
    socket_address address() const noexcept;

    /// Start accepting, and keep serving until \c as is aborted or the server is stopped.
    ///
    /// The returned future resolves once this listener has stopped accepting and the
    /// connections it accepted have drained: they are told to stop reading, so no further
    /// request arrives on them, and are then waited for, so a request already being
    /// answered is finished rather than cut off. The rest of the server - its other
    /// listeners, their connections, its routes, its metrics - is untouched.
    ///
    /// Aborting \c as more than once, or after the server has been stopped, is harmless;
    /// the future resolves once, either way. Serving consumes the listener.
    future<> serve(abort_source& as);
};

class connection : public boost::intrusive::list_base_hook<> {
    friend class http_server;
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
    // Whether a request is currently arriving on this connection. stop_reading() leaves
    // such a connection to finish rather than failing the read of its body.
    bool _reading_request = false;
    // The listener this connection was accepted on. Removing that listener finds its
    // connections by this rather than by address, which a wildcard bind or the proxy
    // protocol would make disagree.
    listener_entry* _listener = nullptr;
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
    /// Stop reading further requests from this connection, leaving the write side alone
    /// so that a reply already being produced still reaches the client, and leaving a
    /// request that is still arriving alone so that its body is not cut off. Used to
    /// drain a listener rather than cut it off.
    void stop_reading();
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

/// What http_server_tester::listeners() hands back: enough of the vector of sockets it
/// used to return for its callers to go on working while they move off it.
///
/// Not a container - a server's listeners are not bare sockets any more - but it answers
/// the questions that were asked of one: how many there are, the address each is bound
/// to, and adding a socket of your own. Positions are counted the way the vector's were,
/// so they mean what they used to for a caller that only ever adds listeners, which is
/// the only thing this interface was ever able to do.
class listeners_compat_view {
    http_server* _server = nullptr;
    friend class http_server;
public:
    explicit listeners_compat_view(http_server& server) noexcept : _server(&server) {}

    /// Stands in for the server_socket that used to be at this position.
    class entry {
        const listener_entry* _entry = nullptr;
        friend class listeners_compat_view;
        explicit entry(const listener_entry& e) noexcept : _entry(&e) {}
    public:
        socket_address local_address() const noexcept;
    };

    size_t size() const noexcept;
    bool empty() const noexcept;
    entry at(size_t i) const;
    entry operator[](size_t i) const;
    entry front() const;
    entry back() const;
    /// Bind a socket of the caller's into the server. As before, nothing is accepted on
    /// it until it is served.
    void push_back(server_socket&& ss);
    void emplace_back(server_socket&& ss);
};

class http_server {
    // std::list, because an accept loop holds a reference to its own entry for as long
    // as it runs, and because the deprecated index-taking interface still wants the
    // order they were added in.
    std::list<listener_entry> _listeners;
    size_t _next_listener_index = 0;
    // What listen() serves its listeners against: they live until the server stops,
    // which is when stop() fires this. The futures are those listeners' ends of it.
    abort_source _stop;
    std::vector<future<>> _listening;
    // Handed out by the deprecated http_server_tester::listeners(). A member rather than
    // a temporary because its callers bind it to a non-const reference.
    listeners_compat_view _listeners_view{*this};
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

    /// Bind \c addr, and hand back a listener that is not yet accepting.
    ///
    /// The address is live by the time this resolves, so a caller can tell its clients
    /// where to find it before anything is served. listener::serve() starts accepting and
    /// says when to stop.
    future<listener> bind(socket_address addr, listen_options lo = {}, server_credentials_ptr credentials = {});
    /// Bind a socket the caller created. \c tls says whether it is already wrapped in
    /// TLS - this class cannot tell, and an accepted connection has to know in order to
    /// report the client's identity to a handler.
    future<listener> bind(server_socket&& ss, bool tls = false);

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
    static server_socket bind_listening_socket(socket_address addr, listen_options lo, server_credentials_ptr credentials);
    listener_entry& add_listener(server_socket&& ss, bool with_tls);
    future<> serve_until_aborted(listener_entry& listener, abort_source& as);
    void drop_listener(listener_entry& listener);
    future<> remove_listener(listener_entry& listener);
    void start_accepting(listener_entry& listener, bool with_tls);
    future<> run_accept_loop(listener_entry& listener, bool tls);
    future<> do_accept_one(listener_entry& listener, bool tls);
    // Find the listener the deprecated index-taking interface is asking for.
    listener_entry* listener_at(int which);
    future<> drain_connections_of(listener_entry& listener);
    void stop_accepting(listener_entry& listener);
    void report_removed(listener_entry& listener);
    future<> do_process_connection(connected_socket conn_fd, socket_address remote_address,
            bool tls, listener_entry& listener, gate::holder listener_hold);
    boost::intrusive::list<connection> _connections;
    friend class seastar::httpd::connection;
    friend class http_server_tester;
    friend class seastar::httpd::listener;
    friend class seastar::httpd::listeners_compat_view;
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
// A hook for tests that need more of a server than its public interface offers. Nothing
// in this tree needs it any more: what its users reached in for, a server now offers
// itself.
class http_server_tester {
public:
    /// \deprecated Serving a socket of your own is listen(server_socket&&), and reading
    /// back what a server is serving is listening_addresses(). This hands out the
    /// server's own list, whose shape is not something callers should be holding on to.
    [[deprecated("Use http_server::listen(server_socket&&) to serve a socket of your own, "
                 "and http_server::listening_addresses() to read back what is served")]]
    static listeners_compat_view& listeners(http_server& server) {
        return server._listeners_view;
    }
};

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

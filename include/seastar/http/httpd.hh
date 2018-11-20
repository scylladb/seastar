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

#include <seastar/http/request_parser.hh>
#include <seastar/http/request.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/metrics_registration.hh>
#include <seastar/util/std-compat.hh>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <bitset>
#include <limits>
#include <cctype>
#include <vector>
#include <boost/intrusive/list.hpp>
#include <seastar/http/routes.hh>

namespace seastar {

namespace httpd {

class http_server;
class http_stats;
class reply;

using namespace std::chrono_literals;

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
    static constexpr size_t limit = 4096;
    using tmp_buf = temporary_buffer<char>;
    http_request_parser _parser;
    std::unique_ptr<request> _req;
    std::unique_ptr<reply> _resp;
    // null element marks eof
    queue<std::unique_ptr<reply>> _replies { 10 };
    bool _done = false;
public:
    connection(http_server& server, connected_socket&& fd,
            socket_address addr)
            : _server(server), _fd(std::move(fd)), _read_buf(_fd.input()), _write_buf(
                    _fd.output()) {
        on_new_connection();
    }
    ~connection();
    void on_new_connection();

    future<> process() {
        // Launch read and write "threads" simultaneously:
        return when_all(read(), respond()).then(
                [] (std::tuple<future<>, future<>> joined) {
            // FIXME: notify any exceptions in joined?
            std::get<0>(joined).ignore_ready_future();
            std::get<1>(joined).ignore_ready_future();
            return make_ready_future<>();
        });
    }
    void shutdown() {
        _fd.shutdown_input();
        _fd.shutdown_output();
    }
    future<> read();
    future<> read_one();
    future<> respond();
    future<> do_response_loop();

    void set_headers(reply& resp);

    future<> start_response();
    future<> write_reply_headers(
            std::unordered_map<sstring, sstring>::iterator hi) {
        if (hi == _resp->_headers.end()) {
            return make_ready_future<>();
        }
        return _write_buf.write(hi->first.begin(), hi->first.size()).then(
                [this] {
                    return _write_buf.write(": ", 2);
                }).then([hi, this] {
            return _write_buf.write(hi->second.begin(), hi->second.size());
        }).then([this] {
            return _write_buf.write("\r\n", 2);
        }).then([hi, this] () mutable {
            return write_reply_headers(++hi);
        });
    }

    static short hex_to_byte(char c) {
        if (c >='a' && c <= 'z') {
            return c - 'a' + 10;
        } else if (c >='A' && c <= 'Z') {
            return c - 'A' + 10;
        }
        return c - '0';
    }

    /**
     * Convert a hex encoded 2 bytes substring to char
     */
    static char hexstr_to_char(const compat::string_view& in, size_t from) {

        return static_cast<char>(hex_to_byte(in[from]) * 16 + hex_to_byte(in[from + 1]));
    }

    /**
     * URL_decode a substring and place it in the given out sstring
     */
    static bool url_decode(const compat::string_view& in, sstring& out) {
        size_t pos = 0;
        char buff[in.length()];
        for (size_t i = 0; i < in.length(); ++i) {
            if (in[i] == '%') {
                if (i + 3 <= in.size()) {
                    buff[pos++] = hexstr_to_char(in, i + 1);
                    i += 2;
                } else {
                    return false;
                }
            } else if (in[i] == '+') {
                buff[pos++] = ' ';
            } else {
                buff[pos++] = in[i];
            }
        }
        out = sstring(buff, pos);
        return true;
    }

    /**
     * Add a single query parameter to the parameter list
     */
    static void add_param(request& req, const compat::string_view& param) {
        size_t split = param.find('=');

        if (split >= param.length() - 1) {
            sstring key;
            if (url_decode(param.substr(0,split) , key)) {
                req.query_parameters[key] = "";
            }
        } else {
            sstring key;
            sstring value;
            if (url_decode(param.substr(0,split), key)
                    && url_decode(param.substr(split + 1), value)) {
                req.query_parameters[key] = value;
            }
        }

    }

    /**
     * Set the query parameters in the request objects.
     * query param appear after the question mark and are separated
     * by the ampersand sign
     */
    static sstring set_query_param(request& req) {
        size_t pos = req._url.find('?');
        if (pos == sstring::npos) {
            return req._url;
        }
        size_t curr = pos + 1;
        size_t end_param;
        compat::string_view url = req._url;
        while ((end_param = req._url.find('&', curr)) != sstring::npos) {
            add_param(req, url.substr(curr, end_param - curr) );
            curr = end_param + 1;
        }
        add_param(req, url.substr(curr));
        return req._url.substr(0, pos);
    }

    future<bool> generate_reply(std::unique_ptr<request> req);

    future<> write_body();

    output_stream<char>& out() {
        return _write_buf;
    }
};

class http_server_tester;

class http_server {
    std::vector<server_socket> _listeners;
    http_stats _stats;
    uint64_t _total_connections = 0;
    uint64_t _current_connections = 0;
    uint64_t _requests_served = 0;
    uint64_t _connections_being_accepted = 0;
    uint64_t _read_errors = 0;
    uint64_t _respond_errors = 0;
    sstring _date = http_date();
    timer<> _date_format_timer { [this] {_date = http_date();} };
    bool _stopping = false;
    promise<> _all_connections_stopped;
    future<> _stopped = _all_connections_stopped.get_future();
private:
    void maybe_idle() {
        if (_stopping && !_connections_being_accepted && !_current_connections) {
            _all_connections_stopped.set_value();
        }
    }
public:
    routes _routes;
    using connection = seastar::httpd::connection;
    explicit http_server(const sstring& name) : _stats(*this, name) {
        _date_format_timer.arm_periodic(1s);
    }
    future<> listen(ipv4_addr addr) {
        listen_options lo;
        lo.reuse_address = true;
        _listeners.push_back(engine().listen(make_ipv4_address(addr), lo));
        _stopped = when_all(std::move(_stopped), do_accepts(_listeners.size() - 1)).discard_result();
        return make_ready_future<>();
    }
    future<> stop() {
        _stopping = true;
        for (auto&& l : _listeners) {
            l.abort_accept();
        }
        for (auto&& c : _connections) {
            c.shutdown();
        }
        maybe_idle();
        return std::move(_stopped);
    }

    future<> do_accepts(int which) {
        ++_connections_being_accepted;
        return _listeners[which].accept().then_wrapped(
                [this, which] (future<connected_socket, socket_address> f_cs_sa) mutable {
            --_connections_being_accepted;
            if (_stopping || f_cs_sa.failed()) {
                f_cs_sa.ignore_ready_future();
                maybe_idle();
                return;
            }
            auto cs_sa = f_cs_sa.get();
            auto conn = new connection(*this, std::get<0>(std::move(cs_sa)), std::get<1>(std::move(cs_sa)));
            conn->process().then_wrapped([conn] (auto&& f) {
                delete conn;
                try {
                    f.get();
                } catch (std::exception& ex) {
                    std::cerr << "request error " << ex.what() << std::endl;
                }
            });
            do_accepts(which);
        }).then_wrapped([] (auto f) {
            try {
                f.get();
            } catch (std::exception& ex) {
                std::cerr << "accept failed: " << ex.what() << std::endl;
            }
        });
    }

    uint64_t total_connections() const {
        return _total_connections;
    }
    uint64_t current_connections() const {
        return _current_connections;
    }
    uint64_t requests_served() const {
        return _requests_served;
    }
    uint64_t read_errors() const {
        return _read_errors;
    }
    uint64_t reply_errors() const {
        return _respond_errors;
    }
    static sstring http_date() {
        auto t = ::time(nullptr);
        struct tm tm;
        gmtime_r(&t, &tm);
        char tmp[100];
        strftime(tmp, sizeof(tmp), "%d %b %Y %H:%M:%S GMT", &tm);
        return tmp;
    }
private:
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


    future<> start(const sstring& name = generate_server_name()) {
        return _server_dist->start(name);
    }

    future<> stop() {
        return _server_dist->stop();
    }

    future<> set_routes(std::function<void(routes& r)> fun) {
        return _server_dist->invoke_on_all([fun](http_server& server) {
            fun(server._routes);
        });
    }

    future<> listen(ipv4_addr addr) {
        return _server_dist->invoke_on_all(&http_server::listen, addr);
    }

    distributed<http_server>& server() {
        return *_server_dist;
    }
};

}

}

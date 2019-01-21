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

#include <seastar/core/reactor.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/future-util.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/print.hh>
#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <queue>
#include <bitset>
#include <limits>
#include <cctype>
#include <vector>
#include <seastar/http/httpd.hh>
#include <seastar/http/reply.hh>

using namespace std::chrono_literals;

namespace seastar {

namespace httpd {
http_stats::http_stats(http_server& server, const sstring& name)
 {
    namespace sm = seastar::metrics;
    std::vector<sm::label_instance> labels;

    labels.push_back(sm::label_instance("service", name));
    _metric_groups.add_group("httpd", {
            sm::make_derive("connections_total", [&server] { return server.total_connections(); }, sm::description("The total number of connections opened"), labels),
            sm::make_gauge("connections_current", [&server] { return server.current_connections(); }, sm::description("The current number of open  connections"), labels),
            sm::make_derive("read_errors", [&server] { return server.read_errors(); }, sm::description("The total number of errors while reading http requests"), labels),
            sm::make_derive("reply_errors", [&server] { return server.reply_errors(); }, sm::description("The total number of errors while replying to http"), labels),
            sm::make_derive("requests_served", [&server] { return server.requests_served(); }, sm::description("The total number of http requests served"), labels)
    });
}

sstring http_server_control::generate_server_name() {
    static thread_local uint16_t idgen;
    return seastar::format("http-{}", idgen++);
}

future<> connection::do_response_loop() {
    return _replies.pop_eventually().then(
        [this] (std::unique_ptr<reply> resp) {
            if (!resp) {
                // eof
                return make_ready_future<>();
            }
            _resp = std::move(resp);
            return start_response().then([this] {
                        return do_response_loop();
                    });
        });
}

future<> connection::start_response() {
    if (_resp->_body_writer) {
        return _resp->write_reply_to_connection(*this).then_wrapped([this] (auto f) {
            if (f.failed()) {
                // In case of an error during the write close the connection
                _server._respond_errors++;
                _done = true;
                _replies.abort(std::make_exception_ptr(std::logic_error("Unknown exception during body creation")));
                _replies.push(std::unique_ptr<reply>());
                f.ignore_ready_future();
                return make_ready_future<>();
            }
            return _write_buf.write("0\r\n\r\n", 5);
        }).then_wrapped([this ] (auto f) {
            if (f.failed()) {
                // We could not write the closing sequence
                // Something is probably wrong with the connection,
                // we should close it, so the client will disconnect
                _done = true;
                _replies.abort(std::make_exception_ptr(std::logic_error("Unknown exception during body creation")));
                _replies.push(std::unique_ptr<reply>());
                f.ignore_ready_future();
                return make_ready_future<>();
            } else {
                return _write_buf.flush();
            }
        }).then_wrapped([this] (auto f) {
            if (f.failed()) {
                // flush failed. just close the connection
                _done = true;
                _replies.abort(std::make_exception_ptr(std::logic_error("Unknown exception during body creation")));
                _replies.push(std::unique_ptr<reply>());
                f.ignore_ready_future();
            }
            _resp.reset();
            return make_ready_future<>();
        });
    }
    set_headers(*_resp);
    _resp->_headers["Content-Length"] = to_sstring(
            _resp->_content.size());
    return _write_buf.write(_resp->_response_line.begin(),
            _resp->_response_line.size()).then([this] {
        return _resp->write_reply_headers(*this);
    }).then([this] {
        return _write_buf.write("\r\n", 2);
    }).then([this] {
        return write_body();
    }).then([this] {
        return _write_buf.flush();
    }).then([this] {
        _resp.reset();
    });
}

connection::~connection() {
    --_server._current_connections;
    _server._connections.erase(_server._connections.iterator_to(*this));
    _server.maybe_idle();
}

void connection::on_new_connection() {
    ++_server._total_connections;
    ++_server._current_connections;
    _server._connections.push_back(*this);
}

future<> connection::read() {
    return do_until([this] {return _done;}, [this] {
        return read_one();
    }).then_wrapped([this] (future<> f) {
        // swallow error
        if (f.failed()) {
            _server._read_errors++;
        }
        f.ignore_ready_future();
        return _replies.push_eventually( {});
    }).finally([this] {
        return _read_buf.close();
    });
}
future<> connection::read_one() {
    _parser.init();
    return _read_buf.consume(_parser).then([this] () mutable {
        if (_parser.eof()) {
            _done = true;
            return make_ready_future<>();
        }
        ++_server._requests_served;
        std::unique_ptr<httpd::request> req = _parser.get_parsed_request();

        return _replies.not_full().then([req = std::move(req), this] () mutable {
            return generate_reply(std::move(req));
        }).then([this](bool done) {
            _done = done;
        });
    });
}

future<> connection::respond() {
    return do_response_loop().then_wrapped([this] (future<> f) {
        // swallow error
        if (f.failed()) {
            _server._respond_errors++;
        }
        f.ignore_ready_future();
        return _write_buf.close();
    });
}

future<> connection::write_body() {
    return _write_buf.write(_resp->_content.begin(),
            _resp->_content.size());
}

void connection::set_headers(reply& resp) {
    resp._headers["Server"] = "Seastar httpd";
    resp._headers["Date"] = _server._date;
}

future<bool> connection::generate_reply(std::unique_ptr<request> req) {
    auto resp = std::make_unique<reply>();
    bool conn_keep_alive = false;
    bool conn_close = false;
    auto it = req->_headers.find("Connection");
    if (it != req->_headers.end()) {
        if (it->second == "Keep-Alive") {
            conn_keep_alive = true;
        } else if (it->second == "Close") {
            conn_close = true;
        }
    }
    bool should_close;
    // TODO: Handle HTTP/2.0 when it releases
    resp->set_version(req->_version);

    if (req->_version == "1.0") {
        if (conn_keep_alive) {
            resp->_headers["Connection"] = "Keep-Alive";
        }
        should_close = !conn_keep_alive;
    } else if (req->_version == "1.1") {
        should_close = conn_close;
    } else {
        // HTTP/0.9 goes here
        should_close = true;
    }
    sstring url = set_query_param(*req.get());
    sstring version = req->_version;
    set_headers(*resp);
    resp->set_version(version);
    return _server._routes.handle(url, std::move(req), std::move(resp)).
    // Caller guarantees enough room
    then([this, should_close, version = std::move(version)](std::unique_ptr<reply> rep) {
        rep->set_version(version).done();
        this->_replies.push(std::move(rep));
        return make_ready_future<bool>(should_close);
    });
}
}

}

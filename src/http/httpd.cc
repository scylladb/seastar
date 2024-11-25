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

#ifdef SEASTAR_MODULE
module;
#endif

#include <memory>
#include <algorithm>
#include <bitset>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <queue>
#include <unordered_map>
#include <vector>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/sstring.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/circular_buffer.hh>
#include <seastar/core/distributed.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/metrics.hh>
#include <seastar/core/print.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/internal/content_source.hh>
#include <seastar/http/reply.hh>
#include <seastar/util/short_streams.hh>
#include <seastar/util/log.hh>
#include <seastar/util/string_utils.hh>
#endif


using namespace std::chrono_literals;

namespace seastar {

logger hlogger("httpd");

namespace httpd {
http_stats::http_stats(http_server& server, const sstring& name)
 {
    namespace sm = seastar::metrics;
    std::vector<sm::label_instance> labels;

    labels.push_back(sm::label_instance("service", name));
    _metric_groups.add_group("httpd", {
            sm::make_counter("connections_total", [&server] { return server.total_connections(); }, sm::description("The total number of connections opened"), labels),
            sm::make_gauge("connections_current", [&server] { return server.current_connections(); }, sm::description("The current number of open  connections"), labels),
            sm::make_counter("read_errors", [&server] { return server.read_errors(); }, sm::description("The total number of errors while reading http requests"), labels),
            sm::make_counter("reply_errors", [&server] { return server.reply_errors(); }, sm::description("The total number of errors while replying to http"), labels),
            sm::make_counter("requests_served", [&server] { return server.requests_served(); }, sm::description("The total number of http requests served"), labels)
    });
}

sstring http_server_control::generate_server_name() {
    static thread_local uint16_t idgen;
    return seastar::format("http-{}", idgen++);
}

future<> connection::do_response_loop() {
    return _replies.pop_eventually().then(
        [this] (std::unique_ptr<http::reply> resp) {
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
                _replies.push(std::unique_ptr<http::reply>());
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
                _replies.push(std::unique_ptr<http::reply>());
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
                _replies.push(std::unique_ptr<http::reply>());
                f.ignore_ready_future();
            }
            _resp.reset();
            return make_ready_future<>();
        });
    }
    set_headers(*_resp);
    _resp->_headers["Content-Length"] = to_sstring(
            _resp->_content.size());
    return _write_buf.write(_resp->_response_line.data(),
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
}

void connection::on_new_connection() {
    ++_server._total_connections;
    ++_server._current_connections;
    _fd.set_nodelay(true);
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
    });
}

static input_stream<char> make_content_stream(http::request* req, input_stream<char>& buf) {
    // Create an input stream based on the requests body encoding or lack thereof
    if (seastar::internal::case_insensitive_cmp()(req->get_header("Transfer-Encoding"), "chunked")) {
        return input_stream<char>(data_source(std::make_unique<internal::chunked_source_impl>(buf, req->chunk_extensions, req->trailing_headers)));
    } else {
        return input_stream<char>(data_source(std::make_unique<internal::content_length_source_impl>(buf, req->content_length)));
    }
}

static future<std::unique_ptr<http::request>>
set_request_content(std::unique_ptr<http::request> req, input_stream<char>* content_stream, bool streaming) {
    req->content_stream = content_stream;

    if (streaming) {
        return make_ready_future<std::unique_ptr<http::request>>(std::move(req));
    } else {
        // Read the entire content into the request content string
        return util::read_entire_stream_contiguous(*content_stream).then([req = std::move(req)] (sstring content) mutable {
            req->content = std::move(content);
            return make_ready_future<std::unique_ptr<http::request>>(std::move(req));
        });
    }
}

void connection::generate_error_reply_and_close(std::unique_ptr<http::request> req, http::reply::status_type status, const sstring& msg) {
    auto resp = std::make_unique<http::reply>();
    // TODO: Handle HTTP/2.0 when it releases
    resp->set_version(req->_version);
    resp->set_status(status, msg);
    resp->done();
    _done = true;
    _replies.push(std::move(resp));
}

future<> connection::read_one() {
    _parser.init();
    return _read_buf.consume(_parser).then([this] () mutable {
        if (_parser.eof()) {
            _done = true;
            return make_ready_future<>();
        }
        ++_server._requests_served;
        std::unique_ptr<http::request> req = _parser.get_parsed_request();

        req->_server_address = this->_server_addr;
        req->_client_address = this->_client_addr;

        if (_tls) {
            req->protocol_name = "https";
        }
        if (_parser.failed()) {
            if (req->_version.empty()) {
                // we might have failed to parse even the version
                req->_version = "1.1";
            }
            generate_error_reply_and_close(std::move(req), http::reply::status_type::bad_request, "Can't parse the request");
            return make_ready_future<>();
        }

        size_t content_length_limit = _server.get_content_length_limit();
        sstring length_header = req->get_header("Content-Length");
        req->content_length = strtol(length_header.c_str(), nullptr, 10);

        if (req->content_length > content_length_limit) {
            auto msg = format("Content length limit ({}) exceeded: {}", content_length_limit, req->content_length);
            generate_error_reply_and_close(std::move(req), http::reply::status_type::payload_too_large, std::move(msg));
            return make_ready_future<>();
        }

        sstring encoding = req->get_header("Transfer-Encoding");
        if (encoding.size() && !seastar::internal::case_insensitive_cmp()(encoding, "chunked")){
            //TODO: add "identity", "gzip"("x-gzip"), "compress"("x-compress"), and "deflate" encodings and their combinations
            generate_error_reply_and_close(std::move(req), http::reply::status_type::not_implemented, format("Encodings other than \"chunked\" are not implemented (received encoding: \"{}\")", encoding));
            return make_ready_future<>();
        }

        auto maybe_reply_continue = [this, req = std::move(req)] () mutable {
            if (req->_version == "1.1" && seastar::internal::case_insensitive_cmp()(req->get_header("Expect"), "100-continue")){
                return _replies.not_full().then([req = std::move(req), this] () mutable {
                    auto continue_reply = std::make_unique<http::reply>();
                    set_headers(*continue_reply);
                    continue_reply->set_version(req->_version);
                    continue_reply->set_status(http::reply::status_type::continue_).done();
                    this->_replies.push(std::move(continue_reply));
                    return make_ready_future<std::unique_ptr<http::request>>(std::move(req));
                });
            } else {
                return make_ready_future<std::unique_ptr<http::request>>(std::move(req));
            }
        };

        return maybe_reply_continue().then([this] (std::unique_ptr<http::request> req) {
            return do_with(make_content_stream(req.get(), _read_buf), sstring(req->_version), std::move(req), [this] (input_stream<char>& content_stream, sstring& version, std::unique_ptr<http::request>& req) {
                return set_request_content(std::move(req), &content_stream, _server.get_content_streaming()).then([this, &content_stream] (std::unique_ptr<http::request> req) {
                    return _replies.not_full().then([this, req = std::move(req)] () mutable {
                        return generate_reply(std::move(req));
                    }).then([this, &content_stream](bool done) {
                        _done = done;
                        // If the handler did not read the entire request
                        // content, this connection cannot be reused so we
                        // need to close it (via "_done = true"). But we can't
                        // just check content_stream.eof(): It may only become
                        // true after read(). Issue #907.
                        return content_stream.read().then([this] (temporary_buffer<char> buf) {
                            if (!buf.empty()) {
                                _done = true;
                            }
                        });
                    });
                }).handle_exception_type([this, &version] (const base_exception& e) mutable {
                    // If the request had a "Transfer-Encoding: chunked" header and content streaming wasn't enabled, we might have failed
                    // before passing the request to handler - when we were parsing chunks
                    auto err_req = std::make_unique<http::request>();
                    err_req->_version = version;
                    generate_error_reply_and_close(std::move(err_req), e.status(), e.str());
                });
            });
        });
    });
}

future<> connection::process() {
    // Launch read and write "threads" simultaneously:
    return when_all(read(), respond()).then(
            [] (std::tuple<future<>, future<>> joined) {
        try {
            std::get<0>(joined).get();
        } catch (...) {
            hlogger.debug("Read exception encountered: {}", std::current_exception());
        }
        try {
            std::get<1>(joined).get();
        } catch (...) {
            hlogger.debug("Response exception encountered: {}", std::current_exception());
        }
        return make_ready_future<>();
    }).finally([this]{
        return _read_buf.close().handle_exception([](std::exception_ptr e) {
            hlogger.debug("Close exception encountered: {}", e);
        });
    });
}
void connection::shutdown() {
    _fd.shutdown_input();
    _fd.shutdown_output();
}

output_stream<char>& connection::out() {
    return _write_buf;
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
    return _write_buf.write(_resp->_content.data(),
            _resp->_content.size());
}

void connection::set_headers(http::reply& resp) {
    resp._headers["Server"] = "Seastar httpd";
    resp._headers["Date"] = _server._date;
}

future<bool> connection::generate_reply(std::unique_ptr<http::request> req) {
    auto resp = std::make_unique<http::reply>();
    resp->set_version(req->_version);
    set_headers(*resp);
    bool keep_alive = req->should_keep_alive();
    if (keep_alive && req->_version == "1.0") {
        resp->_headers["Connection"] = "Keep-Alive";
    }

    sstring url = req->parse_query_param();
    sstring version = req->_version;
    return _server._routes.handle(url, std::move(req), std::move(resp)).
    // Caller guarantees enough room
    then([this, keep_alive , version = std::move(version)](std::unique_ptr<http::reply> rep) {
        rep->set_version(version).done();
        this->_replies.push(std::move(rep));
        return make_ready_future<bool>(!keep_alive);
    });
}

void http_server::set_tls_credentials(server_credentials_ptr credentials) {
    _credentials = credentials;
}

size_t http_server::get_content_length_limit() const {
    return _content_length_limit;
}

void http_server::set_content_length_limit(size_t limit) {
    _content_length_limit = limit;
}

bool http_server::get_content_streaming() const {
    return _content_streaming;
}

void http_server::set_content_streaming(bool b) {
    _content_streaming = b;
}

future<> http_server::listen(socket_address addr, listen_options lo,
            server_credentials_ptr listener_credentials) {
    if (listener_credentials) {
        _listeners.push_back(seastar::tls::listen(listener_credentials, addr, lo));
    } else {
        _listeners.push_back(seastar::listen(addr, lo));
    }
    return do_accepts(_listeners.size() - 1, listener_credentials != nullptr);
}

future<> http_server::listen(socket_address addr, listen_options lo) {
    return listen(addr, lo, _credentials);
}

future<> http_server::listen(socket_address addr,
            server_credentials_ptr listener_credentials) {
    listen_options lo;
    lo.reuse_address = true;
    return listen(addr, lo, listener_credentials);
}

future<> http_server::listen(socket_address addr) {
    listen_options lo;
    lo.reuse_address = true;
    return listen(addr, lo);
}
future<> http_server::stop() {
    future<> tasks_done = _task_gate.close();
    for (auto&& l : _listeners) {
        l.abort_accept();
    }
    for (auto&& c : _connections) {
        c.shutdown();
    }
    return tasks_done;
}

// FIXME: This could return void
future<> http_server::do_accepts(int which, bool tls) {
    (void)try_with_gate(_task_gate, [this, which, tls] {
        return keep_doing([this, which, tls] {
            return try_with_gate(_task_gate, [this, which, tls] {
                return do_accept_one(which, tls);
            });
        }).handle_exception_type([](const gate_closed_exception& e) {});
    }).handle_exception_type([](const gate_closed_exception& e) {});
    return make_ready_future<>();
}

future<> http_server::do_accepts(int which){
    return do_accepts(which, _credentials != nullptr);
}

future<> http_server::do_accept_one(int which, bool tls) {
    return _listeners[which].accept().then([this, tls] (accept_result ar) mutable {
        auto local_address = ar.connection.local_address();
        auto conn = std::make_unique<connection>(*this, std::move(ar.connection),
                std::move(ar.remote_address), std::move(local_address), tls);
        (void)try_with_gate(_task_gate, [conn = std::move(conn)]() mutable {
            return conn->process().handle_exception([conn = std::move(conn)] (std::exception_ptr ex) {
                hlogger.error("request error: {}", ex);
            });
        }).handle_exception_type([] (const gate_closed_exception& e) {});
    }).handle_exception_type([] (const std::system_error &e) {
        // We expect a ECONNABORTED when http_server::stop is called,
        // no point in warning about that.
        if (e.code().value() != ECONNABORTED) {
            hlogger.error("accept failed: {}", e);
        }
    }).handle_exception([] (std::exception_ptr ex) {
        hlogger.error("accept failed: {}", ex);
    });
}

uint64_t http_server::total_connections() const {
    return _total_connections;
}
uint64_t http_server::current_connections() const {
    return _current_connections;
}
uint64_t http_server::requests_served() const {
    return _requests_served;
}
uint64_t http_server::read_errors() const {
    return _read_errors;
}
uint64_t http_server::reply_errors() const {
    return _respond_errors;
}

// Write the current date in the specific "preferred format" defined in
// RFC 7231, Section 7.1.1.1, a.k.a. IMF (Internet Message Format) fixdate.
// For example: Sun, 06 Nov 1994 08:49:37 GMT
sstring http_server::http_date() {
    auto t = ::time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    // Using strftime() would have been easier, but unfortunately relies on
    // the current locale, and we need the month and day names in English.
    static const char* days[] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    static const char* months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    return seastar::format("{}, {:02d} {} {} {:02d}:{:02d}:{:02d} GMT",
        days[tm.tm_wday], tm.tm_mday, months[tm.tm_mon], 1900 + tm.tm_year,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
}


future<> http_server_control::start(const sstring& name) {
    return _server_dist->start(name);
}

future<> http_server_control::stop() noexcept {
    return _server_dist->stop();
}

future<> http_server_control::set_routes(std::function<void(routes& r)> fun) {
    return _server_dist->invoke_on_all([fun](http_server& server) {
        fun(server._routes);
    });
}

future<> http_server_control::listen(socket_address addr) {
    return _server_dist->invoke_on_all<future<> (http_server::*)(socket_address)>(&http_server::listen, addr);
}

future<> http_server_control::listen(socket_address addr, http_server::server_credentials_ptr credentials) {
    return _server_dist->invoke_on_all<future<> (http_server::*)(socket_address, http_server::server_credentials_ptr)>(&http_server::listen, addr, credentials);
}

future<> http_server_control::listen(socket_address addr, listen_options lo) {
    return _server_dist->invoke_on_all<future<> (http_server::*)(socket_address, listen_options)>(&http_server::listen, addr, lo);
}

future<> http_server_control::listen(socket_address addr, listen_options lo, http_server::server_credentials_ptr credentials) {
    return _server_dist->invoke_on_all<future<> (http_server::*)(socket_address, listen_options, http_server::server_credentials_ptr)>(&http_server::listen, addr, lo, credentials);
}

distributed<http_server>& http_server_control::server() {
    return *_server_dist;
}

}

}

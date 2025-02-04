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
 * Copyright (C) 2022 Scylladb, Ltd.
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <concepts>
#include <gnutls/gnutls.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include <seastar/core/loop.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/reactor.hh>
#include <seastar/http/client.hh>
#include <seastar/http/request.hh>
#include <seastar/http/reply.hh>
#include <seastar/http/response_parser.hh>
#include <seastar/http/internal/content_source.hh>
#include <seastar/util/short_streams.hh>
#include <seastar/util/string_utils.hh>
#endif

namespace seastar {
logger http_log("http");
namespace http {
namespace internal {

client_ref::client_ref(http::experimental::client* c) noexcept : _c(c) {
    _c->_nr_connections++;
}

client_ref::~client_ref() {
    if (_c != nullptr) {
        _c->_nr_connections--;
        _c->_wait_con.broadcast();
    }
}

}

namespace experimental {

connection::connection(connected_socket&& fd, internal::client_ref cr)
        : _fd(std::move(fd))
        , _read_buf(_fd.input())
        , _write_buf(_fd.output())
        , _closed(_fd.wait_input_shutdown().finally([me = shared_from_this()] {}))
        , _ref(std::move(cr))
{
}

future<> connection::write_body(const request& req) {
    if (req.body_writer) {
        if (req.content_length != 0) {
            return req.body_writer(internal::make_http_content_length_output_stream(_write_buf, req.content_length, req._bytes_written)).then([&req] {
                if (req.content_length == req._bytes_written) {
                    return make_ready_future<>();
                } else {
                    return make_exception_future<>(std::runtime_error(format("partial request body write, need {} sent {}", req.content_length, req._bytes_written)));
                }
            });
        }
        return req.body_writer(internal::make_http_chunked_output_stream(_write_buf)).then([this] {
            return _write_buf.write("0\r\n\r\n");
        });
    } else if (!req.content.empty()) {
        return _write_buf.write(req.content);
    } else {
        return make_ready_future<>();
    }
}

future<connection::reply_ptr> connection::maybe_wait_for_continue(const request& req) {
    if (req.get_header("Expect") == "") {
        return make_ready_future<reply_ptr>(nullptr);
    }

    return _write_buf.flush().then([this] {
        return recv_reply().then([] (reply_ptr rep) {
            if (rep->_status == reply::status_type::continue_) {
                return make_ready_future<reply_ptr>(nullptr);
            } else {
                return make_ready_future<reply_ptr>(std::move(rep));
            }
        });
    });
}

void connection::setup_request(request& req) {
    if (req._version.empty()) {
        req._version = "1.1";
    }
    if (req.content_length != 0) {
        if (!req.body_writer && req.content.empty()) {
            throw std::runtime_error("Request body writer not set and content is empty");
        }
        req._headers["Content-Length"] = to_sstring(req.content_length);
    }
}

future<> connection::send_request_head(const request& req) {
    return _write_buf.write(req.request_line()).then([this, &req] {
        return req.write_request_headers(_write_buf).then([this] {
            return _write_buf.write("\r\n", 2);
        });
    });
}

future<connection::reply_ptr> connection::recv_reply() {
    http_response_parser parser;
    return do_with(std::move(parser), [this] (auto& parser) {
        parser.init();
        return _read_buf.consume(parser).then([this, &parser] {
            if (parser.eof()) {
                http_log.trace("Parsing response EOFed");
                throw std::system_error(ECONNABORTED, std::system_category());
            }
            if (parser.failed()) {
                http_log.trace("Parsing response failed");
                throw std::runtime_error("Invalid http server response");
            }

            auto resp = parser.get_parsed_response();
            sstring length_header = resp->get_header("Content-Length");
            resp->content_length = strtol(length_header.c_str(), nullptr, 10);
            if ((resp->_version != "1.1") || seastar::internal::case_insensitive_cmp()(resp->get_header("Connection"), "close")) {
                _persistent = false;
            }
            return make_ready_future<reply_ptr>(std::move(resp));
        });
    });
}

future<connection::reply_ptr> connection::do_make_request(request& req) {
    setup_request(req);
    return send_request_head(req).then([this, &req] {
        return maybe_wait_for_continue(req).then([this, &req] (reply_ptr cont) {
            if (cont) {
                return make_ready_future<reply_ptr>(std::move(cont));
            }

            return write_body(req).then([this] {
                return _write_buf.flush().then([this] {
                    return recv_reply();
                });
            });
        });
    });
}

future<reply> connection::make_request(request req) {
    return do_with(std::move(req), [this] (auto& req) {
        return do_make_request(req).then([] (reply_ptr rep) {
            return make_ready_future<reply>(std::move(*rep));
        });
    });
}

input_stream<char> connection::in(reply& rep) {
    if (seastar::internal::case_insensitive_cmp()(rep.get_header("Transfer-Encoding"), "chunked")) {
        return input_stream<char>(data_source(std::make_unique<httpd::internal::chunked_source_impl>(_read_buf, rep.chunk_extensions, rep.trailing_headers)));
    }

    return input_stream<char>(data_source(std::make_unique<httpd::internal::content_length_source_impl>(_read_buf, rep.content_length)));
}

void connection::shutdown() noexcept {
    _persistent = false;
    _fd.shutdown_input();
}

future<> connection::close() {
    return when_all(_read_buf.close(), _write_buf.close()).discard_result().then([this] {
        auto la = _fd.local_address();
        return std::move(_closed).then([la = std::move(la)] {
            http_log.trace("destroyed connection {}", la);
        });
    });
}

client::client(socket_address addr)
        : client(std::make_unique<basic_connection_factory>(std::move(addr)))
{
}

client::client(socket_address addr, shared_ptr<tls::certificate_credentials> creds, sstring host)
        : client(std::make_unique<tls_connection_factory>(std::move(addr), std::move(creds), std::move(host)))
{
}

client::client(std::unique_ptr<connection_factory> f, unsigned max_connections, retry_requests retry)
        : _new_connections(std::move(f))
        , _max_connections(max_connections)
        , _retry(retry)
{
}

future<client::connection_ptr> client::get_connection(abort_source* as) {
    if (!_pool.empty()) {
        connection_ptr con = _pool.front().shared_from_this();
        _pool.pop_front();
        http_log.trace("pop http connection {} from pool", con->_fd.local_address());
        return make_ready_future<connection_ptr>(con);
    }

    if (_nr_connections >= _max_connections) {
        auto sub = as ? as->subscribe([this] () noexcept { _wait_con.broadcast(); }) : std::nullopt;
        return _wait_con.wait().then([this, as, sub = std::move(sub)] {
            if (as != nullptr && as->abort_requested()) {
                return make_exception_future<client::connection_ptr>(as->abort_requested_exception_ptr());
            }
            return get_connection(as);
        });
    }

    return make_connection(as);
}

future<client::connection_ptr> client::make_connection(abort_source* as) {
    _total_new_connections++;
    return _new_connections->make(as).then([cr = internal::client_ref(this)] (connected_socket cs) mutable {
        http_log.trace("created new http connection {}", cs.local_address());
        auto con = seastar::make_shared<connection>(std::move(cs), std::move(cr));
        return make_ready_future<connection_ptr>(std::move(con));
    });
}

future<> client::put_connection(connection_ptr con) {
    if (con->_persistent && (_nr_connections <= _max_connections)) {
        http_log.trace("push http connection {} to pool", con->_fd.local_address());
        _pool.push_back(*con);
        _wait_con.signal();
        return make_ready_future<>();
    }

    http_log.trace("dropping connection {}", con->_fd.local_address());
    return con->close().finally([con] {});
}

future<> client::shrink_connections() {
    if (_nr_connections <= _max_connections) {
        return make_ready_future<>();
    }

    if (!_pool.empty()) {
        connection_ptr con = _pool.front().shared_from_this();
        _pool.pop_front();
        return con->close().finally([this, con] {
            return shrink_connections();
        });
    }

    return _wait_con.wait().then([this] {
        return shrink_connections();
    });
}

future<> client::set_maximum_connections(unsigned nr) {
    if (nr > _max_connections) {
        _max_connections = nr;
        _wait_con.broadcast();
        return make_ready_future<>();
    }

    _max_connections = nr;
    return shrink_connections();
}

template <std::invocable<connection&> Fn>
auto client::with_connection(Fn&& fn, abort_source* as) {
    return get_connection(as).then([this, fn = std::move(fn)] (connection_ptr con) mutable {
        return fn(*con).finally([this, con = std::move(con)] () mutable {
            return put_connection(std::move(con));
        });
    });
}

template <typename Fn>
requires std::invocable<Fn, connection&>
auto client::with_new_connection(Fn&& fn, abort_source* as) {
    return make_connection(as).then([this, fn = std::move(fn)] (connection_ptr con) mutable {
        return fn(*con).finally([this, con = std::move(con)] () mutable {
            return put_connection(std::move(con));
        });
    });
}

future<> client::make_request(request&& req, reply_handler&& handle, std::optional<reply::status_type>&& expected) {
    return do_with(std::move(req), std::move(handle), [this, expected](request& req, reply_handler& handle) mutable {
        return do_make_request(req, handle, nullptr, std::move(expected));
    });
}

future<> client::make_request(request&& req, reply_handler&& handle, abort_source& as, std::optional<reply::status_type>&& expected) {
    return do_with(std::move(req), std::move(handle), [this, as{&as}, expected](request& req, reply_handler& handle) mutable {
        return do_make_request(req, handle, as, std::move(expected));
    });
}

future<> client::make_request(request& req, reply_handler& handle, std::optional<reply::status_type> expected) {
    return do_make_request(req, handle, nullptr, std::move(expected));
}

future<> client::make_request(request& req, reply_handler& handle, abort_source& as, std::optional<reply::status_type> expected) {
    return do_make_request(req, handle, &as, std::move(expected));
}

future<> client::do_make_request(request& req, reply_handler& handle, abort_source* as, std::optional<reply::status_type> expected) {
    return with_connection([this, &req, &handle, as, expected] (connection& con) {
        return do_make_request(con, req, handle, as, expected);
    }, as).handle_exception_type([this, &req, &handle, as, expected] (const std::system_error& ex) {
        if (as && as->abort_requested()) {
            return make_exception_future<>(as->abort_requested_exception_ptr());
        }

        if (!_retry) {
            return make_exception_future<>(ex);
        }

        bool should_retry = false;
        const std::exception* current_exception = &ex;
        while (current_exception) {
            if (auto sys_error = dynamic_cast<const std::system_error*>(current_exception)) {
                auto code = sys_error->code().value();
                if (code == EPIPE || code == ECONNABORTED || code == GNUTLS_E_PREMATURE_TERMINATION) {
                    should_retry = true;
                    break;
                }
            }
            try {
                std::rethrow_if_nested(*current_exception);
            } catch (const std::exception& nested) {
                current_exception = &nested;
                continue;
            } catch (...) {
                break;
            }
            current_exception = nullptr;
        }
        if (!should_retry) {
            return make_exception_future<>(ex);
        }

        // The 'con' connection may not yet be freed, so the total connection
        // count still account for it and with_new_connection() may temporarily
        // break the limit. That's OK, the 'con' will be closed really soon
        return with_new_connection([this, &req, &handle, as, expected] (connection& con) {
            return do_make_request(con, req, handle, as, expected);
        }, as);
    });
}

future<> client::do_make_request(connection& con, request& req, reply_handler& handle, abort_source* as, std::optional<reply::status_type> expected) {
    auto sub = as ? as->subscribe([&con] () noexcept { con.shutdown(); }) : std::nullopt;
    return con.do_make_request(req).then([&con, &handle, expected] (connection::reply_ptr reply) mutable {
        auto& rep = *reply;
        if (expected.has_value() && rep._status != expected.value()) {
            if (!http_log.is_enabled(log_level::debug)) {
                return make_exception_future<>(httpd::unexpected_status_error(rep._status));
            }

            return do_with(con.in(rep), [reply = std::move(reply)] (auto& in) mutable {
                return util::read_entire_stream_contiguous(in).then([reply = std::move(reply)] (auto message) {
                    http_log.debug("request finished with {}: {}", reply->_status, message);
                    return make_exception_future<>(httpd::unexpected_status_error(reply->_status));
                });
            });
        }

        return handle(rep, con.in(rep)).finally([reply = std::move(reply)] {});
    }).handle_exception([&con] (auto ex) mutable {
        con._persistent = false;
        return make_exception_future<>(std::move(ex));
    }).finally([sub = std::move(sub)] {});
}

future<> client::close() {
    if (_pool.empty()) {
        return make_ready_future<>();
    }

    connection_ptr con = _pool.front().shared_from_this();
    _pool.pop_front();
    http_log.trace("closing connection {}", con->_fd.local_address());
    return con->close().then([this, con] {
        return close();
    });
}

} // experimental namespace
} // http namespace
} // seastar namespace

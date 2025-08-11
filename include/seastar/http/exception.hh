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

#include <seastar/util/log.hh>
#include <seastar/util/modules.hh>
#include <seastar/http/reply.hh>
#include <seastar/json/json_elements.hh>

namespace seastar {

namespace httpd {

SEASTAR_MODULE_EXPORT_BEGIN

/**
 * The base_exception is a base for all http exception.
 * It contains a message that will be return as the message content
 * and a status that will be return as a status code.
 */
class base_exception : public std::exception {
public:
    base_exception(const std::string& msg, http::reply::status_type status)
            : _msg(msg), _status(status) {
    }

    /**
     * A base_exception with a content_type is specifying a full response body, whereas
     * a base_exception with only a _status is specifying a string that may be wrapped
     * in e.g. a json_exception.
     */
    base_exception(const std::string& msg, http::reply::status_type status, const std::string &content_type)
            : _msg(msg), _status(status), _content_type(content_type) {
    }


    virtual const char* what() const noexcept {
        return _msg.c_str();
    }

    http::reply::status_type status() const {
        return _status;
    }

    virtual const std::string& str() const {
        return _msg;
    }

    virtual const std::string& content_type() const {
        return _content_type;
    }
private:
    std::string _msg;
    http::reply::status_type _status;
    std::string _content_type;

};

/**
 * Throwing this exception will result in a redirect to the given url
 */
class redirect_exception : public base_exception {
public:
    redirect_exception(const std::string& url, http::reply::status_type status = http::reply::status_type::moved_permanently, const std::optional<int>& retry_after = std::nullopt)
            : base_exception("", status), url(url), retry_after(retry_after) {
    }

    http::reply to_reply() const {
        http::reply reply{};
        reply.add_header("Location", url);
        if (retry_after.has_value()) {
            reply.add_header("Retry-After", std::to_string(retry_after.value()));
        }
        reply.set_status(status());
        return reply;
    }

    std::string url;
    std::optional<int> retry_after;
};

/**
 * Throwing this exception will result in a 404 not found result
 */
class not_found_exception : public base_exception {
public:
    not_found_exception(const std::string& msg = "Not found")
            : base_exception(msg, http::reply::status_type::not_found) {
    }
};

/**
 * Throwing this exception will result in a 400 bad request result
 */

class bad_request_exception : public base_exception {
public:
    bad_request_exception(const std::string& msg)
            : base_exception(msg, http::reply::status_type::bad_request) {
    }
};

class bad_param_exception : public bad_request_exception {
public:
    bad_param_exception(const std::string& msg)
            : bad_request_exception(msg) {
    }
};

class missing_param_exception : public bad_request_exception {
public:
    missing_param_exception(const std::string& param)
            : bad_request_exception(
                    std::string("Missing mandatory parameter '") + param + "'") {
    }
};

class bad_chunk_exception : public bad_request_exception {
public:
    bad_chunk_exception(const std::string& msg)
            : bad_request_exception(
                    std::string("Can't read body chunk in a 'chunked' request '") + msg + "'") {
    }
};

class server_error_exception : public base_exception {
public:
    server_error_exception(const std::string& msg)
            : base_exception(msg, http::reply::status_type::internal_server_error) {
    }
};

class response_parsing_exception : public server_error_exception {
public:
    explicit response_parsing_exception(const std::string& msg) : server_error_exception(msg) {}
};

class /* [[deprecated("Use base_exception or any of its inheritants instead")]] */json_exception : public json::json_base {
public:
    json::json_element<std::string> _msg;
    json::json_element<int> _code;
    void register_params() {
        add(&_msg, "message");
        add(&_code, "code");
    }

    json_exception(const base_exception & e) {
        set(e.str(), e.status());
    }

    json_exception(std::exception_ptr e) {
        std::ostringstream exception_description;
        exception_description << e;
        set(exception_description.str(), http::reply::status_type::internal_server_error);
    }
private:
    void set(const std::string& msg, http::reply::status_type code) {
        register_params();
        _msg = msg;
        _code = (int) code;
    }
};

/**
 * Client-side exception to report unexpected server reply status
 */
class unexpected_status_error : public base_exception {
public:
    unexpected_status_error(http::reply::status_type st)
        : base_exception(fmt::to_string(st), st)
    {}
};

SEASTAR_MODULE_EXPORT_END
}

}

SEASTAR_MODULE_EXPORT
template <>
struct fmt::formatter<seastar::httpd::base_exception> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::httpd::base_exception& e, fmt::format_context& ctx) const {
        return fmt::format_to(ctx.out(), "{} ({})", e.what(), e.status());
    }
};

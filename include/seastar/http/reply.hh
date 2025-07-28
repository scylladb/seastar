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

// This file was modified from boost http example
//
// reply.hpp
// ~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#ifndef SEASTAR_MODULE
#include <unordered_map>
#endif
#include <seastar/core/sstring.hh>
#include <seastar/http/mime_types.hh>
#include <seastar/core/iostream.hh>
#include <seastar/util/noncopyable_function.hh>
#include <seastar/util/modules.hh>
#include <seastar/util/string_utils.hh>
#include <seastar/util/iostream.hh>

namespace seastar {

SEASTAR_MODULE_EXPORT_BEGIN

namespace httpd {

class connection;
class routes;

}

namespace http {

/**
 * This type is moved to namespace level, so 
 * we can forward declare it.
 * 
 * Wrapper type for HTTP status codes, including
 * contants for the most common ones.
 * 
 * Note: this was an enum, but changed to a 
 * struct wrapper type to make it extensible.
 * 
 * This has the drawback of the type being
 * weakly aliasable to int. This is however
 * also a benefit. 
 */
struct status_type {
    int value;

    constexpr explicit status_type(int v)
        : value(v)
    {}
    constexpr operator int() const {
        return value;
    }
    std::strong_ordering operator<=>(const status_type& s) const = default;

    // Helper type to work around constexpr constants 
    // not being declarable inside their own type definition.
    // 
    // Do not use this type directly.
    struct status_init {
        int value;
        constexpr operator status_type() const {
            return status_type(value);
        }
        constexpr operator int() const {
            return value;
        }
    };

    static constexpr status_init continue_{100}; //!< continue
    static constexpr status_init switching_protocols{101}; //!< switching_protocols
    static constexpr status_init ok{200}; //!< ok
    static constexpr status_init created{201}; //!< created
    static constexpr status_init accepted{202}; //!< accepted
    static constexpr status_init nonauthoritative_information{203}; //!< nonauthoritative_information
    static constexpr status_init no_content{204}; //!< no_content
    static constexpr status_init reset_content{205}; //!< reset_content
    static constexpr status_init partial_content{206}; //! partial_content
    static constexpr status_init multiple_choices{300}; //!< multiple_choices
    static constexpr status_init moved_permanently{301}; //!< moved_permanently
    static constexpr status_init moved_temporarily{302}; //!< moved_temporarily
    static constexpr status_init see_other{303}; //!< see_other
    static constexpr status_init not_modified{304}; //!< not_modified
    static constexpr status_init use_proxy{305}; //!< use_proxy
    static constexpr status_init temporary_redirect{307}; //!< temporary_redirect
    static constexpr status_init permanent_redirect{308}; //!< permanent_redirect
    static constexpr status_init bad_request{400}; //!< bad_request
    static constexpr status_init unauthorized{401}; //!< unauthorized
    static constexpr status_init payment_required{402}; //!< payment_required
    static constexpr status_init forbidden{403}; //!< forbidden
    static constexpr status_init not_found{404}; //!< not_found
    static constexpr status_init method_not_allowed{405}; //!< method_not_allowed
    static constexpr status_init not_acceptable{406}; //!< not_acceptable
    static constexpr status_init request_timeout{408}; //!< request_timeout
    static constexpr status_init conflict{409}; //!< conflict
    static constexpr status_init gone{410}; //!< gone
    static constexpr status_init length_required{411}; //!< length_required
    static constexpr status_init payload_too_large{413}; //!< payload_too_large
    static constexpr status_init uri_too_long{414}; //!< uri_too_long
    static constexpr status_init unsupported_media_type{415}; //!< unsupported_media_type
    static constexpr status_init expectation_failed{417}; //!< expectation_failed
    static constexpr status_init page_expired{419}; //!< page_expired
    static constexpr status_init unprocessable_entity{422}; //!< unprocessable_entity
    static constexpr status_init upgrade_required{426}; //!< upgrade_required
    static constexpr status_init too_many_requests{429}; //!< too_many_requests
    static constexpr status_init login_timeout{440}; //!< login_timeout
    static constexpr status_init internal_server_error{500}; //!< internal_server_error
    static constexpr status_init not_implemented{501}; //!< not_implemented
    static constexpr status_init bad_gateway{502}; //!< bad_gateway
    static constexpr status_init service_unavailable{503}; //!< service_unavailable
    static constexpr status_init gateway_timeout{504}; //!< gateway_timeout
    static constexpr status_init http_version_not_supported{505}; //!< http_version_not_supported
    static constexpr status_init insufficient_storage{507}; //!< insufficient_storage
    static constexpr status_init bandwidth_limit_exceeded{509}; //!< bandwidth_limit_exceeded
    static constexpr status_init network_read_timeout{598}; //!< network_read_timeout
    static constexpr status_init network_connect_timeout{599}; //!< network_connect_timeout
};

/**
 * A reply to be sent to a client.
 */
struct reply {
    /**
     * The status of the reply.
     */
    using status_type = http::status_type;
    status_type _status;

    /**
     * HTTP status classes
     * See https://www.iana.org/assignments/http-status-codes/http-status-codes.xhtml
     *
     * 1xx: Informational - Request received, continuing process
     * 2xx: Success - The action was successfully received, understood, and accepted
     * 3xx: Redirection - Further action must be taken in order to complete the request
     * 4xx: Client Error - The request contains bad syntax or cannot be fulfilled
     * 5xx: Server Error - The server failed to fulfill an apparently valid request
     */
    enum class status_class : uint8_t {
        informational = 1,
        success = 2,
        redirection = 3,
        client_error = 4,
        server_error = 5,
        unclassified
    };

    /**
     * Classify http status
     * @param http_status the http status \ref status_type
     * @return one of the \ref status_class values
     */
    static constexpr status_class classify_status(status_type http_status) {
        auto sc = int(http_status) / 100;
        if (sc < 1 || sc > 5) [[unlikely]] {
            return status_class::unclassified;
        }
        return static_cast<status_class>(sc);
    }

    /**
     * The headers to be included in the reply.
     */
    std::unordered_map<sstring, sstring, seastar::internal::case_insensitive_hash, seastar::internal::case_insensitive_cmp> _headers;

    sstring _version;
    /**
     * The content to be sent in the reply.
     */
    sstring _content;
    size_t content_length = 0; // valid when received via client connection
    size_t consumed_content = 0;
    bool _skip_body = false;

    sstring _response_line;
    std::unordered_map<sstring, sstring> trailing_headers;
    std::unordered_map<sstring, sstring> chunk_extensions;

    reply()
            : _status(status_type::ok) {
    }

    reply& add_header(const sstring& h, const sstring& value) {
        _headers[h] = value;
        return *this;
    }

    /**
     * Search for the first header of a given name
     * @param name the header name
     * @return a pointer to the header value, if it exists or empty string
     */
    sstring get_header(const sstring& name) const {
        auto res = _headers.find(name);
        if (res == _headers.end()) {
            return "";
        }
        return res->second;
    }

    reply& set_version(const sstring& version) {
        _version = version;
        return *this;
    }

    reply& set_status(status_type status, sstring content = "") {
        _status = status;
        if (content != "") {
            _content = std::move(content);
        }
        return *this;
    }

    /**
     * Set the content type mime type.
     * Used when the mime type is known.
     * For most cases, use the set_content_type
     */
    reply& set_mime_type(const sstring& mime) {
        _headers["Content-Type"] = mime;
        return *this;
    }

    /**
     * Set the content type mime type according to the file extension
     * that would have been used if it was a file: e.g. html, txt, json etc'
     */
    reply& set_content_type(const sstring& content_type = "html") {
        set_mime_type(http::mime_types::extension_to_type(content_type));
        return *this;
    }

    reply& done(const sstring& content_type) {
        return set_content_type(content_type).done();
    }
    /**
     * Done should be called before using the reply.
     * It would set the response line
     */
    reply& done() {
        _response_line = response_line();
        return *this;
    }
    sstring response_line() const;

    /*!
     * \brief use an output stream to write the message body
     *
     * When a handler needs to use an output stream it should call this method
     * with a function.
     *
     * \param content_type - is used to choose the content type of the body. Use the file extension
     *  you would have used for such a content, (i.e. "txt", "html", "json", etc')
     * \param body_writer - a function that accept an output stream and use that stream to write the body.
     *   The function should take ownership of the stream while using it and must close the stream when it
     *   is done.
     *
     * Message would use chunked transfer encoding in the reply.
     *
     */

    void write_body(const sstring& content_type, noncopyable_function<future<>(output_stream<char>&&)>&& body_writer);

    /*!
     * \brief use and output stream to write the message body
     *
     * The same as above, but the handler can only .write() data into the stream, it will be
     * closed (and flushed) automatically after the writer fn resolves
     */
    template <typename W>
    requires std::is_invocable_r_v<future<>, W, output_stream<char>&>
    void write_body(const sstring& content_type, W&& body_writer) {
        write_body(content_type, [body_writer = std::move(body_writer)] (output_stream<char>&& out) mutable -> future<> {
            return util::write_to_stream_and_close(std::move(out), std::move(body_writer));
        });
    }

    /*!
     * \brief Write a string as the reply
     *
     * \param content_type - is used to choose the content type of the body. Use the file extension
     *  you would have used for such a content, (i.e. "txt", "html", "json", etc')
     * \param content - the message content.
     * This would set the the content and content type of the message along
     * with any additional information that is needed to send the message.
     */
    void write_body(const sstring& content_type, sstring content);

    // RFC7231 Sec. 4.3.2
    // For HEAD replies collect everything from the handler, but don't write the body itself
    void skip_body() noexcept {
        _skip_body = true;
    }

private:
    future<> write_reply_to_connection(httpd::connection& con);
    future<> write_reply_headers(httpd::connection& connection);

    noncopyable_function<future<>(output_stream<char>&&)> _body_writer;
    friend class httpd::routes;
    friend class httpd::connection;
};

std::ostream& operator<<(std::ostream& os, status_type st);
std::ostream& operator<<(std::ostream& os, status_type::status_init st);

} // namespace http

namespace httpd {
using reply [[deprecated("Use http::reply instead")]] = http::reply;
}

SEASTAR_MODULE_EXPORT_END
}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<seastar::http::status_type> : fmt::ostream_formatter {};
template <> struct fmt::formatter<seastar::http::status_type::status_init> : fmt::formatter<seastar::http::status_type> {};
#endif

/**
 * Temporary addition to enable existing code, using things
 * like std::underlying_type_t<status_type> for casts etc
 * to continue worksing. This is not a fully kosher way of
 * treating this overload, but it is mostly harmless...
 */
template<> 
struct std::underlying_type<seastar::http::status_type> {
    using type = int;
};

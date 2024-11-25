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
 * A reply to be sent to a client.
 */
struct reply {
    /**
     * The status of the reply.
     */
    enum class status_type {
        continue_ = 100, //!< continue
        switching_protocols = 101, //!< switching_protocols
        ok = 200, //!< ok
        created = 201, //!< created
        accepted = 202, //!< accepted
        nonauthoritative_information = 203, //!< nonauthoritative_information
        no_content = 204, //!< no_content
        reset_content = 205, //!< reset_content
        partial_content = 206, //! partial_content
        multiple_choices = 300, //!< multiple_choices
        moved_permanently = 301, //!< moved_permanently
        moved_temporarily = 302, //!< moved_temporarily
        see_other = 303, //!< see_other
        not_modified = 304, //!< not_modified
        use_proxy = 305, //!< use_proxy
        temporary_redirect = 307, //!< temporary_redirect
        bad_request = 400, //!< bad_request
        unauthorized = 401, //!< unauthorized
        payment_required = 402, //!< payment_required
        forbidden = 403, //!< forbidden
        not_found = 404, //!< not_found
        method_not_allowed = 405, //!< method_not_allowed
        not_acceptable = 406, //!< not_acceptable
        request_timeout = 408, //!< request_timeout
        conflict = 409, //!< conflict
        gone = 410, //!< gone
        length_required = 411, //!< length_required
        payload_too_large = 413, //!< payload_too_large
        uri_too_long = 414, //!< uri_too_long
        unsupported_media_type = 415, //!< unsupported_media_type
        expectation_failed = 417, //!< expectation_failed
        page_expired = 419, //!< page_expired
        unprocessable_entity = 422, //!< unprocessable_entity
        upgrade_required = 426, //!< upgrade_required
        too_many_requests = 429, //!< too_many_requests
        login_timeout = 440, //!< login_timeout
        internal_server_error = 500, //!< internal_server_error
        not_implemented = 501, //!< not_implemented
        bad_gateway = 502, //!< bad_gateway
        service_unavailable = 503,  //!< service_unavailable
        gateway_timeout = 504, //!< gateway_timeout
        http_version_not_supported = 505, //!< http_version_not_supported
        insufficient_storage = 507, //!< insufficient_storage
        bandwidth_limit_exceeded = 509, //!< bandwidth_limit_exceeded
        network_read_timeout = 598, //!< network_read_timeout
        network_connect_timeout = 599, //!< network_connect_timeout
    } _status;

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
        auto sc = static_cast<std::underlying_type_t<status_type>>(http_status) / 100;
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

private:
    future<> write_reply_to_connection(httpd::connection& con);
    future<> write_reply_headers(httpd::connection& connection);

    noncopyable_function<future<>(output_stream<char>&&)> _body_writer;
    friend class httpd::routes;
    friend class httpd::connection;
};

std::ostream& operator<<(std::ostream& os, reply::status_type st);

} // namespace http

namespace httpd {
using reply [[deprecated("Use http::reply instead")]] = http::reply;
}

SEASTAR_MODULE_EXPORT_END
}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<seastar::http::reply::status_type> : fmt::ostream_formatter {};
#endif

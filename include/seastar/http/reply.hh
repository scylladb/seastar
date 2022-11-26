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

#include <seastar/core/sstring.hh>
#include <unordered_map>
#include <seastar/http/mime_types.hh>
#include <seastar/core/iostream.hh>
#include <seastar/util/noncopyable_function.hh>

namespace seastar {

struct http_response;

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
        unprocessable_entity = 422, //!< unprocessable_entity
        upgrade_required = 426, //!< upgrade_required
        too_many_requests = 429, //!< too_many_requests
        internal_server_error = 500, //!< internal_server_error
        not_implemented = 501, //!< not_implemented
        bad_gateway = 502, //!< bad_gateway
        service_unavailable = 503,  //!< service_unavailable
        gateway_timeout = 504, //!< gateway_timeout
        http_version_not_supported = 505, //!< http_version_not_supported 
        insufficient_storage = 507 //!< insufficient_storage
    } _status;

    /**
     * The headers to be included in the reply.
     */
    std::unordered_map<sstring, sstring> _headers;

    sstring _version;
    /**
     * The content to be sent in the reply.
     */
    sstring _content;

    sstring _response_line;
    std::unordered_map<sstring, sstring> trailing_headers;
    std::unordered_map<sstring, sstring> chunk_extensions;

    reply()
            : _status(status_type::ok) {
    }

    explicit reply(http_response&&);

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
    sstring response_line();

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

}

#if FMT_VERSION >= 90000
template <> struct fmt::formatter<seastar::http::reply::status_type> : fmt::ostream_formatter {};
#endif

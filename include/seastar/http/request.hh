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

//
// request.hpp
// ~~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include <seastar/core/iostream.hh>
#include <seastar/core/sstring.hh>
#include <string_view>
#include <strings.h>
#include <seastar/http/common.hh>
#include <seastar/http/mime_types.hh>
#include <seastar/http/types.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/core/iostream.hh>
#include <seastar/util/string_utils.hh>
#include <seastar/util/iostream.hh>

namespace seastar {

namespace http {

namespace experimental { class connection; }

/**
 * A request received from a client.
 */
struct request {
    enum class ctclass
        : char {
            other, multipart, app_x_www_urlencoded,
    };

    socket_address _client_address;
    socket_address _server_address;
    sstring _method;
    sstring _url;
    sstring _version;
    ctclass content_type_class;
    size_t content_length = 0;
    mutable size_t _bytes_written = 0;
    std::unordered_map<sstring, sstring, seastar::internal::case_insensitive_hash, seastar::internal::case_insensitive_cmp> _headers;
    // deprecated: it is used to store last value of query parameters, but will be removed in the future
    [[deprecated("Use helper methods instead")]] std::unordered_map<sstring, sstring> query_parameters;
    httpd::parameters param;
    [[deprecated("use content_stream (server-side) / write_body (client-side) instead")]]
    sstring content; // server-side deprecated: use content_stream instead
    /*
     * The handler should read the contents of this stream till reaching eof (i.e., the end of this request's content). Failing to do so
     * will force the server to close this connection, and the client will not be able to reuse this connection for the next request.
     * The stream should not be closed by the handler, the server will close it for the handler.
     * */
    input_stream<char>* content_stream;
    std::unordered_map<sstring, sstring> trailing_headers;
    std::unordered_map<sstring, sstring> chunk_extensions;
    sstring protocol_name = "http";
    http::body_writer_type body_writer; // for client

    using query_parameters_type = std::unordered_map<sstring, std::vector<sstring>, seastar::internal::string_view_hash, std::equal_to<>>;
private:
    query_parameters_type _query_params;
public:

// NOTE: Remove this once both `query_parameters` and `content` are removed
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    request() = default;
    request(request&&) = default;
    request& operator=(request&&) = default;
    ~request() = default;
#pragma GCC diagnostic pop

    /**
     * Get the address of the client that generated the request
     * @return The address of the client that generated the request
     */
    const socket_address & get_client_address() const {
        return _client_address;
    }

    /**
     * Get the address of the server that handled the request
     * @return The address of the server that handled the request
     */
    const socket_address & get_server_address() const {
        return _server_address;
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

    /**
     * Does the query parameters contain a given key?
     * @param key the query parameter key
     * @return true if the key exists, false otherwise
     */
    bool has_query_param(std::string_view key) const {
        return _query_params.contains(key);
    }

    /**
     * Search for the last query parameter of a given key
     * @param key the query parameter key
     * @return the query parameter value, if it exists or the default_value otherwise
     */
    sstring get_query_param(std::string_view key, std::string_view default_value = "") const {
        auto res = _query_params.find(key);
        if (res != _query_params.end()) {
            return res->second.back();
        }

        return sstring(default_value);
    }

    /**
     * Search for all query parameters of a given key
     * @param key the query parameter key
     * @return a vector of all query parameter values, if it exists or an empty vector
     */
    const std::vector<sstring>& get_query_param_array(std::string_view key) const {
        if (auto res = _query_params.find(key); res != _query_params.end()) {
            return res->second;
        }
        static const std::vector<sstring> empty_vector;
        return empty_vector;
    }

    /**
     * Get all query parameters
     * @return a map of all query parameters
     */
    const query_parameters_type& get_query_params() const {
        return _query_params;
    }

    /**
     * Set a query parameter value
     * @param key the query parameter key
     * @param value the query parameter value
     * @return a reference to this request object
     */
    request& set_query_param(std::string_view key, std::string_view value) {
        return set_query_param(key, {value});
    }

    /**
     * Set a query parameter value
     * @param key the query parameter key
     * @param values the query parameter values
     * @return a reference to this request object
    */
    request& set_query_param(std::string_view key, std::initializer_list<std::string_view> values) {
        _query_params[sstring(key)] = std::vector<sstring>(values.begin(), values.end());
        return *this;
    }

    /**
     * Set a query parameter value
     * @param key the query parameter key
     * @param values the query parameter values
     * @return a reference to this request object
    */
    request& set_query_param(std::string_view key, std::vector<sstring> values) {
        _query_params[sstring(key)] = std::move(values);
        return *this;
    }

    /**
     * Set the query parameters in the request objects.
     * @param params a map of query parameters
     * @return a reference to this request object
     */
    request& set_query_params(query_parameters_type params) {
        _query_params = std::move(params);
        return *this;
    }

    /**
     * Search for the last path parameter of a given key
     * @param key the path paramerter key
     * @return the unescaped path parameter value, if it exists and can be path decoded successfully, otherwise it
     *  returns an empty string
     */
    sstring get_path_param(const sstring& key) const {
        return param.get_decoded_param(key);
    }

    /**
     * Get the request protocol name. Can be either "http" or "https".
     */
    sstring get_protocol_name() const {
        return protocol_name;
    }

    /**
     * Get the request url.
     * @return the request url
     */
    sstring get_url() const {
        return get_protocol_name() + "://" + get_header("Host") + _url;
    }

    bool is_multi_part() const {
        return content_type_class == ctclass::multipart;
    }

    bool is_form_post() const {
        return content_type_class == ctclass::app_x_www_urlencoded;
    }

    bool should_keep_alive() const {
        if (_version == "0.9") {
            return false;
        }

        // TODO: handle HTTP/2.0 when it releases

        auto it = _headers.find("Connection");
        if (_version == "1.0") {
            return it != _headers.end()
                 && seastar::internal::case_insensitive_cmp()(it->second, "keep-alive");
        } else { // HTTP/1.1
            return it == _headers.end() || !seastar::internal::case_insensitive_cmp()(it->second, "close");
        }
    }

    /**
     * Set the query parameters in the request objects.
     * Returns the URL path part, i.e. -- without the query paremters
     * query param appear after the question mark and are separated
     * by the ampersand sign
     */
    sstring parse_query_param();

    /**
     * Generates the URL string from the _url and query_parameters
     * values in a form parseable by the above method
     */
    sstring format_url() const;

    /**
     * Set the content type. The content_type string can be one of:
     * 1. A MIME (RFC 2045) Content-Type - looking like "type/subtype", e.g.,
     *   "text/html"
     * 2. If a "/" is missing in the given string, we look it up in a list of
     *    common file extensions listed in the http::mime_types map. For
     *    example "html" will be mapped to "text/html".
     */
    void set_content_type(std::string_view content_type) {
        if (content_type.find('/') == std::string_view::npos) {
            content_type = http::mime_types::extension_to_type(content_type);
        }
        _headers["Content-Type"] = sstring(content_type);
    }

    [[deprecated("Use set_content_type(std::string_view) instead")]]
    void set_content_type() {
        set_content_type("html");
    }

    [[deprecated("Use set_content_type(std::string_view) instead")]]
    void set_mime_type(const sstring& mime) {
        set_content_type(mime);
    }

    /**
     * \brief Write a string as the body
     *
     * \param content_type - is used to choose the content type of the body. Use the file extension
     *  you would have used for such a content, (i.e. "txt", "html", "json", etc')
     * \param content - the message content.
     * This would set the the content, conent length and content type of the message along
     * with any additional information that is needed to send the message.
     *
     * This method is good to be used if the body is available as a contiguous buffer.
     */
    void write_body(const sstring& content_type, sstring content);

    /**
     * \brief Use an output stream to write the message body
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
     * This method can be used to write body of unknown or hard to evaluate length. For example,
     * when sending the contents of some other input_stream or when the body is available as a
     * collection of memory buffers. Message would use chunked transfer encoding.
     *
     */
    void write_body(const sstring& content_type, http::body_writer_type&& body_writer);

    /*!
     * \brief use and output stream to write the message body
     *
     * The same as above, but the caller can only .write() data into the stream, it will be
     * closed (and flushed) automatically after the writer fn resolves
     */
    template <typename W>
    requires std::is_invocable_r_v<future<>, W, output_stream<char>&>
    void write_body(const sstring& content_type, W&& body_writer) {
        write_body(content_type, [body_writer = std::move(body_writer)] (output_stream<char>&& out) mutable -> future<> {
            return util::write_to_stream_and_close(std::move(out), std::move(body_writer));
        });
    }

    /**
     * \brief Use an output stream to write the message body
     *
     * When a handler needs to use an output stream it should call this method
     * with a function.
     *
     * \param content_type - is used to choose the content type of the body. Use the file extension
     *  you would have used for such a content, (i.e. "txt", "html", "json", etc')
     * \param len - known in advance content length
     * \param body_writer - a function that accept an output stream and use that stream to write the body.
     *   The function should take ownership of the stream while using it and must close the stream when it
     *   is done.
     *
     * This method is to be used when the body is not available of a single contiguous buffer, but the
     * size of it is known and it's desirable to provide it to the server, or when the server strongly
     * requires the content-length header for any reason.
     *
     * Message would use plain encoding in the the reply with Content-Length header set accordingly.
     * If the body_writer doesn't generate enough bytes into the stream or tries to put more data into
     * the stream, sending the request would resolve with exceptional future.
     *
     */
    void write_body(const sstring& content_type, size_t len, http::body_writer_type&& body_writer);

    /*!
     * \brief use and output stream to write the message body
     *
     * The same as above, but the caller can only .write() data into the stream, it will be
     * closed (and flushed) automatically after the writer fn resolves
     */
    template <typename W>
    requires std::is_invocable_r_v<future<>, W, output_stream<char>&>
    void write_body(const sstring& content_type, size_t len, W&& body_writer) {
        write_body(content_type, len, [body_writer = std::move(body_writer)] (output_stream<char>&& out) mutable -> future<> {
            return util::write_to_stream_and_close(std::move(out), std::move(body_writer));
        });
    }

    /**
     * \brief Make request send Expect header
     *
     * When set, the connection::make_request will send the Expect header and will wait for the
     * server resply before tranferring the body
     *
     */
    void set_expects_continue();

    /**
     * \brief Make simple request
     *
     * \param method - method to use, e.g. "GET" or "POST"
     * \param host - host to contact. This value will be used as the "Host" header
     * \path - the URL to send the request to
     *
     */
    static request make(sstring method, sstring host, sstring path);

    /**
     * \brief Make simple request
     *
     * \param method - method to use, e.g. operation_type::GET
     * \param host - host to contact. This value will be used as the "Host" header
     * \path - the URL to send the request to
     *
     */
    static request make(httpd::operation_type type, sstring host, sstring path);

    sstring request_line() const;
    future<> write_request_headers(output_stream<char>& out) const;
private:
    void add_query_param(std::string_view param);
    friend class experimental::connection;
};

namespace internal {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
inline sstring& deprecated_content(request& req) noexcept { return req.content; }
inline const sstring& deprecated_content(const request& req) noexcept { return req.content; }
#pragma GCC diagnostic pop
}

} // namespace httpd

namespace httpd {
using request [[deprecated("Use http::request instead")]] = http::request;
}

}

template <>
struct fmt::formatter<seastar::http::request> {
    constexpr auto parse(format_parse_context& ctx) { return ctx.begin(); }
    auto format(const seastar::http::request& rq, fmt::format_context& ctx) const {
        auto out = fmt::format_to(ctx.out(), "{} {}", rq._method, rq._url);
        for (const auto& h : rq._headers) {
            out = fmt::format_to(out, " {}:{}", h.first, h.second);
        }
        if (!rq.body_writer) {
            const auto& body = seastar::http::internal::deprecated_content(rq);
            if (!body.empty()) {
                out = fmt::format_to(out, " {}", body);
            }
        }
        return out;
    }
};

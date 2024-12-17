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
#include <strings.h>
#include <seastar/http/common.hh>
#include <seastar/http/mime_types.hh>
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
    std::unordered_map<sstring, sstring> query_parameters;
    httpd::parameters param;
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
    noncopyable_function<future<>(output_stream<char>&&)> body_writer; // for client

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
     * Search for the last query parameter of a given key
     * @param key the query paramerter key
     * @return the query parameter value, if it exists or empty string
     */
    sstring get_query_param(const sstring& key) const {
        auto res = query_parameters.find(key);
        if (res == query_parameters.end()) {
            return "";
        }
        return res->second;
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
     * Set the content type mime type.
     * Used when the mime type is known.
     * For most cases, use the set_content_type
     */
    void set_mime_type(const sstring& mime) {
        _headers["Content-Type"] = mime;
    }

    /**
     * Set the content type mime type according to the file extension
     * that would have been used if it was a file: e.g. html, txt, json etc'
     */
    void set_content_type(const sstring& content_type = "html") {
        set_mime_type(http::mime_types::extension_to_type(content_type));
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
    void write_body(const sstring& content_type, noncopyable_function<future<>(output_stream<char>&&)>&& body_writer);

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
    void write_body(const sstring& content_type, size_t len, noncopyable_function<future<>(output_stream<char>&&)>&& body_writer);

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

} // namespace httpd

namespace httpd {
using request [[deprecated("Use http::request instead")]] = http::request;
}

}

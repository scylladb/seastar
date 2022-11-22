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

#include <seastar/net/api.hh>
#include <seastar/core/iostream.hh>

namespace seastar {

namespace http {

struct request;
struct reply;

namespace experimental {

/**
 * \brief Class connection represents an HTTP connection over a given transport
 *
 * Check the demos/http_client_demo.cc for usage example
 */

class connection {
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;

public:
    /**
     * \brief Create an http connection
     *
     * Construct the connection that will work over the provided \fd transport socket
     *
     */
    connection(connected_socket&& fd);

    /**
     * \brief Send the request and wait for response
     *
     * Sends the provided request to the server, and returns a future that will resolve
     * into the server response.
     *
     * If the request was configured with the set_expects_continue() and the server replied
     * early with some error code, this early reply will be returned back.
     *
     * The returned reply only contains the status and headers. To get the reply body the
     * caller should read it via the input_stream provided by the connection.in() method.
     *
     * \param rq -- request to be sent
     *
     */
    future<reply> make_request(request rq);

    /**
     * \brief Get a reference on the connection input stream
     *
     * The stream can be used to get back the server response body. After the stream is
     * finished, the reply can be additionally updated with trailing headers and chunk
     * extentions
     *
     */
    input_stream<char> in(reply& rep);

    /**
     * \brief Closes the connection
     *
     * Connection must be closed regardless of whether there was an exception making the
     * request or not
     */
    future<> close();

private:
    future<> send_request_head(request& rq);
    future<std::optional<reply>> maybe_wait_for_continue(request& req);
    future<> write_body(request& rq);
    future<reply> recv_reply();
};

} // experimental namespace

} // http namespace

} // seastar namespace

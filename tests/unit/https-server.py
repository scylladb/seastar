#!/usr/bin/env python3
#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

#
# Copyright (C) 2023 Kefu Chai ( tchaikov@gmail.com )
#


import argparse
import socket
import ssl
from http.server import HTTPServer as _HTTPServer
from http.server import SimpleHTTPRequestHandler


class HTTPSServer(_HTTPServer):
    def __init__(self, addr, port, context):
        super().__init__((addr, port), SimpleHTTPRequestHandler)
        self.context = context

    def get_request(self):
        sock, addr = self.socket.accept()
        ssl_conn = self.context.wrap_socket(sock, server_side=True)
        return ssl_conn, addr

    def get_listen_port(self):
        if self.socket.family == socket.AF_INET:
            addr, port = self.socket.getsockname()
            return port
        elif self.socket.family == socket.AF_INET6:
            address, port, flowinfo, scope_id = self.socket.getsockname()
            return port
        else:
            raise Exception(f"unknown family: {self.socket.family}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="httpd for testing TLS")
    parser.add_argument('--server', action='store',
                        help='server address in <host>:<port> format',
                        default='localhost:11311')
    parser.add_argument('--cert', action='store',
                        help='path to the certificate')
    parser.add_argument('--key', action='store',
                        help='path to the private key')
    args = parser.parse_args()
    host, port = args.server.split(':')

    context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    context.load_cert_chain(certfile=args.cert, keyfile=args.key)
    with HTTPSServer(host, int(port), context) as server:
        # print out the listening port when ready to serve
        print(server.get_listen_port(), flush=True)
        server.serve_forever()

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
 * Copyright (C) 2026 Kefu Chai (tchaikov@gmail.com)
 */

#pragma once

#include <seastar/net/quic.hh>
#include <boost/dll.hpp>
#include <string>

namespace {

const auto quic_cert_location = boost::dll::program_location().parent_path();

std::string certfile(const std::string& file) {
    return (quic_cert_location / file).string();
}

/// Build server credentials from the testcrt self-signed certificate.
/// If @p alpn is non-empty, sets the ALPN to that value.
seastar::shared_ptr<seastar::experimental::quic::credentials>
make_server_creds(std::string alpn = {}) {
    auto creds = seastar::make_shared<seastar::experimental::quic::credentials>();
    creds->set_x509_key_file(certfile("test.crt"), certfile("test.key"));
    if (!alpn.empty()) {
        creds->set_alpn(std::move(alpn));
    }
    return creds;
}

/// Build client credentials for loopback testing (no cert verification).
/// If @p alpn is non-empty, sets the ALPN to that value.
seastar::shared_ptr<seastar::experimental::quic::credentials>
make_client_creds(std::string alpn = {}) {
    auto creds = seastar::make_shared<seastar::experimental::quic::credentials>();
    creds->set_no_verify();
    if (!alpn.empty()) {
        creds->set_alpn(std::move(alpn));
    }
    return creds;
}

} // anonymous namespace

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
 * Copyright (C) 2015 Cloudius Systems, Ltd.
 */

#include "core/do_with.hh"
#include "test-utils.hh"
#include "core/sstring.hh"
#include "core/reactor.hh"
#include "core/do_with.hh"
#include "core/future-util.hh"
#include "net/tls.hh"

using namespace seastar;

SEASTAR_TEST_CASE(test_simple_x509_client) {
    auto certs = ::make_shared<tls::certificate_credentials>();
    return certs->set_x509_trust_file("tests/tls-ca-bundle.pem", tls::x509_crt_format::PEM).then([certs]() {
        auto addr = make_ipv4_address(ipv4_addr("216.58.209.132:443"));
        return tls::connect(certs, addr, "www.google.com").then([](connected_socket s) {
            return do_with(std::move(s), [](connected_socket& s) {
                return do_with(s.output(), [&s](auto& os) {
                    static const sstring msg("GET / HTTP/1.0\r\n\r\n");
                    auto f = os.write(msg);
                    return f.then([&s, &os]() mutable {
                        auto f = os.flush();
                        return f.then([&s, &os]() mutable {
                            return do_with(s.input(), [](auto& in) {
                                auto f = in.read();
                                return f.then([](temporary_buffer<char> buf) {
                                    // std::cout << buf.get() << std::endl;
                                    BOOST_CHECK(strncmp(buf.get(), "HTTP/", 5) == 0);
                                    BOOST_CHECK(buf.size() > 8);
                                });
                            });
                        });
                    }).finally([&os] {
                        return os.close();
                    });
                });
            });
        });
    });
}

/*
 * Certificates
 *
 * KEY:
 * certtool --generate-privkey --outfile tests/test.key
 *
 * CRT:
 * certtool --generate-self-signed --load-privkey tests/test.key --outfile tests/test.crt
 *
 * CRL:
 * certtool --generate-crl --load-ca-privkey tests/test.key --load-ca-certificate tests/test.crt --outfile=tests/test.crl
 *
 *
 */
SEASTAR_TEST_CASE(test_simple_x509_client_server) {
    static const auto port = 4711;
    static const sstring message = "hej lilla fisk du kan dansa fint";
    static const int loops = 20;

    auto dh = ::make_shared<tls::dh_params>();
    auto certs = ::make_shared<tls::server_credentials>(dh);

    auto f = certs->set_x509_trust_file("tests/tls-ca-bundle.pem", tls::x509_crt_format::PEM).then([certs]() {
        return certs->set_x509_key_file("tests/test.crt", "tests/test.key", tls::x509_crt_format::PEM).then([certs]() {
            return certs->set_x509_crl_file("tests/test.crl", tls::x509_crt_format::PEM);
        });
    });

    struct streams {
        ::connected_socket s;
        input_stream<char> in;
        output_stream<char> out;

        streams(::connected_socket cs) : s(std::move(cs)), in(s.input()), out(s.output())
        {}
    };

    return f.then([certs]() {
        listen_options opts;
        opts.reuse_address = true;
        return do_with(tls::listen(certs, ::make_ipv4_address( {port}), opts), [certs](::server_socket& ss) {
            return when_all(
                tls::connect(certs, ::make_ipv4_address( {port})).then([](::connected_socket s) {
                    auto strms = ::make_lw_shared<streams>(std::move(s));
                    auto n = ::make_lw_shared<int>();
                    return repeat([strms, n]() mutable {
                        return strms->out.write(message).then([strms, n]() {
                            return strms->out.flush().then([strms] {
                                return strms->in.read_exactly(message.size()).then([](temporary_buffer<char> buf) {
                                    sstring tmp(buf.begin(), buf.end());
                                    BOOST_CHECK(message == tmp);
                                });
                            }).then([n] {
                                ++(*n);
                                return make_ready_future<stop_iteration>(*n == loops ? stop_iteration::yes : stop_iteration::no);
                            });
                        });
                    }).then([strms]{
                        return strms->out.close();
                    }).finally([strms]{});
                }),
                ss.accept().then([](::connected_socket s, ::socket_address) {
                    auto strms = ::make_lw_shared<streams>(std::move(s));
                    auto n = ::make_lw_shared<int>();
                    return repeat([strms, s = std::move(s), n]() mutable {
                        return strms->in.read_exactly(message.size()).then([strms, n](temporary_buffer<char> buf) {
                            sstring tmp(buf.begin(), buf.end());
                            BOOST_CHECK(message == tmp);
                            return strms->out.write(message).then([strms]() {
                                return strms->out.flush();
                            }).then([n] {
                                ++(*n);
                                return make_ready_future<stop_iteration>(*n == loops ? stop_iteration::yes : stop_iteration::no);
                            });
                        });
                    }).then([strms]{
                        return strms->out.close();
                    }).finally([strms]{});
                })
            ).then([](std::tuple<future<>, future<>> pe) {
                std::get<0>(pe);
                std::get<1>(pe);
            });
        });
    });
}

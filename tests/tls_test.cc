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
#include "core/sharded.hh"
#include "core/gate.hh"
#include "net/tls.hh"

using namespace seastar;

static future<> connect_to_ssl_google(::shared_ptr<tls::certificate_credentials> certs) {
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
}

SEASTAR_TEST_CASE(test_simple_x509_client) {
    auto certs = ::make_shared<tls::certificate_credentials>();
    return certs->set_x509_trust_file("tests/tls-ca-bundle.pem", tls::x509_crt_format::PEM).then([certs]() {
        return connect_to_ssl_google(certs);
    });
}

SEASTAR_TEST_CASE(test_x509_client_with_system_trust) {
    auto certs = ::make_shared<tls::certificate_credentials>();
    return certs->set_system_trust().then([certs]() {
        return connect_to_ssl_google(certs);
    });
}

struct streams {
    ::connected_socket s;
    input_stream<char> in;
    output_stream<char> out;

    streams(::connected_socket cs) : s(std::move(cs)), in(s.input()), out(s.output())
    {}
};

static const sstring message = "hej lilla fisk du kan dansa fint";

class echoserver {
    ::server_socket _socket;
    ::shared_ptr<tls::server_credentials> _certs;
    seastar::gate _gate;
    bool _stopped = false;
public:
    echoserver()
            : _certs(
                    ::make_shared<tls::server_credentials>(
                            ::make_shared<tls::dh_params>())) {
    }

    future<> listen(socket_address addr, sstring crtfile, sstring keyfile) {
        return _certs->set_x509_key_file(crtfile, keyfile, tls::x509_crt_format::PEM).then([this, addr] {
            ::listen_options opts;
            opts.reuse_address = true;

            _socket = tls::listen(_certs, addr, opts);

            with_gate(_gate, [this] {
                return _socket.accept().then([this](::connected_socket s, socket_address) {
                    auto strms = ::make_lw_shared<streams>(std::move(s));
                    return repeat([strms]() {
                        return strms->in.read_exactly(message.size()).then([strms](temporary_buffer<char> buf) {
                            if (buf.empty()) {
                                return make_ready_future<stop_iteration>(stop_iteration::yes);
                            }
                            sstring tmp(buf.begin(), buf.end());
                            return strms->out.write(tmp).then([strms]() {
                                return strms->out.flush();
                            }).then([] {
                                return make_ready_future<stop_iteration>(stop_iteration::no);
                            });
                        });
                    }).then([strms]{
                        return strms->out.close();
                    }).finally([strms]{});
                }).handle_exception([this](auto ep) {
                    if (_stopped) {
                        return make_ready_future<>();
                    }
                    return make_exception_future(std::move(ep));
                });
            });
            return make_ready_future<>();
        });
    }

    future<> stop() {
        _stopped = true;
        _socket.abort_accept();
        return _gate.close();
    }
};

static future<> run_echo_test(int loops, sstring trust, sstring name, sstring crt = "tests/test.crt", sstring key = "tests/test.key") {
    static const auto port = 4711;

    auto certs = ::make_shared<tls::certificate_credentials>();
    auto server = ::make_shared<seastar::sharded<echoserver>>();
    auto addr = ::make_ipv4_address( {0x7f000001, port});

    return certs->set_x509_trust_file(trust, tls::x509_crt_format::PEM).then([=] {
        return server->start().then([=]() {
            return server->invoke_on_all(&echoserver::listen, addr, crt, key);
        }).then([=] {
            return tls::connect(certs, addr, name).then([loops](::connected_socket s) {
                auto strms = ::make_lw_shared<streams>(std::move(s));
                auto range = boost::irange(0, loops);
                return do_for_each(range, [strms](auto) {
                    return strms->out.write(message).then([strms]() {
                        return strms->out.flush().then([strms] {
                            return strms->in.read_exactly(message.size()).then([](temporary_buffer<char> buf) {
                                sstring tmp(buf.begin(), buf.end());
                                BOOST_CHECK(message == tmp);
                            });
                        });
                    });
                }).then([strms]{
                    return strms->out.close();
                }).finally([strms]{});
            });
        }).finally([server] {
            return server->stop().finally([server]{});
        });
    });
}

/*
 * Certificates:
 *
 * make -f tests/mkcert.gmk domain=scylladb.org server=test
 *
 * ->   test.crt
 *      test.csr
 *      catest.pem
 *      catest.key
 *
 * catest == snakeoil root authority for these self-signed certs
 *
 */
SEASTAR_TEST_CASE(test_simple_x509_client_server) {
    // Make sure we load our own auth trust pem file, otherwise our certs
    // will not validate
    // Must match expected name with cert CA or give empty name to ignore
    // server name
    return run_echo_test(20, "tests/catest.pem", "test.scylladb.org");
}


SEASTAR_TEST_CASE(test_simple_x509_client_server_again) {
    return run_echo_test(20, "tests/catest.pem", "test.scylladb.org");
}

SEASTAR_TEST_CASE(test_x509_client_server_cert_validation_fail) {
    // Load a real trust authority here, which out certs are _not_ signed with.
    return run_echo_test(1, "tests/tls-ca-bundle.pem", {}).then([] {
            BOOST_FAIL("Should have gotten validation error");
    }).handle_exception([](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (tls::verification_error&) {
            // ok.
        } catch (...) {
            BOOST_FAIL("Unexpected exception");
        }
    });
}

SEASTAR_TEST_CASE(test_x509_client_server_cert_validation_fail_name) {
    // Use trust store with our signer, but wrong host name
    return run_echo_test(1, "tests/tls-ca-bundle.pem", "nils.holgersson.gov").then([] {
            BOOST_FAIL("Should have gotten validation error");
    }).handle_exception([](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (tls::verification_error&) {
            // ok.
        } catch (...) {
            BOOST_FAIL("Unexpected exception");
        }
    });
}

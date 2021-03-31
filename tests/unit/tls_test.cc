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

#include <iostream>

#include <seastar/core/do_with.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/net/tls.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>

#include <boost/dll.hpp>

#include "loopback_socket.hh"
#include "tmpdir.hh"

#include <gnutls/gnutls.h>

#if 0

static void enable_gnutls_logging() {
    gnutls_global_set_log_level(99);
        gnutls_global_set_log_function([](int lv, const char * msg) {
           std::cerr << "GNUTLS (" << lv << ") " << msg << std::endl;
        });
}
#endif

static const auto cert_location = boost::dll::program_location().parent_path();

static std::string certfile(const std::string& file) {
    return (cert_location / file).string();
}

using namespace seastar;

static future<> connect_to_ssl_addr(::shared_ptr<tls::certificate_credentials> certs, socket_address addr) {
    return tls::connect(certs, addr, "www.google.com").then([](connected_socket s) {
        return do_with(std::move(s), [](connected_socket& s) {
            return do_with(s.output(), [&s](auto& os) {
                static const sstring msg("GET / HTTP/1.0\r\n\r\n");
                auto f = os.write(msg);
                return f.then([&s, &os]() mutable {
                    auto f = os.flush();
                    return f.then([&s]() mutable {
                        return do_with(s.input(), [](auto& in) {
                            auto f = in.read();
                            return f.then([](temporary_buffer<char> buf) {
                                // std::cout << buf.get() << std::endl;

                                // Avoid passing a nullptr as an argument of strncmp().
                                // If the temporary_buffer is empty (e.g. due to the underlying TCP connection
                                // being reset) passing the buf.get() (which would be a nullptr) to strncmp()
                                // causes a runtime error which masks the actual issue.
                                if (buf) {
                                    BOOST_CHECK(strncmp(buf.get(), "HTTP/", 5) == 0);
                                }
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

static future<> connect_to_ssl_google(::shared_ptr<tls::certificate_credentials> certs) {
    static socket_address google;

    if (google.is_unspecified()) {
        return net::dns::resolve_name("www.google.com", net::inet_address::family::INET).then([certs](net::inet_address addr) {
            google = socket_address(addr, 443);
            return connect_to_ssl_google(certs);
        });
    }
    return connect_to_ssl_addr(std::move(certs), google);
}

SEASTAR_TEST_CASE(test_simple_x509_client) {
    auto certs = ::make_shared<tls::certificate_credentials>();
    return certs->set_x509_trust_file(certfile("tls-ca-bundle.pem"), tls::x509_crt_format::PEM).then([certs]() {
        return connect_to_ssl_google(certs);
    });
}

SEASTAR_TEST_CASE(test_x509_client_with_system_trust) {
    auto certs = ::make_shared<tls::certificate_credentials>();
    return certs->set_system_trust().then([certs]() {
        return connect_to_ssl_google(certs);
    });
}

SEASTAR_TEST_CASE(test_x509_client_with_builder_system_trust) {
    tls::credentials_builder b;
    (void)b.set_system_trust();
    return connect_to_ssl_google(b.build_certificate_credentials());
}

SEASTAR_TEST_CASE(test_x509_client_with_builder_system_trust_multiple) {
    tls::credentials_builder b;
    (void)b.set_system_trust();
    auto creds = b.build_certificate_credentials();

    return parallel_for_each(boost::irange(0, 20), [creds](auto i) { return connect_to_ssl_google(creds); });
}

SEASTAR_TEST_CASE(test_x509_client_with_priority_strings) {
    static std::vector<sstring> prios( { "NONE:+VERS-TLS-ALL:+MAC-ALL:+RSA:+AES-128-CBC:+SIGN-ALL:+COMP-NULL",
        "NORMAL:+ARCFOUR-128", // means normal ciphers plus ARCFOUR-128.
        "SECURE128:-VERS-SSL3.0:+COMP-DEFLATE", // means that only secure ciphers are enabled, SSL3.0 is disabled, and libz compression enabled.
        "NONE:+VERS-TLS-ALL:+AES-128-CBC:+RSA:+SHA1:+COMP-NULL:+SIGN-RSA-SHA1",
        "SECURE256:+SECURE128",
        "NORMAL:%COMPAT",
        "NORMAL:-MD5",
        "NONE:+VERS-TLS-ALL:+MAC-ALL:+RSA:+AES-128-CBC:+SIGN-ALL:+COMP-NULL",
        "NORMAL:+ARCFOUR-128",
        "SECURE128:-VERS-TLS1.0:+COMP-DEFLATE",
        "SECURE128:+SECURE192:-VERS-TLS-ALL:+VERS-TLS1.2"
    });
    return do_for_each(prios, [](const sstring & prio) {
        tls::credentials_builder b;
        (void)b.set_system_trust();
        b.set_priority_string(prio);
        return connect_to_ssl_google(b.build_certificate_credentials());
    });
}

SEASTAR_TEST_CASE(test_x509_client_with_priority_strings_fail) {
    static std::vector<sstring> prios( { "NONE",
        "NONE:+CURVE-SECP256R1"
    });
    return do_for_each(prios, [](const sstring & prio) {
        tls::credentials_builder b;
        (void)b.set_system_trust();
        b.set_priority_string(prio);
        try {
            return connect_to_ssl_google(b.build_certificate_credentials()).then([] {
                BOOST_FAIL("Expected exception");
            }).handle_exception([](auto ep) {
                // ok.
            });
        } catch (...) {
            // also ok
        }
        return make_ready_future<>();
    });
}

SEASTAR_TEST_CASE(test_failed_connect) {
    tls::credentials_builder b;
    (void)b.set_system_trust();
    return connect_to_ssl_addr(b.build_certificate_credentials(), ipv4_addr()).handle_exception([](auto) {});
}

SEASTAR_TEST_CASE(test_non_tls) {
    ::listen_options opts;
    opts.reuse_address = true;
    auto addr = ::make_ipv4_address( {0x7f000001, 4712});
    auto server = server_socket(seastar::listen(addr, opts));

    auto c = server.accept();

    tls::credentials_builder b;
    (void)b.set_system_trust();

    auto f = connect_to_ssl_addr(b.build_certificate_credentials(), addr);


    return c.then([f = std::move(f)](accept_result ar) mutable {
        ::connected_socket s = std::move(ar.connection);
        std::cerr << "Established connection" << std::endl;
        auto sp = std::make_unique<::connected_socket>(std::move(s));
        timer<> t([s = std::ref(*sp)] {
            std::cerr << "Killing server side" << std::endl;
            s.get() = ::connected_socket();
        });
        t.arm(timer<>::clock::now() + std::chrono::seconds(5));
        return std::move(f).finally([t = std::move(t), sp = std::move(sp)] {});
    }).handle_exception([server = std::move(server)](auto ep) {
        std::cerr << "Got expected exception" << std::endl;
    });
}

SEASTAR_TEST_CASE(test_abort_accept_before_handshake) {
    auto certs = ::make_shared<tls::server_credentials>(::make_shared<tls::dh_params>());
    return certs->set_x509_key_file(certfile("test.crt"), certfile("test.key"), tls::x509_crt_format::PEM).then([certs] {
        ::listen_options opts;
        opts.reuse_address = true;
        auto addr = ::make_ipv4_address( {0x7f000001, 4712});
        auto server = server_socket(tls::listen(certs, addr, opts));
        auto c = server.accept();
        BOOST_CHECK(!c.available()); // should not be finished

        server.abort_accept();

        return c.then([](auto) { BOOST_FAIL("Should not reach"); }).handle_exception([](auto) {
            // ok
        }).finally([server = std::move(server)] {});
    });
}

SEASTAR_TEST_CASE(test_abort_accept_after_handshake) {
    return async([] {
        auto certs = ::make_shared<tls::server_credentials>(::make_shared<tls::dh_params>());
        certs->set_x509_key_file(certfile("test.crt"), certfile("test.key"), tls::x509_crt_format::PEM).get();

        ::listen_options opts;
        opts.reuse_address = true;
        auto addr = ::make_ipv4_address( {0x7f000001, 4712});
        auto server = tls::listen(certs, addr, opts);
        auto sa = server.accept();

        tls::credentials_builder b;
        b.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();

        auto c = tls::connect(b.build_certificate_credentials(), addr).get0();
        server.abort_accept(); // should not affect the socket we got.

        auto s = sa.get0();
        auto out = c.output();
        auto in = s.connection.input();

        out.write("apa").get();
        auto f = out.flush();
        auto buf = in.read().get0();
        f.get();
        BOOST_CHECK(sstring(buf.begin(), buf.end()) == "apa");

        out.close().get();
        in.close().get();
    });
}

SEASTAR_TEST_CASE(test_abort_accept_on_server_before_handshake) {
    return async([] {
        ::listen_options opts;
        opts.reuse_address = true;
        auto addr = ::make_ipv4_address( {0x7f000001, 4712});
        auto server = server_socket(seastar::listen(addr, opts));
        auto sa = server.accept();

        tls::credentials_builder b;
        b.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();

        auto creds = b.build_certificate_credentials();
        auto f = tls::connect(creds, addr);

        server.abort_accept();
        try {
            sa.get();
        } catch (...) {
        }
        server = {};

        try {
            // the connect as such should succeed, but the handshare following it
            // should not.
            auto c = f.get0();
            auto out = c.output();
            out.write("apa").get();
            out.flush().get();
            out.close().get();

            BOOST_FAIL("Expected exception");
        } catch (...) {
            // ok
        }
    });
}


struct streams {
    ::connected_socket s;
    input_stream<char> in;
    output_stream<char> out;

    // note: using custom output_stream, because we don't want polled flush
    streams(::connected_socket cs) : s(std::move(cs)), in(s.input()), out(s.output().detach(), 8192)
    {}
};

static const sstring message = "hej lilla fisk du kan dansa fint";

class echoserver {
    ::server_socket _socket;
    ::shared_ptr<tls::server_credentials> _certs;
    seastar::gate _gate;
    bool _stopped = false;
    size_t _size;
    std::exception_ptr _ex;
public:
    echoserver(size_t message_size, bool use_dh_params = true)
            : _certs(
                    use_dh_params 
                        ? ::make_shared<tls::server_credentials>(::make_shared<tls::dh_params>())
                        : ::make_shared<tls::server_credentials>()
                    )
            , _size(message_size)
    {}

    future<> listen(socket_address addr, sstring crtfile, sstring keyfile, tls::client_auth ca = tls::client_auth::NONE, sstring trust = {}) {
        _certs->set_client_auth(ca);
        auto f = _certs->set_x509_key_file(crtfile, keyfile, tls::x509_crt_format::PEM);
        if (!trust.empty()) {
            f = f.then([this, trust = std::move(trust)] {
                return _certs->set_x509_trust_file(trust, tls::x509_crt_format::PEM);
            });
        }
        return f.then([this, addr] {
            ::listen_options opts;
            opts.reuse_address = true;

            _socket = tls::listen(_certs, addr, opts);

            (void)try_with_gate(_gate, [this] {
                return _socket.accept().then([this](accept_result ar) {
                    ::connected_socket s = std::move(ar.connection);
                    auto strms = ::make_lw_shared<streams>(std::move(s));
                    return repeat([strms, this]() {
                        return strms->in.read_exactly(_size).then([strms](temporary_buffer<char> buf) {
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
                    }).finally([strms]{
                        return strms->out.close();
                    }).finally([strms]{});
                }).handle_exception([this](auto ep) {
                    if (_stopped) {
                        return make_ready_future<>();
                    }
                    _ex = ep;
                    return make_ready_future<>();
                });
            }).handle_exception_type([] (const gate_closed_exception&) {/* ignore */});
            return make_ready_future<>();
        });
    }

    future<> stop() {
        _stopped = true;
        _socket.abort_accept();
        return _gate.close().handle_exception([this] (std::exception_ptr ignored) {
            if (_ex) {
                std::rethrow_exception(_ex);
            }
        });
    }
};

static future<> run_echo_test(sstring message,
                int loops,
                sstring trust,
                sstring name,
                sstring crt = certfile("test.crt"),
                sstring key = certfile("test.key"),
                tls::client_auth ca = tls::client_auth::NONE,
                sstring client_crt = {},
                sstring client_key = {},
                bool do_read = true,
                bool use_dh_params = true,
                tls::dn_callback distinguished_name_callback = {}
)
{
    static const auto port = 4711;

    auto msg = ::make_shared<sstring>(std::move(message));
    auto certs = ::make_shared<tls::certificate_credentials>();
    auto server = ::make_shared<seastar::sharded<echoserver>>();
    auto addr = ::make_ipv4_address( {0x7f000001, port});

    assert(do_read || loops == 1);

    future<> f = make_ready_future();

    if (!client_crt.empty() && !client_key.empty()) {
        f = certs->set_x509_key_file(client_crt, client_key, tls::x509_crt_format::PEM);
        if (distinguished_name_callback) {
            certs->set_dn_verification_callback(std::move(distinguished_name_callback));
        }
    }

    return f.then([=] {
        return certs->set_x509_trust_file(trust, tls::x509_crt_format::PEM);
    }).then([=] {
        return server->start(msg->size(), use_dh_params).then([=]() {
            sstring server_trust;
            if (ca != tls::client_auth::NONE) {
                server_trust = trust;
            }
            return server->invoke_on_all(&echoserver::listen, addr, crt, key, ca, server_trust);
        }).then([=] {
            return tls::connect(certs, addr, name).then([loops, msg, do_read](::connected_socket s) {
                auto strms = ::make_lw_shared<streams>(std::move(s));
                auto range = boost::irange(0, loops);
                return do_for_each(range, [strms, msg](auto) {
                    auto f = strms->out.write(*msg);
                    return f.then([strms, msg]() {
                        return strms->out.flush().then([strms, msg] {
                            return strms->in.read_exactly(msg->size()).then([msg](temporary_buffer<char> buf) {
                                if (buf.empty()) {
                                    throw std::runtime_error("Unexpected EOF");
                                }
                                sstring tmp(buf.begin(), buf.end());
                                BOOST_CHECK(*msg == tmp);
                            });
                        });
                    });
                }).then_wrapped([strms, do_read] (future<> f1) {
                    // Always call close()
                    return (do_read ? strms->out.close() : make_ready_future<>()).then_wrapped([strms, f1 = std::move(f1)] (future<> f2) mutable {
                        // Verification errors will be reported by the call to output_stream::close(),
                        // which waits for the flush to actually happen. They can also be reported by the
                        // input_stream::read_exactly() call. We want to keep only one and avoid nested exception mess.
                        if (f1.failed()) {
                            (void)f2.handle_exception([] (std::exception_ptr ignored) { });
                            return std::move(f1);
                        }
                        (void)f1.handle_exception([] (std::exception_ptr ignored) { });
                        return f2;
                    }).finally([strms] { });
                });
            });
        }).finally([server] {
            return server->stop().finally([server]{});
        });
    });
}

/*
 * Certificates:
 *
 * make -f tests/unit/mkcert.gmk domain=scylladb.org server=test
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
    return run_echo_test(message, 20, certfile("catest.pem"), "test.scylladb.org");
}

SEASTAR_TEST_CASE(test_simple_x509_client_server_again) {
    return run_echo_test(message, 20, certfile("catest.pem"), "test.scylladb.org");
}

#if GNUTLS_VERSION_NUMBER >= 0x030600
// Test #769 - do not set dh_params in server certs - let gnutls negotiate.
SEASTAR_TEST_CASE(test_simple_server_default_dhparams) {
    return run_echo_test(message, 20, certfile("catest.pem"), "test.scylladb.org",
        certfile("test.crt"), certfile("test.key"), tls::client_auth::NONE,
        {}, {}, true, /* use_dh_params */ false
    );
}
#endif

SEASTAR_TEST_CASE(test_x509_client_server_cert_validation_fail) {
    // Load a real trust authority here, which out certs are _not_ signed with.
    return run_echo_test(message, 1, certfile("tls-ca-bundle.pem"), {}).then([] {
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
    return run_echo_test(message, 1, certfile("tls-ca-bundle.pem"), "nils.holgersson.gov").then([] {
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

SEASTAR_TEST_CASE(test_large_message_x509_client_server) {
    // Make sure we load our own auth trust pem file, otherwise our certs
    // will not validate
    // Must match expected name with cert CA or give empty name to ignore
    // server name
    sstring msg = uninitialized_string(512 * 1024);
    for (size_t i = 0; i < msg.size(); ++i) {
        msg[i] = '0' + char(i % 30);
    }
    return run_echo_test(std::move(msg), 20, certfile("catest.pem"), "test.scylladb.org");
}

SEASTAR_TEST_CASE(test_simple_x509_client_server_fail_client_auth) {
    // Make sure we load our own auth trust pem file, otherwise our certs
    // will not validate
    // Must match expected name with cert CA or give empty name to ignore
    // server name
    // Server will require certificate auth. We supply none, so should fail connection
    return run_echo_test(message, 20, certfile("catest.pem"), "test.scylladb.org", certfile("test.crt"), certfile("test.key"), tls::client_auth::REQUIRE).then([] {
        BOOST_FAIL("Expected exception");
    }).handle_exception([](auto ep) {
        // ok.
    });
}

SEASTAR_TEST_CASE(test_simple_x509_client_server_client_auth) {
    // Make sure we load our own auth trust pem file, otherwise our certs
    // will not validate
    // Must match expected name with cert CA or give empty name to ignore
    // server name
    // Server will require certificate auth. We supply one, so should succeed with connection
    return run_echo_test(message, 20, certfile("catest.pem"), "test.scylladb.org", certfile("test.crt"), certfile("test.key"), tls::client_auth::REQUIRE, certfile("test.crt"), certfile("test.key"));
}

SEASTAR_TEST_CASE(test_simple_x509_client_server_client_auth_with_dn_callback) {
    // In addition to the above test, the certificate's subject and issuer
    // Distinguished Names (DNs) will be checked for the occurrence of a specific
    // substring (in this case, the test.scylladb.org url)
    return run_echo_test(message, 20, certfile("catest.pem"), "test.scylladb.org", certfile("test.crt"), certfile("test.key"), tls::client_auth::REQUIRE, certfile("test.crt"), certfile("test.key"), true, true, [](tls::session_type t, sstring subject, sstring issuer) {
        BOOST_REQUIRE(t == tls::session_type::CLIENT);
        BOOST_REQUIRE(subject.find("test.scylladb.org") != sstring::npos);
        BOOST_REQUIRE(issuer.find("test.scylladb.org") != sstring::npos);
    });
}

SEASTAR_TEST_CASE(test_simple_x509_client_server_client_auth_dn_callback_fails) {
    // Test throwing an exception from within the Distinguished Names callback
    return run_echo_test(message, 20, certfile("catest.pem"), "test.scylladb.org", certfile("test.crt"), certfile("test.key"), tls::client_auth::REQUIRE, certfile("test.crt"), certfile("test.key"), true, true, [](tls::session_type, sstring, sstring) {
        throw tls::verification_error("to test throwing from within the callback");
    }).then([] {
        BOOST_FAIL("Should have gotten a verification_error exception");
    }).handle_exception([](auto) {
        // ok.
    });
}

SEASTAR_TEST_CASE(test_many_large_message_x509_client_server) {
    // Make sure we load our own auth trust pem file, otherwise our certs
    // will not validate
    // Must match expected name with cert CA or give empty name to ignore
    // server name
    sstring msg = uninitialized_string(4 * 1024 * 1024);
    for (size_t i = 0; i < msg.size(); ++i) {
        msg[i] = '0' + char(i % 30);
    }
    // Sending a huge-ish message a and immediately closing the session (see params)
    // provokes case where tls::vec_push entered race and asserted on broken IO state
    // machine.
    auto range = boost::irange(0, 20);
    return do_for_each(range, [msg = std::move(msg)](auto) {
        return run_echo_test(std::move(msg), 1, certfile("catest.pem"), "test.scylladb.org", certfile("test.crt"), certfile("test.key"), tls::client_auth::NONE, {}, {}, false);
    });
}

SEASTAR_THREAD_TEST_CASE(test_close_timout) {
    tls::credentials_builder b;

    b.set_x509_key_file(certfile("test.crt"), certfile("test.key"), tls::x509_crt_format::PEM).get();
    b.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();
    b.set_dh_level();
    b.set_system_trust().get();

    auto creds = b.build_certificate_credentials();
    auto serv = b.build_server_credentials();

    semaphore sem(0);

    class my_loopback_connected_socket_impl : public loopback_connected_socket_impl {
    public:
        semaphore& _sem;
        bool _close = false;

        my_loopback_connected_socket_impl(semaphore& s, lw_shared_ptr<loopback_buffer> tx, lw_shared_ptr<loopback_buffer> rx)
            : loopback_connected_socket_impl(tx, rx)
            , _sem(s)
        {}
        ~my_loopback_connected_socket_impl() {
            _sem.signal();
        }
        class my_sink_impl : public data_sink_impl {
        public:
            data_sink _sink;
            my_loopback_connected_socket_impl& _impl;
            promise<> _p;
            my_sink_impl(data_sink sink, my_loopback_connected_socket_impl& impl)
                : _sink(std::move(sink))
                , _impl(impl)
            {}
            future<> flush() override {
                return _sink.flush();
            }
            using data_sink_impl::put;
            future<> put(net::packet p) override {
                if (std::exchange(_impl._close, false)) {
                    return _p.get_future().then([this, p = std::move(p)]() mutable {
                        return put(std::move(p));
                    });
                }
                return _sink.put(std::move(p));
            }
            future<> close() override {
                _p.set_value();
                return make_ready_future<>();
            }
        };
        data_sink sink() override {
            return data_sink(std::make_unique<my_sink_impl>(loopback_connected_socket_impl::sink(), *this));
        }
    };

    auto constexpr iterations = 500;
        
    for (int i = 0; i < iterations; ++i) {
        auto b1 = ::make_lw_shared<loopback_buffer>(nullptr, loopback_buffer::type::SERVER_TX);
        auto b2 = ::make_lw_shared<loopback_buffer>(nullptr, loopback_buffer::type::CLIENT_TX);
        auto ssi = std::make_unique<my_loopback_connected_socket_impl>(sem, b1, b2);
        auto csi = std::make_unique<my_loopback_connected_socket_impl>(sem, b2, b1);

        auto& ssir = *ssi;
        auto& csir = *csi;

        auto ss = tls::wrap_server(serv, connected_socket(std::move(ssi))).get0();
        auto cs = tls::wrap_client(creds, connected_socket(std::move(csi))).get0();

        auto os = cs.output().detach();
        auto is = ss.input();

        auto f1 = os.put(temporary_buffer<char>(10));
        auto f2 = is.read();
        f1.get();
        f2.get();
        
        // block further writes
        ssir._close = true;
        csir._close = true;
    }

    sem.wait(2 * iterations).get();
}

SEASTAR_THREAD_TEST_CASE(test_reload_certificates) {
    tmpdir tmp;

    namespace fs = std::filesystem;

    // copy the wrong certs. We don't trust these
    // blocking calls, but this is a test and seastar does not have a copy 
    // util and I am lazy...
    fs::copy_file(certfile("other.crt"), tmp.path() / "test.crt");
    fs::copy_file(certfile("other.key"), tmp.path() / "test.key");

    auto cert = (tmp.path() / "test.crt").native();
    auto key = (tmp.path() / "test.key").native();
    std::unordered_set<sstring> changed;
    promise<> p;

    tls::credentials_builder b;
    b.set_x509_key_file(cert, key, tls::x509_crt_format::PEM).get();
    b.set_dh_level();

    auto certs = b.build_reloadable_server_credentials([&](const std::unordered_set<sstring>& files, std::exception_ptr ep) {
        if (ep) {
            return;
        }
        changed.insert(files.begin(), files.end());
        if (changed.count(cert) && changed.count(key)) {
            p.set_value();
        }
    }).get0();

    ::listen_options opts;
    opts.reuse_address = true;
    auto addr = ::make_ipv4_address( {0x7f000001, 4712});
    auto server = tls::listen(certs, addr, opts);

    tls::credentials_builder b2;
    b2.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();

    {
        auto sa = server.accept();
        auto c = tls::connect(b2.build_certificate_credentials(), addr).get0();
        auto s = sa.get0();
        auto in = s.connection.input();

        output_stream<char> out(c.output().detach(), 4096);

        try {
            out.write("apa").get();
            auto f = out.flush();
            auto f2 = in.read();

            try {
                f.get();
                BOOST_FAIL("should not reach");
            } catch (tls::verification_error&) {
                // ok
            }
            try {
                out.close().get();
            } catch (...) {
            }

            try {
                f2.get();
                BOOST_FAIL("should not reach");
            } catch (...) {
                // ok
            }
            try {
                in.close().get();
            } catch (...) {
            }
        } catch (tls::verification_error&) {
            // ok
        }
    }

    // copy the right (trusted) certs over the old ones.
    fs::copy_file(certfile("test.crt"), tmp.path() / "test0.crt");
    fs::copy_file(certfile("test.key"), tmp.path() / "test0.key");

    rename_file((tmp.path() / "test0.crt").native(), (tmp.path() / "test.crt").native()).get();
    rename_file((tmp.path() / "test0.key").native(), (tmp.path() / "test.key").native()).get();

    p.get_future().get();

    // now it should work
    {
        auto sa = server.accept();
        auto c = tls::connect(b2.build_certificate_credentials(), addr).get0();
        auto s = sa.get0();
        auto in = s.connection.input();

        output_stream<char> out(c.output().detach(), 4096);

        out.write("apa").get();
        auto f = out.flush();
        auto buf = in.read().get0();
        f.get();
        out.close().get();
        in.read().get(); // ignore - just want eof
        in.close().get();

        BOOST_CHECK_EQUAL(sstring(buf.begin(), buf.end()), "apa");
    }
}

SEASTAR_THREAD_TEST_CASE(test_reload_broken_certificates) {
    tmpdir tmp;

    namespace fs = std::filesystem;

    fs::copy_file(certfile("test.crt"), tmp.path() / "test.crt");
    fs::copy_file(certfile("test.key"), tmp.path() / "test.key");

    auto cert = (tmp.path() / "test.crt").native();
    auto key = (tmp.path() / "test.key").native();
    std::unordered_set<sstring> changed;
    promise<> p;

    tls::credentials_builder b;
    b.set_x509_key_file(cert, key, tls::x509_crt_format::PEM).get();
    b.set_dh_level();

    queue<std::exception_ptr> q(10);

    auto certs = b.build_reloadable_server_credentials([&](const std::unordered_set<sstring>& files, std::exception_ptr ep) {
        if (ep) {
            q.push(std::move(ep));
            return;
        }
        changed.insert(files.begin(), files.end());
        if (changed.count(cert) && changed.count(key)) {
            p.set_value();
        }
    }).get0();

    // very intentionally use blocking calls. We want all our modifications to happen
    // before any other continuation is allowed to process.

    fs::remove(cert);
    fs::remove(key);

    std::ofstream(cert.c_str()) << "lala land" << std::endl;
    std::ofstream(key.c_str()) << "lala land" << std::endl;

    // should get one or two exceptions
    q.pop_eventually().get();

    fs::remove(cert);
    fs::remove(key);

    fs::copy_file(certfile("test.crt"), cert);
    fs::copy_file(certfile("test.key"), key);

    // now it should reload
    p.get_future().get();
}

using namespace std::chrono_literals;

// the same as previous test, but we set a big tolerance for 
// reload errors, and verify that either our scheduling/fs is 
// super slow, or we got through the changes without failures.
SEASTAR_THREAD_TEST_CASE(test_reload_tolerance) {
    tmpdir tmp;

    namespace fs = std::filesystem;

    fs::copy_file(certfile("test.crt"), tmp.path() / "test.crt");
    fs::copy_file(certfile("test.key"), tmp.path() / "test.key");

    auto cert = (tmp.path() / "test.crt").native();
    auto key = (tmp.path() / "test.key").native();
    std::unordered_set<sstring> changed;
    promise<> p;

    tls::credentials_builder b;
    b.set_x509_key_file(cert, key, tls::x509_crt_format::PEM).get();
    b.set_dh_level();

    int nfails = 0;

    // use 5s tolerance - this should ensure we don't generate any errors.
    auto certs = b.build_reloadable_server_credentials([&](const std::unordered_set<sstring>& files, std::exception_ptr ep) {
        if (ep) {
            ++nfails;
            return;
        }
        changed.insert(files.begin(), files.end());
        if (changed.count(cert) && changed.count(key)) {
            p.set_value();
        }
    }, std::chrono::milliseconds(5000)).get0();

    // very intentionally use blocking calls. We want all our modifications to happen
    // before any other continuation is allowed to process.

    auto start = std::chrono::system_clock::now();

    fs::remove(cert);
    fs::remove(key);

    std::ofstream(cert.c_str()) << "lala land" << std::endl;
    std::ofstream(key.c_str()) << "lala land" << std::endl;

    fs::remove(cert);
    fs::remove(key);

    fs::copy_file(certfile("test.crt"), cert);
    fs::copy_file(certfile("test.key"), key);

    // now it should reload
    p.get_future().get();

    auto end = std::chrono::system_clock::now();

    BOOST_ASSERT(nfails == 0 || (end - start) > 4s);
}

SEASTAR_THREAD_TEST_CASE(test_reload_by_move) {
    tmpdir tmp;
    tmpdir tmp2;

    namespace fs = std::filesystem;

    fs::copy_file(certfile("test.crt"), tmp.path() / "test.crt");
    fs::copy_file(certfile("test.key"), tmp.path() / "test.key");
    fs::copy_file(certfile("test.crt"), tmp2.path() / "test.crt");
    fs::copy_file(certfile("test.key"), tmp2.path() / "test.key");

    auto cert = (tmp.path() / "test.crt").native();
    auto key = (tmp.path() / "test.key").native();
    auto cert2 = (tmp2.path() / "test.crt").native();
    auto key2 = (tmp2.path() / "test.key").native();

    std::unordered_set<sstring> changed;
    promise<> p;

    tls::credentials_builder b;
    b.set_x509_key_file(cert, key, tls::x509_crt_format::PEM).get();
    b.set_dh_level();

    int nfails = 0;

    // use 5s tolerance - this should ensure we don't generate any errors.
    auto certs = b.build_reloadable_server_credentials([&](const std::unordered_set<sstring>& files, std::exception_ptr ep) {
        if (ep) {
            ++nfails;
            return;
        }
        changed.insert(files.begin(), files.end());
        if (changed.count(cert) && changed.count(key)) {
            p.set_value();
        }
    }, std::chrono::milliseconds(5000)).get0();

    // very intentionally use blocking calls. We want all our modifications to happen
    // before any other continuation is allowed to process.

    fs::remove(cert);
    fs::remove(key);

    // deletes should _not_ cause errors/reloads
    try {
        with_timeout(std::chrono::steady_clock::now() + 3s, p.get_future()).get();
        BOOST_FAIL("should not reach");
    } catch (timed_out_error&) {
        // ok
    }

    BOOST_REQUIRE_EQUAL(changed.size(), 0);

    p = promise();

    fs::rename(cert2, cert);
    fs::rename(key2, key);

    // now it should reload
    p.get_future().get();

    BOOST_REQUIRE_EQUAL(changed.size(), 2);
    changed.clear();

    // again, without delete

    fs::copy_file(certfile("test.crt"), tmp2.path() / "test.crt");
    fs::copy_file(certfile("test.key"), tmp2.path() / "test.key");

    p = promise();

    fs::rename(cert2, cert);
    fs::rename(key2, key);

    // it should reload here as well.
    p.get_future().get();

    // could get two notifications. but not more. 
    for (int i = 0;; ++i) {
        p = promise();
        try {
            with_timeout(std::chrono::steady_clock::now() + 3s, p.get_future()).get();
            BOOST_ASSERT(i == 0);
        } catch (timed_out_error&) {
            // ok
            break;
        }
    }
}

SEASTAR_THREAD_TEST_CASE(test_closed_write) {
    tls::credentials_builder b;

    b.set_x509_key_file(certfile("test.crt"), certfile("test.key"), tls::x509_crt_format::PEM).get();
    b.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();
    b.set_dh_level();
    b.set_system_trust().get();

    auto creds = b.build_certificate_credentials();
    auto serv = b.build_server_credentials();

    ::listen_options opts;
    opts.reuse_address = true;
    opts.set_fixed_cpu(this_shard_id());

    auto addr = ::make_ipv4_address( {0x7f000001, 4712});
    auto server = tls::listen(serv, addr, opts);

    auto check_same_message_two_writes = [](output_stream<char>& out) {
        std::exception_ptr ep1, ep2;

        try {
            out.write("apa").get();
            out.flush().get();
            BOOST_FAIL("should not reach");
        } catch (...) {
            // ok
            ep1 = std::current_exception();
        }

        try {
            out.write("apa").get();
            out.flush().get();
            BOOST_FAIL("should not reach");
        } catch (...) {
            // ok
            ep2 = std::current_exception();
        }

        try {
            std::rethrow_exception(ep1);
        } catch (std::exception& e1) {
            try {
                std::rethrow_exception(ep2);
            } catch (std::exception& e2) {
                BOOST_REQUIRE_EQUAL(std::string(e1.what()), std::string(e2.what()));
                return;
            }
        }

        BOOST_FAIL("should not reach");
    };


    {
        auto sa = server.accept();
        auto c = tls::connect(creds, addr).get0();
        auto s = sa.get0();
        auto in = s.connection.input();

        output_stream<char> out(c.output().detach(), 4096);
        // close on client end before writing
        out.close().get();

        check_same_message_two_writes(out);
    }

    {
        auto sa = server.accept();
        auto c = tls::connect(creds, addr).get0();
        auto s = sa.get0();
        auto in = s.connection.input();

        output_stream<char> out(c.output().detach(), 4096);

        out.write("apa").get();
        auto f = out.flush();
        in.read().get();
        f.get();

        // close on server end before writing
        in.close().get();
        s.connection.shutdown_input();
        s.connection.shutdown_output();

        // we won't get broken pipe until
        // after a while (tm)
        for (;;) {
            try {
                out.write("apa").get();
                out.flush().get();
            } catch (...) {
                break;
            }
        }

        // now check we get the same message.
        check_same_message_two_writes(out);
    }

}
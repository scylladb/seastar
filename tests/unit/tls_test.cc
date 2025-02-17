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

#include <ranges>
#include <iostream>

#include <seastar/core/do_with.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/thread.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/process.hh>
#include <seastar/net/tls.hh>
#include <seastar/net/dns.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/testing/test_case.hh>
#include <seastar/testing/thread_test_case.hh>
#include <seastar/util/defer.hh>

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

static future<> connect_to_ssl_addr(::shared_ptr<tls::certificate_credentials> certs, socket_address addr, const sstring& name = {}) {
    return repeat_until_value([=]() mutable {
        return tls::connect(certs, addr, tls::tls_options{.server_name = name}).then([](connected_socket s) {
            return do_with(std::move(s), [](connected_socket& s) {
                return do_with(s.output(), [&s](auto& os) {
                    static const sstring msg("GET / HTTP/1.0\r\n\r\n");
                    auto f = os.write(msg);
                    return f.then([&s, &os]() mutable {
                        auto f = os.flush();
                        return f.then([&s]() mutable {
                            return do_with(s.input(), sstring{}, [](auto& in, sstring& buffer) {
                                return do_until(std::bind(&input_stream<char>::eof, std::cref(in)), [&buffer, &in] {
                                    auto f = in.read();
                                    return f.then([&](temporary_buffer<char> buf) {
                                        buffer.append(buf.get(), buf.size());
                                    });
                                }).then([&buffer]() -> future<std::optional<bool>> {
                                    if (buffer.empty()) {
                                        // # 1127 google servers have a (pretty short) timeout between connect and expected first
                                        // write. If we are delayed inbetween connect and write above (cert verification, scheduling
                                        // solar spots or just time sharing on AWS) we could get a short read here. Just retry.
                                        // If we get an actual error, it is either on protocol level (exception) or HTTP error.
                                        return make_ready_future<std::optional<bool>>(std::nullopt);
                                    }
                                    BOOST_CHECK(buffer.size() > 8);
                                    BOOST_CHECK_EQUAL(buffer.substr(0, 5), sstring("HTTP/"));
                                    return make_ready_future<std::optional<bool>>(true);
                                });
                            });
                        });
                    }).finally([&os] {
                        return os.close();
                    });
                });
            });
        });

    }).discard_result();
}

#if SEASTAR_TESTING_WITH_NETWORKING

static const auto google_name = "www.google.com";

// broken out from below. to allow pre-lookup
static future<socket_address> google_address() {
    static socket_address google;

    if (google.is_unspecified()) {
        return net::dns::resolve_name(google_name, net::inet_address::family::INET).then([](net::inet_address addr) {
            google = socket_address(addr, 443);
            return google_address();
        });
    }
    return make_ready_future<socket_address>(google);
}

static future<> connect_to_ssl_google(::shared_ptr<tls::certificate_credentials> certs) {
    return google_address().then([certs](socket_address addr) {
        return connect_to_ssl_addr(std::move(certs), addr, google_name);
    });
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
    // avoid getting parallel connects stuck on dns lookup (if running single case).
    // pre-lookup www.google.com
    return google_address().then([](socket_address) {
        tls::credentials_builder b;
        (void)b.set_system_trust();
        auto creds = b.build_certificate_credentials();

        return parallel_for_each(std::views::iota(0, 20), [creds](auto i) { return connect_to_ssl_google(creds); });
    });
}

SEASTAR_TEST_CASE(test_x509_client_with_system_trust_and_priority_strings) {
    static std::vector<sstring> prios( {
        "NORMAL:+ARCFOUR-128", // means normal ciphers plus ARCFOUR-128.
        "SECURE128:-VERS-SSL3.0:+COMP-DEFLATE", // means that only secure ciphers are enabled, SSL3.0 is disabled, and libz compression enabled.
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

SEASTAR_TEST_CASE(test_x509_client_with_system_trust_and_priority_strings_fail) {
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
#endif // SEASTAR_TESTING_WITH_NETWORKING

class https_server {
    const sstring _cert;
    const std::string _addr = "127.0.0.1";
    experimental::process _process;
    uint16_t _port;

    static experimental::process spawn(const std::string& addr, const sstring& key, const sstring& cert) {
        auto httpd = boost::dll::program_location().parent_path() / "https-server.py";
        const std::vector<sstring> argv{
          "httpd",
          "--server", fmt::format("{}:{}", addr, 0),
          "--key", key,
          "--cert", cert,
        };
        return experimental::spawn_process(httpd.string(), {.argv = argv}).get();
    }

    // https-server.py picks an available port and listens on it. when it is
    // ready to serve, it prints out the listening port. without hardwiring to
    // a fixed port, we are able to run multiple tests in parallel.
    static uint16_t read_port(experimental::process& process) {
        using consumption_result_type = typename input_stream<char>::consumption_result_type;
        using stop_consuming_type = typename consumption_result_type::stop_consuming_type;
        using tmp_buf = stop_consuming_type::tmp_buf;
        struct consumer {
            future<consumption_result_type> operator()(tmp_buf buf) {
                if (auto newline = std::find(buf.begin(), buf.end(), '\n'); newline != buf.end()) {
                    size_t consumed = newline - buf.begin();
                    line += std::string_view(buf.get(), consumed);
                    buf.trim_front(consumed);
                    return make_ready_future<consumption_result_type>(stop_consuming_type(std::move(buf)));
                } else {
                    line += std::string_view(buf.get(), buf.size());
                    return make_ready_future<consumption_result_type>(stop_consuming_type({}));
                }
            }
            std::string line;
        };
        auto reader = ::make_shared<consumer>();
        process.cout().consume(*reader).get();
        return std::stoul(reader->line);
    }

public:
    https_server(const std::string& ca = "mtls_ca")
        : _cert(certfile(fmt::format("{}.crt", ca)))
        , _process(spawn(_addr, certfile(fmt::format("{}.key", ca)), _cert))
        , _port(read_port(_process))
    {}
    ~https_server() {
        _process.terminate();
        _process.wait().discard_result().get();
    }
    const sstring& cert() const {
        return _cert;
    }
    socket_address addr() const {
        return ipv4_addr(_addr, _port);
    }
    sstring name() const {
        // should be identical to the one passed as the "-addext" option when
        // generating the cert.
        return "127.0.0.1";
    }
};

#if !SEASTAR_TESTING_WITH_NETWORKING

SEASTAR_THREAD_TEST_CASE(test_simple_x509_client) {
    auto certs = ::make_shared<tls::certificate_credentials>();
    https_server server;
    certs->set_x509_trust_file(server.cert(), tls::x509_crt_format::PEM).get();
    connect_to_ssl_addr(certs, server.addr(), server.name()).get();
}

#endif // !SEASTAR_TESTING_WITH_NETWORKING

SEASTAR_THREAD_TEST_CASE(test_x509_client_with_builder) {
    tls::credentials_builder b;
    https_server server;
    b.set_x509_trust_file(server.cert(), tls::x509_crt_format::PEM).get();
    connect_to_ssl_addr(b.build_certificate_credentials(), server.addr()).get();
}

SEASTAR_THREAD_TEST_CASE(test_x509_client_with_builder_multiple) {
    tls::credentials_builder b;
    https_server server;
    b.set_x509_trust_file(server.cert(), tls::x509_crt_format::PEM).get();
    auto creds = b.build_certificate_credentials();
    auto addr = server.addr();
    parallel_for_each(std::views::iota(0, 20), [creds, addr](auto i) {
        return connect_to_ssl_addr(creds, addr);
    }).get();
}

SEASTAR_THREAD_TEST_CASE(test_x509_client_with_priority_strings) {
    static std::vector<sstring> prios( {
        "NORMAL:+ARCFOUR-128", // means normal ciphers plus ARCFOUR-128.
        "SECURE128:-VERS-SSL3.0:+COMP-DEFLATE", // means that only secure ciphers are enabled, SSL3.0 is disabled, and libz compression enabled.
        "SECURE256:+SECURE128",
        "NORMAL:%COMPAT",
        "NORMAL:-MD5",
        "NONE:+VERS-TLS-ALL:+MAC-ALL:+RSA:+AES-256-GCM:+SIGN-ALL:+COMP-NULL:+GROUP-EC-ALL",
        "NORMAL:+ARCFOUR-128",
        "SECURE128:-VERS-TLS1.0:+COMP-DEFLATE",
        "SECURE128:+SECURE192:-VERS-TLS-ALL:+VERS-TLS1.2"
    });
    tls::credentials_builder b;
    https_server server;
    b.set_x509_trust_file(server.cert(), tls::x509_crt_format::PEM).get();
    auto addr = server.addr();
    do_for_each(prios, [&b, addr](const sstring& prio) {
        b.set_priority_string(prio);
        return connect_to_ssl_addr(b.build_certificate_credentials(), addr);
    }).get();
}

SEASTAR_THREAD_TEST_CASE(test_x509_client_with_priority_strings_fail) {
    static std::vector<sstring> prios( { "NONE",
        "NONE:+CURVE-SECP256R1"
    });
    tls::credentials_builder b;
    https_server server;
    b.set_x509_trust_file(server.cert(), tls::x509_crt_format::PEM).get();
    auto addr = server.addr();
    do_for_each(prios, [&b, addr](const sstring& prio) {
        b.set_priority_string(prio);
        try {
            return connect_to_ssl_addr(b.build_certificate_credentials(), addr).then([] {
                BOOST_FAIL("Expected exception");
            }).handle_exception([](auto ep) {
                // ok.
            });
        } catch (...) {
            // also ok
        }
        return make_ready_future<>();
    }).get();
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

        auto c = tls::connect(b.build_certificate_credentials(), addr).get();
        auto s = sa.get();
        server.abort_accept(); // should not affect the socket we got.
        auto out = c.output();
        auto in = s.connection.input();

        out.write("apa").get();
        auto f = out.flush();
        auto buf = in.read().get();
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
            auto c = f.get();
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

    SEASTAR_ASSERT(do_read || loops == 1);

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
            return tls::connect(certs, addr, tls::tls_options{.server_name=name}).then([loops, msg, do_read](::connected_socket s) {
                auto strms = ::make_lw_shared<streams>(std::move(s));
                auto range = std::views::iota(0, loops);
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
    // Load a real trust authority here, which our certs are _not_ signed with.
    return run_echo_test(message, 1, certfile("tls-ca-bundle.pem"), {}).then([] {
            BOOST_FAIL("Should have gotten validation error");
    }).handle_exception([](auto ep) {
        try {
            std::rethrow_exception(ep);
        } catch (tls::verification_error& e) {
            // Verify exception contains info on subject/issuer
            BOOST_REQUIRE_NE(sstring(e.what()).find("Issuer"), sstring::npos);
            BOOST_REQUIRE_NE(sstring(e.what()).find("Subject"), sstring::npos);
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
    auto range = std::views::iota(0, 20);
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

        auto ss = tls::wrap_server(serv, connected_socket(std::move(ssi))).get();
        auto cs = tls::wrap_client(creds, connected_socket(std::move(csi))).get();

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
    }).get();

    ::listen_options opts;
    opts.reuse_address = true;
    auto addr = ::make_ipv4_address( {0x7f000001, 4712});
    auto server = tls::listen(certs, addr, opts);

    tls::credentials_builder b2;
    b2.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();

    {
        auto sa = server.accept();
        auto c = tls::connect(b2.build_certificate_credentials(), addr).get();
        auto s = sa.get();
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
        auto c = tls::connect(b2.build_certificate_credentials(), addr).get();
        auto s = sa.get();
        auto in = s.connection.input();

        output_stream<char> out(c.output().detach(), 4096);

        out.write("apa").get();
        auto f = out.flush();
        auto buf = in.read().get();
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
    }).get();

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
    }, std::chrono::milliseconds(5000)).get();

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

    SEASTAR_ASSERT(nfails == 0 || (end - start) > 4s);
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
    }, std::chrono::milliseconds(5000)).get();

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
            SEASTAR_ASSERT(i == 0);
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
    b.set_client_auth(tls::client_auth::REQUIRE);

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
        auto c = tls::connect(creds, addr).get();
        auto s = sa.get();
        auto in = s.connection.input();

        output_stream<char> out(c.output().detach(), 4096);
        // close on client end before writing
        out.close().get();

        check_same_message_two_writes(out);
    }

    {
        auto sa = server.accept();
        auto c = tls::connect(creds, addr).get();
        auto s = sa.get();
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

/*
 * Certificates:
 *
 * make -f tests/unit/mkmtls.gmk domain=scylladb.org server=test
 *
 * ->   mtls_ca.crt
 *      mtls_ca.key
 *      mtls_server.crt
 *      mtls_server.csr
 *      mtls_server.key
 *      mtls_client1.crt
 *      mtls_client1.csr
 *      mtls_client1.key
 *      mtls_client2.crt
 *      mtls_client2.csr
 *      mtls_client2.key
 *
 */
SEASTAR_THREAD_TEST_CASE(test_dn_name_handling) {
    // Connect to server using two different client certificates.
    // Make sure that for every client the server can fetch DN string
    // and the string is correct.
    // The setup consist of several certificates:
    // - CA
    // - mtls_server.crt - server certificate
    // - mtls_client1.crt - first client certificate
    // - mtls_client2.crt - second client certificate
    //
    // The test runs server that uses mtls_server.crt.
    // The server accepts two incomming connections, first one uses mtls_client1.crt
    // and the second one uses mtls_client2.crt. Every client sends a short string
    // that server receives and tries to find it in the DN string.

    auto addr = ::make_ipv4_address( {0x7f000001, 4712});

    auto client1_creds = [] {
        tls::credentials_builder builder;
        builder.set_x509_trust_file(certfile("mtls_ca.crt"), tls::x509_crt_format::PEM).get();
        builder.set_x509_key_file(certfile("mtls_client1.crt"), certfile("mtls_client1.key"), tls::x509_crt_format::PEM).get();
        return builder.build_certificate_credentials();
    }();

    auto client2_creds = [] {
        tls::credentials_builder builder;
        builder.set_x509_trust_file(certfile("mtls_ca.crt"), tls::x509_crt_format::PEM).get();
        builder.set_x509_key_file(certfile("mtls_client2.crt"), certfile("mtls_client2.key"), tls::x509_crt_format::PEM).get();
        return builder.build_certificate_credentials();
    }();

    auto server_creds = [] {
        tls::credentials_builder builder;
        builder.set_x509_trust_file(certfile("mtls_ca.crt"), tls::x509_crt_format::PEM).get();
        builder.set_x509_key_file(certfile("mtls_server.crt"), certfile("mtls_server.key"), tls::x509_crt_format::PEM).get();
        builder.set_client_auth(tls::client_auth::REQUIRE);
        return builder.build_server_credentials();
    }();

    auto fetch_dn = [server_creds, addr] (sstring id, shared_ptr<tls::certificate_credentials> client_cred) {
        listen_options lo{};
        lo.reuse_address = true;
        auto server_sock = tls::listen(server_creds, addr, lo);

        auto sa = server_sock.accept();
        auto c = tls::connect(client_cred, addr).get();
        auto s = sa.get();

        auto in = s.connection.input();
        output_stream<char> out(c.output().detach(), 1024);
        out.write(id).get();

        auto fdn = tls::get_dn_information(s.connection);

        auto fout = out.flush();
        auto fin = in.read();

        fout.get();

        auto dn = fdn.get();
        auto client_id = fin.get();

        in.close().get();
        out.close().get();

        s.connection.shutdown_input();
        s.connection.shutdown_output();

        c.shutdown_input();
        c.shutdown_output();

        auto it = dn->subject.find(sstring(client_id.get(), client_id.size()));
        BOOST_REQUIRE(it != sstring::npos);
    };

    fetch_dn("client1.org", client1_creds);
    fetch_dn("client2.org", client2_creds);
}

SEASTAR_THREAD_TEST_CASE(test_alt_names) {
    tls::credentials_builder b;

    b.set_x509_key_file(certfile("test.crt"), certfile("test.key"), tls::x509_crt_format::PEM).get();
    b.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();
    b.set_client_auth(tls::client_auth::REQUIRE);

    auto creds = b.build_certificate_credentials();
    auto serv = b.build_server_credentials();

    ::listen_options opts;
    opts.reuse_address = true;
    opts.set_fixed_cpu(this_shard_id());

    auto addr = ::make_ipv4_address( {0x7f000001, 4712});
    auto server = tls::listen(serv, addr, opts);

    {
        auto sa = server.accept();
        auto c = tls::connect(creds, addr).get();
        auto s = sa.get();

        auto in = s.connection.input();
        output_stream<char> out(c.output().detach(), 1024);
        out.write("nils").get();

        auto falt_names = tls::get_alt_name_information(s.connection);

        auto fout = out.flush();
        auto fin = in.read();

        fout.get();

        auto alt_names = falt_names.get();
        fin.get();

        in.close().get();
        out.close().get();

        s.connection.shutdown_input();
        s.connection.shutdown_output();

        c.shutdown_input();
        c.shutdown_output();

        auto ensure_alt_name = [&](tls::subject_alt_name_type type, size_t min_count) {
            for (auto& v : alt_names) {
                if (type != v.type) {
                    continue;
                }
                std::visit([&](auto& val) {
                    BOOST_TEST_MESSAGE(fmt::format("Alt name type: {}: {}", int(type), val).c_str());
                }, v.value);
                if (--min_count == 0) {
                    return;
                }
            }
            BOOST_FAIL("Missing " + std::to_string(min_count) + " alt name attributes of type " + std::to_string(int(type)));
        };

        ensure_alt_name(tls::subject_alt_name_type::ipaddress, 1);
        ensure_alt_name(tls::subject_alt_name_type::rfc822name, 2);
        ensure_alt_name(tls::subject_alt_name_type::dnsname, 1);
    }

}

SEASTAR_THREAD_TEST_CASE(test_peer_certificate_chain_handling) {
    tls::credentials_builder b;

    b.set_x509_key_file(certfile("test.crt"), certfile("test.key"), tls::x509_crt_format::PEM).get();
    b.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();
    b.set_client_auth(tls::client_auth::REQUIRE);

    auto creds = b.build_certificate_credentials();
    auto serv = b.build_server_credentials();

    ::listen_options opts;
    opts.reuse_address = true;
    opts.set_fixed_cpu(this_shard_id());

    auto addr = ::make_ipv4_address( {0x7f000001, 4712});
    auto server = tls::listen(serv, addr, opts);

    {
        auto sa = server.accept();
        auto c = tls::connect(creds, addr).get();
        auto s = sa.get();

        auto in = s.connection.input();
        output_stream<char> out(c.output().detach(), 1024);
        out.write("nils").get();

        auto fscrts = tls::get_peer_certificate_chain(s.connection);
        auto fccrts = tls::get_peer_certificate_chain(c);

        auto fout = out.flush();
        auto fin = in.read();

        fout.get();

        auto scrts = fscrts.get();
        auto ccrts = fccrts.get();
        fin.get();

        in.close().get();
        out.close().get();

        s.connection.shutdown_input();
        s.connection.shutdown_output();

        c.shutdown_input();
        c.shutdown_output();

        auto read_file = [](std::filesystem::path const& path) {
            auto contents = tls::certificate_data(std::filesystem::file_size(path));
            std::ifstream{path, std::ios_base::binary}.read(reinterpret_cast<char *>(contents.data()), contents.size());
            return contents;
        };

        auto ders = {read_file(certfile("test.crt.der"))};

        BOOST_REQUIRE(std::ranges::equal(scrts, ders));
        BOOST_REQUIRE(std::ranges::equal(ccrts, ders));
    }
}

SEASTAR_THREAD_TEST_CASE(test_skip_wait_for_eof) {
    tls::credentials_builder b;

    b.set_x509_key_file(certfile("test.crt"), certfile("test.key"), tls::x509_crt_format::PEM).get();
    b.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();
    b.set_client_auth(tls::client_auth::REQUIRE);

    auto creds = b.build_certificate_credentials();
    auto serv = b.build_server_credentials();

    ::listen_options opts;
    opts.reuse_address = true;
    opts.set_fixed_cpu(this_shard_id());

    auto addr = ::make_ipv4_address({0x7f000001, 4712});
    auto server = tls::listen(serv, addr, opts);

    {
        // Initiate a connection while specifying that it should not wait for eof on shutdown.
        auto sa = server.accept();
        auto c = engine().connect(addr).get();
        auto c_tls = tls::wrap_client(creds, std::move(c),
                                      tls::tls_options{.wait_for_eof_on_shutdown = false}).get();
        auto s = sa.get();

        auto in = s.connection.input();
        auto out = c_tls.output();

        // Write some data in the socket to handshake.
        out.write("apa").get();
        auto f = out.flush();
        auto buf = in.read().get();
        f.get();
        BOOST_CHECK(sstring(buf.begin(), buf.end()) == "apa");

        // Prevent the server from reading from the connection.
        // This ensures that it will miss the bye message and not
        // reply with an eof.
        server.abort_accept();

        // Initiate closing of the TLS session
        c_tls.shutdown_input();
        c_tls.shutdown_output();

        // Ensure that the session is closed promptly. When wait_for_eof_on_shutdown is not
        // specified, the call to wait_input_shutdown will hang for 10 seconds waiting for
        // and eof from the server.
        try {
            with_timeout(std::chrono::steady_clock::now() + 1s, c_tls.wait_input_shutdown()).get();
        } catch (timed_out_error&) {
            BOOST_FAIL("Timed out while waiting for input shutdown."
                       "This indicates the EOF wait was not skipped");
        }
    }
}

/**
 * Test TLS13 session ticket support.
*/
SEASTAR_THREAD_TEST_CASE(test_tls13_session_tickets) {
    tls::credentials_builder b;

    b.set_x509_key_file(certfile("test.crt"), certfile("test.key"), tls::x509_crt_format::PEM).get();
    b.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();
    b.set_session_resume_mode(tls::session_resume_mode::TLS13_SESSION_TICKET);
    b.set_priority_string("SECURE128:+SECURE192:-VERS-TLS-ALL:+VERS-TLS1.3");

    auto creds = b.build_certificate_credentials();
    auto serv = b.build_server_credentials();

    ::listen_options opts;
    opts.reuse_address = true;
    opts.set_fixed_cpu(this_shard_id());

    auto addr = ::make_ipv4_address( {0x7f000001, 4712});
    auto server = tls::listen(serv, addr, opts);

    tls::session_data sess_data;

    {
        auto sa = server.accept();
        auto c = tls::connect(creds, addr).get();
        auto s = sa.get();

        auto in = s.connection.input();
        auto cin = c.input();
        output_stream<char> out(c.output().detach(), 1024);
        output_stream<char> sout(s.connection.output().detach(), 1024);

        // write data in both directions. Required for session data to
        // become available.
        out.write("nils").get();
        auto fin = in.read();
        auto fout = out.flush();

        fout.get();
        fin.get();

        sout.write("banan").get();
        fin = cin.read();
        fout = sout.flush();

        fout.get();
        fin.get();

        BOOST_REQUIRE(!tls::check_session_is_resumed(c).get()); // no resume data

        // get ticket data
        sess_data = tls::get_session_resume_data(c).get();
        BOOST_REQUIRE(!sess_data.empty());

        in.close().get();
        out.close().get();

        s.connection.shutdown_input();
        s.connection.shutdown_output();

        c.shutdown_input();
        c.shutdown_output();
    }


    {
        auto sa = server.accept();

        // tell client to try resuming.
        tls::tls_options tls_opts;
        tls_opts.session_resume_data = sess_data;

        auto c = tls::connect(creds, addr, tls_opts).get();
        auto s = sa.get();

        // This is ok. Will force a handshake.
        auto f = tls::check_session_is_resumed(c);

        // But we need to force some IO to make the
        // handshake actually happen.
        auto in = s.connection.input();
        output_stream<char> out(c.output().detach(), 1024);

        auto fin = in.read();

        out.write("nils").get();
        auto fout = out.flush();

        fout.get();
        fin.get();

        BOOST_REQUIRE(f.get()); // Should work

        in.close().get();
        out.close().get();

        s.connection.shutdown_input();
        s.connection.shutdown_output();

        c.shutdown_input();
        c.shutdown_output();
    }

}

SEASTAR_THREAD_TEST_CASE(test_reload_certificates_with_only_shard0_notify) {
    tmpdir tmp;

    namespace fs = std::filesystem;

    // copy the wrong certs. We don't trust these
    // blocking calls, but this is a test and seastar does not have a copy 
    // util and I am lazy...
    fs::copy_file(certfile("other.crt"), tmp.path() / "test.crt");
    fs::copy_file(certfile("other.key"), tmp.path() / "test.key");

    auto cert = (tmp.path() / "test.crt").native();
    auto key = (tmp.path() / "test.key").native();
    promise<> p;

    tls::credentials_builder b;
    b.set_x509_key_file(cert, key, tls::x509_crt_format::PEM).get();
    b.set_dh_level();

    auto certs = b.build_server_credentials();

    auto shard_1_certs = smp::submit_to(1, [&]() -> future<shared_ptr<tls::server_credentials>> {
        co_return co_await b.build_reloadable_server_credentials([&, changed = std::unordered_set<sstring>{}](const tls::credentials_builder& builder, const std::unordered_set<sstring>& files, std::exception_ptr ep) mutable -> future<> {
            if (ep) {
                co_return;
            }
            changed.insert(files.begin(), files.end());
            if (changed.count(cert) && changed.count(key)) {
                // shard one certs are not reloadable. We issue a reload of them from shard 0
                // - to save inotify instances.
                co_await smp::submit_to(0, [&] {
                    builder.rebuild(*certs);
                    p.set_value();
                });
            }
        });
    }).get();

    auto def = defer([&]() noexcept {
        try {
            smp::submit_to(1, [&] {
                shard_1_certs = nullptr;
            }).get();
        } catch (...) {}
    });

    ::listen_options opts;
    opts.reuse_address = true;
    auto addr = ::make_ipv4_address( {0x7f000001, 4712});
    auto server = tls::listen(certs, addr, opts);

    tls::credentials_builder b2;
    b2.set_x509_trust_file(certfile("catest.pem"), tls::x509_crt_format::PEM).get();

    {
        auto sa = server.accept();
        auto c = tls::connect(b2.build_certificate_credentials(), addr).get();
        auto s = sa.get();
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
        auto c = tls::connect(b2.build_certificate_credentials(), addr).get();
        auto s = sa.get();
        auto in = s.connection.input();

        output_stream<char> out(c.output().detach(), 4096);

        out.write("apa").get();
        auto f = out.flush();
        auto buf = in.read().get();
        f.get();
        out.close().get();
        in.read().get(); // ignore - just want eof
        in.close().get();

        BOOST_CHECK_EQUAL(sstring(buf.begin(), buf.end()), "apa");
    }
}

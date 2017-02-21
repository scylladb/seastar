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

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include <experimental/optional>
#include <system_error>

#include "core/reactor.hh"
#include "core/thread.hh"
#include "core/sstring.hh"
#include "core/semaphore.hh"
#include "core/timer.hh"
#include "tls.hh"
#include "stack.hh"

class net::get_impl {
public:
    static std::unique_ptr<connected_socket_impl> get(connected_socket s) {
        return std::move(s._csi);
    }
};

class blob_wrapper: public gnutls_datum_t {
public:
    blob_wrapper(const seastar::tls::blob& in)
            : gnutls_datum_t {
                    reinterpret_cast<uint8_t *>(const_cast<char *>(in.data())),
                    unsigned(in.size()) } {
    }
};

class gnutlsinit {
public:
    gnutlsinit() {
        gnutls_global_init();
    }
    ~gnutlsinit() {
        gnutls_global_deinit();
    }
};

// Helper to ensure gnutls legacy init
// is handled properly with regards to
// object life spans. Could be better,
// this version will not destroy the
// gnutls stack until process exit.
class gnutlsobj {
public:
    gnutlsobj() {
        static gnutlsinit init;
    }
};

// Helper
static future<temporary_buffer<char>> read_fully(const sstring& name, const sstring& what) {
    return open_file_dma(name, open_flags::ro).then([](file f) {
        return do_with(std::move(f), [](file& f) {
            return f.size().then([&f](uint64_t size) {
                return f.dma_read_bulk<char>(0, size);
            }).finally([&f]() {
                return f.close();
            });
        });
    }).handle_exception([name, what](std::exception_ptr ep) -> future<temporary_buffer<char>> {
       try {
           std::rethrow_exception(std::move(ep));
       } catch (...) {
           std::throw_with_nested(std::runtime_error(sstring("Could not read ") + what + " " + name));
       }
    });
}

// Note: we are not using gnutls++ interfaces, mainly because we
// want to keep _our_ interface reasonably non-gnutls (well...)
// and once we get to this level, their abstractions don't help
// that much anyway. And they are sooo c++98...
class gnutls_error_category : public std::error_category {
public:
    constexpr gnutls_error_category() noexcept : std::error_category{} {}
    const char * name() const noexcept {
        return "GnuTLS";
    }
    std::string message(int error) const {
        return gnutls_strerror(error);
    }
};

static const gnutls_error_category glts_errorc;

// Checks a gnutls return value.
// < 0 -> error.
static void gtls_chk(int res) {
    if (res < 0) {
        throw std::system_error(res, glts_errorc);
    }
}

class seastar::tls::dh_params::impl : gnutlsobj {
    static gnutls_sec_param_t to_gnutls_level(level l) {
        switch (l) {
            case level::LEGACY: return GNUTLS_SEC_PARAM_LEGACY;
#if GNUTLS_VERSION_NUMBER >= 0x030300
            case level::MEDIUM: return GNUTLS_SEC_PARAM_MEDIUM;
#else
            case level::MEDIUM: return GNUTLS_SEC_PARAM_NORMAL;
#endif
            case level::HIGH: return GNUTLS_SEC_PARAM_HIGH;
            case level::ULTRA: return GNUTLS_SEC_PARAM_ULTRA;
            default:
                throw std::runtime_error(sprint("Unknown value of dh_params::level: %d", static_cast<std::underlying_type_t<level>>(l)));
        }
    }
public:
    impl()
            : _params([] {
                gnutls_dh_params_t params;
                gtls_chk(gnutls_dh_params_init(&params));
                return params;
            }()) {
    }
    impl(level lvl)
            : impl() {
        auto bits = gnutls_sec_param_to_pk_bits(GNUTLS_PK_DH, to_gnutls_level(lvl));
        gtls_chk(gnutls_dh_params_generate2(*this, bits));
    }
    impl(const blob& pkcs3, x509_crt_format fmt)
            : impl() {
        blob_wrapper w(pkcs3);
        gtls_chk(
                gnutls_dh_params_import_pkcs3(*this, &w,
                        gnutls_x509_crt_fmt_t(fmt)));
    }
    impl(const impl& v)
            : _params([&v] {
                gnutls_dh_params_t params;
                gtls_chk(gnutls_dh_params_init(&params));
                gtls_chk(gnutls_dh_params_cpy(params, v._params));
                return params;
            }()) {
    }
    ~impl() {
        if (_params != nullptr) {
            gnutls_dh_params_deinit(_params);
        }
    }
    operator gnutls_dh_params_t() const {
        return _params;
    }
private:
    gnutls_dh_params_t _params;
};

seastar::tls::dh_params::dh_params(level lvl) : _impl(std::make_unique<impl>(lvl))
{}

seastar::tls::dh_params::dh_params(const blob& b, x509_crt_format fmt)
        : _impl(std::make_unique<impl>(b, fmt)) {
}

seastar::tls::dh_params::~dh_params() {
}

seastar::tls::dh_params::dh_params(dh_params&&) noexcept = default;
seastar::tls::dh_params& seastar::tls::dh_params::operator=(dh_params&&) noexcept = default;

future<seastar::tls::dh_params> seastar::tls::dh_params::from_file(
        const sstring& filename, x509_crt_format fmt) {
    return read_fully(filename, "dh parameters").then([fmt](temporary_buffer<char> buf) {
        return make_ready_future<dh_params>(dh_params(blob(buf.get()), fmt));
    });
}

class seastar::tls::x509_cert::impl : gnutlsobj {
public:
    impl()
            : _cert([] {
                gnutls_x509_crt_t cert;
                gtls_chk(gnutls_x509_crt_init(&cert));
                return cert;
            }()) {
    }
    impl(const blob& b, x509_crt_format fmt)
        : impl()
    {
        blob_wrapper w(b);
        gtls_chk(gnutls_x509_crt_import(*this, &w, gnutls_x509_crt_fmt_t(fmt)));
    }
    ~impl() {
        if (_cert != nullptr) {
            gnutls_x509_crt_deinit(_cert);
        }
    }
    operator gnutls_x509_crt_t() const {
        return _cert;
    }

private:
    gnutls_x509_crt_t _cert;
};

seastar::tls::x509_cert::x509_cert(::shared_ptr<impl> impl)
        : _impl(std::move(impl)) {
}

seastar::tls::x509_cert::x509_cert(const blob& b, x509_crt_format fmt)
        : x509_cert(::make_shared<impl>(b, fmt)) {
}

future<seastar::tls::x509_cert> seastar::tls::x509_cert::from_file(
        const sstring& filename, x509_crt_format fmt) {
    return read_fully(filename, "x509 certificate").then([fmt](temporary_buffer<char> buf) {
        return make_ready_future<x509_cert>(x509_cert(blob(buf.get()), fmt));
    });
}

class seastar::tls::certificate_credentials::impl: public gnutlsobj {
public:
    impl()
            : _creds([] {
                gnutls_certificate_credentials_t xcred;
                gnutls_certificate_allocate_credentials(&xcred);
                if (xcred == nullptr) {
                    throw std::bad_alloc();
                }
                return xcred;
            }()), _priority(nullptr, &gnutls_priority_deinit)
    {}
    ~impl() {
        if (_creds != nullptr) {
            gnutls_certificate_free_credentials (_creds);
        }
    }

    operator gnutls_certificate_credentials_t() const {
        return _creds;
    }

    void set_x509_trust(const blob& b, x509_crt_format fmt) {
        blob_wrapper w(b);
        gtls_chk(
                gnutls_certificate_set_x509_trust_mem(_creds, &w,
                        gnutls_x509_crt_fmt_t(fmt)));
    }
    void set_x509_crl(const blob& b, x509_crt_format fmt) {
        blob_wrapper w(b);
        gtls_chk(
                gnutls_certificate_set_x509_crl_mem(_creds, &w,
                        gnutls_x509_crt_fmt_t(fmt)));
    }
    void set_x509_key(const blob& cert, const blob& key, x509_crt_format fmt) {
        blob_wrapper w1(cert);
        blob_wrapper w2(key);
        gtls_chk(
                gnutls_certificate_set_x509_key_mem(_creds, &w1, &w2,
                        gnutls_x509_crt_fmt_t(fmt)));
    }
    void set_simple_pkcs12(const blob& b, x509_crt_format fmt,
            const sstring& password) {
        blob_wrapper w(b);
        gtls_chk(
                gnutls_certificate_set_x509_simple_pkcs12_mem(_creds, &w,
                        gnutls_x509_crt_fmt_t(fmt), password.c_str()));
    }
    void dh_params(const tls::dh_params& dh) {
        auto cpy = std::make_unique<tls::dh_params::impl>(*dh._impl);
        gnutls_certificate_set_dh_params(*this, *cpy);
        _dh_params = std::move(cpy);
    }
    future<> set_system_trust() {
        return seastar::async([this] {
            gtls_chk(gnutls_certificate_set_x509_system_trust(_creds));
            _load_system_trust = false; // should only do once, for whatever reason
        });
    }
    void set_client_auth(client_auth ca) {
        _client_auth = ca;
    }
    client_auth get_client_auth() const {
        return _client_auth;
    }
    void set_priority_string(const sstring& prio) {
        const char * err = prio.c_str();
        try {
            gnutls_priority_t p;
            gtls_chk(gnutls_priority_init(&p, prio.c_str(), &err));
            _priority.reset(p);
        } catch (...) {
            std::throw_with_nested(std::invalid_argument(std::string("Could not set priority: ") + err));
        }
    }
    gnutls_priority_t get_priority() const {
        return _priority.get();
    }
private:
    friend class credentials_builder;
    friend class session;

    bool need_load_system_trust() const {
        return _load_system_trust;
    }
    future<> maybe_load_system_trust() {
        return with_semaphore(_system_trust_sem, 1, [this] {
            if (!_load_system_trust) {
                return make_ready_future();
            }
            return set_system_trust();
        });
    }

    gnutls_certificate_credentials_t _creds;
    std::unique_ptr<tls::dh_params::impl> _dh_params;
    std::unique_ptr<std::remove_pointer_t<gnutls_priority_t>, void(*)(gnutls_priority_t)> _priority;
    client_auth _client_auth = client_auth::NONE;
    bool _load_system_trust = false;
    semaphore _system_trust_sem {1};
};

seastar::tls::certificate_credentials::certificate_credentials()
        : _impl(std::make_unique<impl>()) {
}

seastar::tls::certificate_credentials::~certificate_credentials() {
}

seastar::tls::certificate_credentials::certificate_credentials(
        certificate_credentials&&) noexcept = default;
seastar::tls::certificate_credentials& seastar::tls::certificate_credentials::operator=(
        certificate_credentials&&) noexcept = default;

void seastar::tls::certificate_credentials::set_x509_trust(const blob& b,
        x509_crt_format fmt) {
    _impl->set_x509_trust(b, fmt);
}

void seastar::tls::certificate_credentials::set_x509_crl(const blob& b,
        x509_crt_format fmt) {
    _impl->set_x509_crl(b, fmt);

}
void seastar::tls::certificate_credentials::set_x509_key(const blob& cert,
        const blob& key, x509_crt_format fmt) {
    _impl->set_x509_key(cert, key, fmt);
}

void seastar::tls::certificate_credentials::set_simple_pkcs12(const blob& b,
        x509_crt_format fmt, const sstring& password) {
    _impl->set_simple_pkcs12(b, fmt, password);
}

future<> seastar::tls::abstract_credentials::set_x509_trust_file(
        const sstring& cafile, x509_crt_format fmt) {
    return read_fully(cafile, "trust file").then([this, fmt](temporary_buffer<char> buf) {
        set_x509_trust(blob(buf.get(), buf.size()), fmt);
    });
}

future<> seastar::tls::abstract_credentials::set_x509_crl_file(
        const sstring& crlfile, x509_crt_format fmt) {
    return read_fully(crlfile, "crl file").then([this, fmt](temporary_buffer<char> buf) {
        set_x509_crl(blob(buf.get(), buf.size()), fmt);
    });
}

future<> seastar::tls::abstract_credentials::set_x509_key_file(
        const sstring& cf, const sstring& kf, x509_crt_format fmt) {
    return read_fully(cf, "certificate file").then([this, fmt, kf](temporary_buffer<char> buf) {
        return read_fully(kf, "key file").then([this, fmt, buf = std::move(buf)](temporary_buffer<char> buf2) {
                    set_x509_key(blob(buf.get(), buf.size()), blob(buf2.get(), buf2.size()), fmt);
                });
    });
}

future<> seastar::tls::abstract_credentials::set_simple_pkcs12_file(
        const sstring& pkcs12file, x509_crt_format fmt,
        const sstring& password) {
    return read_fully(pkcs12file, "pkcs12 file").then([this, fmt, password](temporary_buffer<char> buf) {
        set_simple_pkcs12(blob(buf.get(), buf.size()), fmt, password);
    });
}

future<> seastar::tls::certificate_credentials::set_system_trust() {
    return _impl->set_system_trust();
}

void seastar::tls::certificate_credentials::set_priority_string(const sstring& prio) {
    _impl->set_priority_string(prio);
}

seastar::tls::server_credentials::server_credentials(::shared_ptr<dh_params> dh)
    : server_credentials(*dh)
{}

seastar::tls::server_credentials::server_credentials(const dh_params& dh) {
    _impl->dh_params(dh);
}

seastar::tls::server_credentials::server_credentials(server_credentials&&) noexcept = default;
seastar::tls::server_credentials& seastar::tls::server_credentials::operator=(
        server_credentials&&) noexcept = default;

void seastar::tls::server_credentials::set_client_auth(client_auth ca) {
    _impl->set_client_auth(ca);
}

static const sstring dh_level_key = "dh_level";
static const sstring x509_trust_key = "x509_trust";
static const sstring x509_crl_key = "x509_crl";
static const sstring x509_key_key = "x509_key";
static const sstring pkcs12_key = "pkcs12";
static const sstring system_trust = "system_trust";

typedef std::basic_string<seastar::tls::blob::value_type, seastar::tls::blob::traits_type, std::allocator<seastar::tls::blob::value_type>> buffer_type;

void seastar::tls::credentials_builder::set_dh_level(dh_params::level level) {
    _blobs.emplace(dh_level_key, level);
}

void seastar::tls::credentials_builder::set_x509_trust(const blob& b, x509_crt_format fmt) {
    _blobs.emplace(x509_trust_key, std::make_pair(b.to_string(), fmt));
}

void seastar::tls::credentials_builder::set_x509_crl(const blob& b, x509_crt_format fmt) {
    _blobs.emplace(x509_crl_key, std::make_pair(b.to_string(), fmt));
}

void seastar::tls::credentials_builder::set_x509_key(const blob& cert, const blob& key, x509_crt_format fmt) {
    _blobs.emplace(x509_key_key, std::make_tuple(cert.to_string(), key.to_string(), fmt));
}

void seastar::tls::credentials_builder::set_simple_pkcs12(const blob& b, x509_crt_format fmt, const sstring& password) {
    _blobs.emplace(pkcs12_key, std::make_tuple(b.to_string(), fmt, password));
}

future<> seastar::tls::credentials_builder::set_system_trust() {
    // TODO / Caveat:
    // We cannot actually issue a loading of system trust here,
    // because we have no actual tls context.
    // And we probably _don't want to get into the guessing game
    // of where the system trust cert chains are, since this is
    // super distro dependent, and usually compiled into the library.
    // Pretent it is raining, and just set a flag.
    // Leave the function returning future, so if we change our
    // minds and want to do explicit loading, we can...
    _blobs.emplace(system_trust, true);
    return make_ready_future();
}

void seastar::tls::credentials_builder::set_client_auth(client_auth auth) {
    _client_auth = auth;
}

void seastar::tls::credentials_builder::set_priority_string(const sstring& prio) {
    _priority = prio;
}

void seastar::tls::credentials_builder::apply_to(certificate_credentials& creds) const {
    // Could potentially be templated down, but why bother...
    {
        auto tr = _blobs.equal_range(x509_trust_key);
        for (auto& p : boost::make_iterator_range(tr.first, tr.second)) {
            auto v = boost::any_cast<std::pair<buffer_type, x509_crt_format>>(p.second);
            creds.set_x509_trust(v.first, v.second);
        }
    }
    {
        auto tr = _blobs.equal_range(x509_crl_key);
        for (auto& p : boost::make_iterator_range(tr.first, tr.second)) {
            auto v = boost::any_cast<std::pair<buffer_type, x509_crt_format>>(p.second);
            creds.set_x509_crl(v.first, v.second);
        }
    }
    {
        auto tr = _blobs.equal_range(x509_key_key);
        for (auto& p : boost::make_iterator_range(tr.first, tr.second)) {
            auto v = boost::any_cast<std::tuple<buffer_type, buffer_type, x509_crt_format>>(p.second);
            creds.set_x509_key(std::get<0>(v), std::get<1>(v), std::get<2>(v));
        }
    }
    {
        auto tr = _blobs.equal_range(pkcs12_key);
        for (auto& p : boost::make_iterator_range(tr.first, tr.second)) {
            auto v = boost::any_cast<std::tuple<buffer_type, x509_crt_format, sstring>>(p.second);
            creds.set_simple_pkcs12(std::get<0>(v), std::get<1>(v), std::get<2>(v));
        }
    }

    // TODO / Caveat:
    // We cannot do this immediately, because we are not a continuation, and
    // potentially blocking calls are a no-no.
    // Doing this detached would be indeterministic, so set a flag in
    // credentials, and do actual loading in first handshake (see session)
    if (_blobs.count(system_trust)) {
        creds._impl->_load_system_trust = true;
    }

    if (!_priority.empty()) {
        creds.set_priority_string(_priority);
    }

    creds._impl->set_client_auth(_client_auth);
}

::shared_ptr<seastar::tls::certificate_credentials> seastar::tls::credentials_builder::build_certificate_credentials() const {
    auto creds = ::make_shared<certificate_credentials>();
    apply_to(*creds);
    return creds;
}

::shared_ptr<seastar::tls::server_credentials> seastar::tls::credentials_builder::build_server_credentials() const {
    auto i = _blobs.find(dh_level_key);
    if (i == _blobs.end()) {
        throw std::invalid_argument("No DH level set");
    }
    auto creds = ::make_shared<server_credentials>(dh_params(boost::any_cast<dh_params::level>(i->second)));
    apply_to(*creds);
    return creds;
}

namespace seastar {
namespace tls {

/**
 * Session wraps gnutls session, and is the
 * actual conduit for an TLS/SSL data flow.
 *
 * We use a connected_socket and its sink/source
 * for IO. Note that we need to keep ownership
 * of these, since we handle handshake etc.
 *
 */
class session : public enable_lw_shared_from_this<session> {
public:
    enum class type
        : uint32_t {
            CLIENT = GNUTLS_CLIENT, SERVER = GNUTLS_SERVER,
    };

    session(type t, ::shared_ptr<certificate_credentials> creds,
            std::unique_ptr<::net::connected_socket_impl> sock, sstring name = { })
            : _type(t), _sock(std::move(sock)), _creds(std::move(creds)), _hostname(
                    std::move(name)), _in(_sock->source()), _out(_sock->sink()),
                    _in_sem(1), _out_sem(1), _output_pending(
                    make_ready_future<>()), _session([t] {
                gnutls_session_t session;
                gtls_chk(gnutls_init(&session, GNUTLS_NONBLOCK|uint32_t(t)));
                return session;
            }(), &gnutls_deinit) {
        gtls_chk(gnutls_set_default_priority(*this));
        gtls_chk(
                gnutls_credentials_set(*this, GNUTLS_CRD_CERTIFICATE,
                        *_creds->_impl));
        if (_type == type::SERVER) {
            switch (_creds->_impl->get_client_auth()) {
                case client_auth::NONE:
                default:
                    gnutls_certificate_server_set_request(*this, GNUTLS_CERT_IGNORE);
                    break;
                case client_auth::REQUEST:
                    gnutls_certificate_server_set_request(*this, GNUTLS_CERT_REQUEST);
                    break;
                case client_auth::REQUIRE:
                    gnutls_certificate_server_set_request(*this, GNUTLS_CERT_REQUIRE);
                    break;
            }
        }

        auto prio = _creds->_impl->get_priority();
        if (prio) {
            gtls_chk(gnutls_priority_set(*this, prio));
        }

        gnutls_transport_set_ptr(*this, this);
        gnutls_transport_set_vec_push_function(*this, &vec_push_wrapper);
        gnutls_transport_set_pull_function(*this, &pull_wrapper);

        // This would be nice, because we preferably want verification to
        // abort hand shake so peer immediately knows we bailed...
#if GNUTLS_VERSION_NUMBER >= 0x030406
        if (_type == type::CLIENT) {
            gnutls_session_set_verify_function(*this, &verify_wrapper);
        }
#endif
    }
    session(type t, ::shared_ptr<certificate_credentials> creds,
            ::connected_socket sock, sstring name = { })
            : session(t, std::move(creds), ::net::get_impl::get(std::move(sock)),
                    std::move(name)) {
    }

    ~session() {}

    void close() {
        shutdown_input();
    }

    typedef temporary_buffer<char> buf_type;

    sstring cert_status_to_string(gnutls_certificate_type_t type, unsigned int status) {
        gnutls_datum_t out;
        gtls_chk(
                gnutls_certificate_verification_status_print(status, type, &out,
                        0));
        sstring s(reinterpret_cast<const char *>(out.data), out.size);
        gnutls_free(out.data);
        return s;
    }

    future<> maybe_rehandshake() {
        if (_type == type::CLIENT) {
            return make_ready_future<>(); // can ignore
        }
        return handshake();
    }

    future<> handshake() {
        // maybe load system certificates before handshake, in case we
        // have not done so yet...
        if (_creds->_impl->need_load_system_trust()) {
            return _creds->_impl->maybe_load_system_trust().then([this] {
               return handshake();
            });
        }
        auto res = gnutls_handshake(*this);
        if (res < 0) {
            switch (res) {
            case GNUTLS_E_AGAIN:
                // Could not send/recv data immediately.
                // Ask gnutls which direction we are waiting for.
                if (gnutls_record_get_direction(*this) == 0) {
                    return wait_for_input().then([this] {
                        return handshake();
                    });
                } else {
                    return wait_for_output().then([this] {
                        return handshake();
                    });
                }
            case GNUTLS_E_NO_CERTIFICATE_FOUND:
                return make_exception_future<>(verification_error("No certificate was found"));
#if GNUTLS_VERSION_NUMBER >= 0x030406
            case GNUTLS_E_CERTIFICATE_ERROR:
                verify(); // should throw. otherwise, fallthrough
#endif
            default:
                return make_exception_future<>(std::system_error(res, glts_errorc));
            }
        }
        if (_type == type::CLIENT) {
            verify();
        }
        return make_ready_future<>();
    }

    size_t in_avail() const {
        return _input.size();
    }
    bool eof() const {
        return _eof;
    }
    future<> wait_for_input(bool async = false) {
        if (!_input.empty()) {
            return make_ready_future<>();
        }

        /**
         * If trying to wait for available data "async", i.e.
         * outside the normal read/write streams flow - e.g.
         * socket shutdown - we cannot just hijack the underlying
         * socket read promise, because then anyone already waiting
         * will never wake up.
         * But, since gnutls state machine works as it does,
         * we only need to wait for data to be processed in general,
         * and the handshake should get processed.
         *
         * In this case, it is sufficient to acquire and release the
         * semaphore.
         */
        if (async && !_in_sem.current()) {
            return get_units(_in_sem, 1).discard_result();
        }

        return with_semaphore(_in_sem, 1, [this] {
            return _in.get().then([this](buf_type buf) {
                _eof |= buf.empty();
               _input = std::move(buf);
            });
        });
    }
    future<> wait_for_output(bool async = false) {
        // See above
        if (async && !_out_sem.current()) {
            return get_units(_out_sem, 1).discard_result();
        }
        // The future generated by sending.
        return with_semaphore(_out_sem, 1, [this] {
            auto f = std::move(*_output_pending);
            _output_pending = {};
            return f.handle_exception([this](auto ep) {
                _output_exception = std::move(ep);
            });
        });
    }

    static session * from_transport_ptr(gnutls_transport_ptr_t ptr) {
        return static_cast<session *>(ptr);
    }
#if GNUTLS_VERSION_NUMBER >= 0x030406
    static int verify_wrapper(gnutls_session_t gs) {
        try {
            from_transport_ptr(gnutls_transport_get_ptr(gs))->verify();
            return 0;
        } catch (...) {
            return GNUTLS_E_CERTIFICATE_ERROR;
        }
    }
#endif
    static ssize_t vec_push_wrapper(gnutls_transport_ptr_t ptr, const giovec_t * iov, int iovcnt) {
        return from_transport_ptr(ptr)->vec_push(iov, iovcnt);
    }
    static ssize_t pull_wrapper(gnutls_transport_ptr_t ptr, void* dst, size_t len) {
        return from_transport_ptr(ptr)->pull(dst, len);
    }

    void verify() {
        unsigned int status;
        auto res = gnutls_certificate_verify_peers3(*this,
                _hostname.empty() ? nullptr : _hostname.c_str(), &status);
        if (res < 0) {
            throw std::system_error(res, glts_errorc);
        }
        if (status & GNUTLS_CERT_INVALID) {
            throw verification_error(
                    cert_status_to_string(gnutls_certificate_type_get(*this),
                            status));
        }
    }

    ssize_t pull(void* dst, size_t len) {
        if (eof()) {
            return 0;
        }
        // If we have data in buffers, we can complete.
        // Otherwise, we must be conservative.
        if (_input.empty()) {
            gnutls_transport_set_errno(*this, EAGAIN);
            return -1;
        }
        auto n = std::min(len, _input.size());
        memcpy(dst, _input.get(), n);
        _input.trim_front(n);
        return n;
    }
    ssize_t vec_push(const giovec_t * iov, int iovcnt) {
        // Sending is a pain.
        // While gnutls handles async IO, it assumes
        // that if it get EAGAIN (io would block)
        // the data was _not_ sent. However, we have
        // no other means of sending than simply
        // put it on the wire and wait for it to
        // complete.
        // This is a mismatch.
        // Luckily, gnutls has the feature that
        // if a send fails due to blocking, a caller
        // (us) can send null+0 and thus cause a re-send
        // of the last encrypted packet attempted
        // (from the internal buffers in gnutls).
        // So, to handle all the above, we keep track
        // of how much we issued a send of,
        // and then attempt to match it with next
        // send request. If the data size match, we
        // assume it is the re-send from higher up,
        // that has properly waited for output future
        // completion. In this case we can ignore the
        // send and just consider it completed.

        size_t n = 0;
        for (int i = 0; i < iovcnt; ++i) {
            n += iov[i].iov_len;
        }
        // See above. If we have a pending send
        // the next time we reach this point, it
        // must be the re-send, otherwise we
        // have broken our state machine.
        if (_out_expect == 0) {
            scattered_message<char> msg;
            for (int i = 0; i < iovcnt; ++i) {
                msg.append(sstring(reinterpret_cast<const char *>(iov[i].iov_base), iov[i].iov_len));
            }
            _output_exception = {};
            _output_pending = _out.put(std::move(msg).release());
            // Did we complete already?
            if (_output_pending->available() && !_output_pending->failed()) {
                return n;
            }
            if (_output_pending->failed()) {
                _output_exception = _output_pending->get_exception();
            }
        }
        if (_output_exception) {
            _output_pending = {};
            _out_expect = 0;
            gnutls_transport_set_errno(*this, EIO);
            return -1;
        }
        if (_out_expect != 0) {
            assert(!_output_pending);
            if (n != _out_expect) {
                throw std::logic_error("State machine broken?");
            }
            _out_expect = 0;
            return n;
        }
        // No? Let the IO complete and tell gnutls we could not
        // complete. This will propagate the error code upwards to
        // our higher level code.
        _out_expect = n;
        gnutls_transport_set_errno(*this, EAGAIN);
        return -1;
    }

    operator gnutls_session_t() const {
        return _session.get();
    }

    future<>
    handle_error(int res) {
        return make_exception_future(std::system_error(res, glts_errorc));
    }
    future<>
    handle_output_error(int res) {
        if (_output_exception) {
            return make_exception_future(std::move(_output_exception));
        } else {
            return handle_error(res);
        }
    }

    template<typename Func>
    future<> finish_handshake_op(int res, Func&& f, bool async = false) {
        if (res < 0) {
            switch (res) {
            case GNUTLS_E_AGAIN:
                // Could not send/recv data immediately.
                // Ask gnutls which direction we are waiting for.
                if (gnutls_record_get_direction(*this) == 0) {
                    return wait_for_input(async).then([this, f = std::forward<Func>(f)] {
                        return f();
                    });
                } else {
                    return wait_for_output(async).then([this, f = std::forward<Func>(f)] {
                        return f();
                    });
                }
            default:
                // Since this is a handshake, _output_exception
                // should only be set if we did actually do a push
                // that failed. So its ok to use handle_output_error
                return handle_output_error(res);
            }
        }
        return make_ready_future<>();
    }

    future<> shutdown(gnutls_close_request_t how) {
        // Note: shutdown is potentially simultaneous with outstanding
        // read/writes, so must do "async" handshake. See wait_for_input
        // for explanation.
        return finish_handshake_op(gnutls_bye(*this, how),
                std::bind(&session::shutdown, this, how), true);
    }
    template<typename Func>
    void shutdown_with_timer(gnutls_close_request_t how, Func && func);

    void shutdown_input() {
        if (!std::exchange(_shutdown_rw, true)) {
            shutdown_with_timer(GNUTLS_SHUT_RDWR, [](session & s) {
               s._eof = true;
               s._in.close();
            });
        }
    }
    void shutdown_output() {
        if (!std::exchange(_shutdown_wr, true)) {
            shutdown_with_timer(GNUTLS_SHUT_WR, [](session & s) {
               s._out.close();
            });
        }
    }
    // helper for sink
    future<> flush() {
        return _out.flush();
    }

    ::net::connected_socket_impl & socket() const {
        return *_sock;
    }

    struct session_ref;
private:
    type _type;

    std::unique_ptr<::net::connected_socket_impl> _sock;
    ::shared_ptr<certificate_credentials> _creds;
    const sstring _hostname;
    data_source _in;
    data_sink _out;

    semaphore _in_sem, _out_sem;

    bool _eof = false;
    bool _shutdown_wr = false;
    bool _shutdown_rw = false;

    std::experimental::optional<future<>> _output_pending;
    std::exception_ptr _output_exception;
    size_t _out_expect = 0;
    buf_type _input;

    // modify this to a unique_ptr to handle exceptions in our constructor.
    std::unique_ptr<std::remove_pointer_t<gnutls_session_t>, void(*)(gnutls_session_t)> _session;
};

struct session::session_ref {
    session_ref(lw_shared_ptr<session> session)
                    : _session(std::move(session)) {
    }
    ~session_ref() {
        // This is not super pretty. But we take some care to only own sessions
        // through session_ref, and we need to initiate shutdown on "last owner",
        // since we cannot revive the session in destructor.
        if (_session.use_count() == 1) {
            _session->close();
        }
    }

    lw_shared_ptr<session> _session;
};

template<typename Func>
void session::shutdown_with_timer(gnutls_close_request_t how, Func && func) {
    auto f = shutdown(how);
    if (!f.available()) {
        auto me = shared_from_this();
        session_ref sr(std::move(me));

        timer<> t([sr, func] {
            func(*sr._session);
        });
        t.arm(timer<>::clock::now() + std::chrono::seconds(10));
        f.then_wrapped([sr, t = std::move(t), func](auto f) {
           f.ignore_ready_future();
           func(*sr._session);
        });
    }
}

class tls_connected_socket_impl : public ::net::connected_socket_impl, public session::session_ref {
public:
    using session_ref::session_ref;

    class source_impl;
    class sink_impl;

    data_source source() override;
    data_sink sink() override;

    void shutdown_input() override {
        _session->shutdown_input();
    }
    void shutdown_output() override {
        _session->shutdown_output();
    }
    void set_nodelay(bool nodelay) override {
        _session->socket().set_nodelay(nodelay);
    }
    bool get_nodelay() const override {
        return _session->socket().get_nodelay();
    }
    void set_keepalive(bool keepalive) override {
        _session->socket().set_keepalive(keepalive);
    }
    bool get_keepalive() const override {
        return _session->socket().get_keepalive();
    }
    void set_keepalive_parameters(const ::net::keepalive_params& p) override {
        _session->socket().set_keepalive_parameters(p);
    }
    ::net::keepalive_params get_keepalive_parameters() const override {
        return _session->socket().get_keepalive_parameters();
    }
};


class tls_connected_socket_impl::source_impl: public ::data_source_impl, public session::session_ref {
public:
    using session_ref::session_ref;
private:
    future<temporary_buffer<char>> get() override {
        // gnutls might have stuff in its buffers.
        auto avail = gnutls_record_check_pending(*_session);
        if (avail == 0) {
            // or we might...
            avail = _session->in_avail();
        }
        if (avail != 0) {
            // typically, unencrypted data can get smaller (padding),
            // but not larger.
            temporary_buffer<char> output(avail);
            auto n = gnutls_record_recv(*_session, output.get_write(),
                    output.size());
            if (n < 0) {
                switch (n) {
                case GNUTLS_E_AGAIN:
                    // Assume we got this because we read to little underlying
                    // data to finish a tls packet
                    // Our input buffer should be empty now, so just go again
                    return get();
                case GNUTLS_E_REHANDSHAKE:
                    return _session->maybe_rehandshake().then([this] {
                       return get();
                    });
                default:
                    return make_exception_future<temporary_buffer<char>>(std::system_error(n, glts_errorc));
                }
            }
            output.trim(n);
            return make_ready_future<temporary_buffer<char>>(std::move(output));
        }
        if (_session->eof()) {
            return make_ready_future<temporary_buffer<char>>();
        }
        // No input? wait for out buffers to fill...
        return _session->wait_for_input().then([this] {
            return get();
        });
    }
    future<> close() override {
        _session->shutdown_input();
        return make_ready_future<>();
    }
};

// Note: source/sink, and by extension, the in/out streams
// produced, cannot exist outside the direct life span of
// the connected_socket itself. This is consistent with
// other sockets in seastar, though I am than less fond of it...
class tls_connected_socket_impl::sink_impl: public ::data_sink_impl, public session::session_ref {
public:
    using session_ref::session_ref;
private:
    typedef ::net::fragment* frag_iter;

    future<> put(::net::packet p, frag_iter i, frag_iter e, size_t off = 0) {
        while (i != e) {
            auto ptr = i->base;
            auto size = i->size;
            while (off < size) {
                // gnutls does not have a sendv. Why?...
                auto res = gnutls_record_send(*_session, ptr + off, size - off);

                if (res < 0) {
                    switch (res) {
                    case GNUTLS_E_AGAIN:
                        // See the session::put comments.
                        // If underlying says EAGAIN, we've actually issued
                        // a send, but must wait for completion.
                        return _session->wait_for_output().then(
                                [this, p = std::move(p), size, i, e, off]() mutable {
                                    // re-send same buffers (gnutls internal)
                                    auto check = gnutls_record_send(*_session, nullptr, 0);
                                    return this->put(std::move(p), i, e, off + check);
                                });
                    default:
                        return _session->handle_output_error(res);
                    }
                }
                off += res;
            }
            off = 0;
            ++i;
        }
        return make_ready_future<>();
    }

    future<> flush() override {
        return _session->flush();
    }
    future<> put(::net::packet p) override {
        auto i = p.fragments().begin();
        auto e = p.fragments().end();
        return put(std::move(p), i, e);
    }

    future<> close() override {
        _session->shutdown_output();
        return make_ready_future<>();
    }
};

class server_session : public ::net::server_socket_impl {
public:
    server_session(::shared_ptr<server_credentials> creds, ::server_socket sock)
            : _creds(std::move(creds)), _sock(std::move(sock)) {
    }
    future<connected_socket, socket_address> accept() override {
        // We're not actually doing anything very SSL until we get
        // an actual connection. Then we create a "server" session
        // and wrap it up after handshaking.
        return _sock.accept().then([this](::connected_socket s, ::socket_address addr) {
            return wrap_server(_creds, std::move(s)).then([addr](::connected_socket s) {
                return make_ready_future<connected_socket, socket_address>(std::move(s), addr);
            });
        });
    }
    void abort_accept() override  {
        _sock.abort_accept();
    }
private:
    ::shared_ptr<server_credentials> _creds;
    ::server_socket _sock;
};

class tls_socket_impl : public ::net::socket_impl {
    ::shared_ptr<certificate_credentials> _cred;
    sstring _name;
    seastar::socket _socket;
public:
    tls_socket_impl(::shared_ptr<certificate_credentials> cred, sstring name)
            : _cred(cred), _name(std::move(name)), _socket(engine().net().socket()) {
    }
    virtual future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) override {
        return _socket.connect(sa, local, proto).then([cred = std::move(_cred), name = std::move(_name)](::connected_socket s) mutable {
            return wrap_client(cred, std::move(s), std::move(name));
        });
    }
    virtual void shutdown() override {
        _socket.shutdown();
    }
};

}
}

data_source seastar::tls::tls_connected_socket_impl::source() {
    return data_source(std::make_unique<source_impl>(_session));
}

data_sink seastar::tls::tls_connected_socket_impl::sink() {
    return data_sink(std::make_unique<sink_impl>(_session));
}


future<::connected_socket> seastar::tls::connect(::shared_ptr<certificate_credentials> cred, socket_address sa, sstring name) {
    return engine().connect(sa).then([cred = std::move(cred), name = std::move(name)](::connected_socket s) mutable {
        return wrap_client(cred, std::move(s), std::move(name));
    });
}

future<::connected_socket> seastar::tls::connect(::shared_ptr<certificate_credentials> cred, socket_address sa, socket_address local, sstring name) {
    return engine().connect(sa, local).then([cred = std::move(cred), name = std::move(name)](::connected_socket s) mutable {
        return wrap_client(cred, std::move(s), std::move(name));
    });
}

seastar::socket seastar::tls::socket(::shared_ptr<certificate_credentials> cred, sstring name) {
    return seastar::socket(std::make_unique<tls_socket_impl>(std::move(cred), std::move(name)));
}

future<::connected_socket> seastar::tls::wrap_client(::shared_ptr<certificate_credentials> cred, ::connected_socket&& s, sstring name) {
    auto sess = make_lw_shared<session>(session::type::CLIENT, std::move(cred), std::move(s), std::move(name));
    auto f = sess->handshake();
    return f.then([sess = std::move(sess)]() mutable {
        ::connected_socket ssls(std::make_unique<tls_connected_socket_impl>(std::move(sess)));
        return make_ready_future<::connected_socket>(std::move(ssls));
    });
}

future<::connected_socket> seastar::tls::wrap_server(::shared_ptr<server_credentials> cred, ::connected_socket&& s) {
    auto sess = make_lw_shared<session>(session::type::SERVER, std::move(cred), std::move(s));
    auto f = sess->handshake();
    return f.then([sess = std::move(sess)]() mutable {
        ::connected_socket ssls(std::make_unique<tls_connected_socket_impl>(std::move(sess)));
        return make_ready_future<::connected_socket>(std::move(ssls));
    });
}

::server_socket seastar::tls::listen(::shared_ptr<server_credentials> creds, ::socket_address sa, ::listen_options opts) {
    return listen(std::move(creds), engine().listen(sa, opts));
}

::server_socket seastar::tls::listen(::shared_ptr<server_credentials> creds, ::server_socket ss) {
    ::server_socket ssls(std::make_unique<server_session>(creds, std::move(ss)));
    return ::server_socket(std::move(ssls));
}


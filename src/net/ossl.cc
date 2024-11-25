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
 * Copyright 2024 Redpanda Data
 */

#ifdef SEASTAR_MODULE
module;
#endif

#include <fmt/ranges.h>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/provider.h>
#include <openssl/rand.h>
#include <openssl/safestack.h>
#include <openssl/ssl.h>
#include <openssl/sslerr.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

#include <span>
#include <system_error>

#ifdef SEASTAR_MODULE
module seastar;
#else
#include "net/tls-impl.hh"

#include <seastar/core/gate.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/stack.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/defer.hh>
#include <seastar/util/later.hh>
#include <seastar/util/log.hh>
#endif

namespace seastar {

enum class ossl_errc : int{};

}

namespace std {

template<>
struct is_error_code_enum<seastar::ossl_errc> : true_type {};

}

template<>
struct fmt::formatter<seastar::ossl_errc> : public fmt::formatter<std::string_view> {
    auto format(seastar::ossl_errc error, fmt::format_context& ctx) const -> decltype(ctx.out()) {
        constexpr size_t error_buf_size = 256;
        // Buffer passed to ERR_error_string must be at least 256 bytes large
        // https://www.openssl.org/docs/man3.0/man3/ERR_error_string_n.html
        std::array<char, error_buf_size> buf{};
        ERR_error_string_n(
          static_cast<unsigned long>(error), buf.data(), buf.size());
        // ERR_error_string_n does include the terminating null character
        return fmt::format_to(ctx.out(), "{}", buf.data());
    }
};

namespace seastar {

int tls_version_to_ossl(tls::tls_version version) {
    switch(version) {
    case tls::tls_version::tlsv1_0:
        return TLS1_VERSION;
    case tls::tls_version::tlsv1_1:
        return TLS1_1_VERSION;
    case tls::tls_version::tlsv1_2:
        return TLS1_2_VERSION;
    case tls::tls_version::tlsv1_3:
        return TLS1_3_VERSION;
    }

    __builtin_unreachable();
}

class ossl_error_category : public std::error_category {
public:
    constexpr ossl_error_category() noexcept : std::error_category{} {}
    const char* name() const noexcept override {
        return "OpenSSL";
    }
    std::string message(int error) const override {
        return fmt::format("{}", static_cast<ossl_errc>(error));
    }
};

const std::error_category& tls::error_category() {
    static const ossl_error_category ec;
    return ec;
}

std::error_code make_error_code(ossl_errc e) {
    return std::error_code(static_cast<int>(e), tls::error_category());
}

std::system_error make_ossl_error(const std::string & msg) {
    std::vector<ossl_errc> error_codes;
    for (auto code = ERR_get_error(); code != 0; code = ERR_get_error()) {
        error_codes.push_back(static_cast<ossl_errc>(code));
    }
    if (error_codes.empty()) {
        return std::system_error{
            static_cast<int>(ERR_PACK(ERR_LIB_USER, 0, ERR_R_OPERATION_FAIL)),
            tls::error_category(),
            msg};
    } else {
        auto err_code = static_cast<unsigned long>(error_codes.front());
        if (ERR_LIB_SYS == ERR_GET_LIB(err_code)) {
            // If the error code belongs to ERR_LIB_SYS, then the error is a system error
            // Extract the errno using ERR_GET_REASON and throw a std::generic_category
            return std::system_error(
                ERR_GET_REASON(err_code),
                std::generic_category(),
                fmt::format("{}: {}", msg, error_codes));
        }
        return std::system_error(
            static_cast<int>(err_code),
            tls::error_category(),
            fmt::format("{}: {}", msg, error_codes));
    }
}

template<typename T>
sstring asn1_str_to_str(T* asn1) {
    const auto len = ASN1_STRING_length(asn1);
    return sstring(reinterpret_cast<const char*>(ASN1_STRING_get0_data(asn1)), len);
};

static std::vector<std::byte> extract_x509_serial(X509* cert) {
    constexpr size_t serial_max = 160;
    const ASN1_INTEGER *serial_no = X509_get_serialNumber(cert);
    const size_t serial_size = std::min(serial_max, (size_t)serial_no->length);
    std::vector<std::byte> serial(
        reinterpret_cast<std::byte*>(serial_no->data),
        reinterpret_cast<std::byte*>(serial_no->data + serial_size));
    return serial;
}

static time_t extract_x509_expiry(X509* cert) {
    const ASN1_TIME *not_after = X509_get0_notAfter(cert);
    if (not_after != nullptr) {
        tm tm_struct{};
        ASN1_TIME_to_tm(not_after, &tm_struct);
        return mktime(&tm_struct);
    }
    return -1;
}

template<typename T, auto fn>
struct ssl_deleter {
    void operator()(T* ptr) { fn(ptr); }
};

// Must define this method as sk_X509_pop_free is a macro
void X509_pop_free(STACK_OF(X509)* ca) {
    sk_X509_pop_free(ca, X509_free);
}

void X509_INFO_pop_free(STACK_OF(X509_INFO)* infos) {
    sk_X509_INFO_pop_free(infos, X509_INFO_free);
}

void GENERAL_NAME_pop_free(GENERAL_NAMES* gns) {
    sk_GENERAL_NAME_pop_free(gns, GENERAL_NAME_free);
}

template<typename T, auto fn>
using ssl_handle = std::unique_ptr<T, ssl_deleter<T, fn>>;

using bio_method_ptr = ssl_handle<BIO_METHOD, BIO_meth_free>;
using bio_ptr = ssl_handle<BIO, BIO_free>;
using evp_pkey_ptr = ssl_handle<EVP_PKEY, EVP_PKEY_free>;
using x509_ptr = ssl_handle<X509, X509_free>;
using x509_crl_ptr = ssl_handle<X509_CRL, X509_CRL_free>;
using x509_store_ptr = ssl_handle<X509_STORE, X509_STORE_free>;
using x509_store_ctx_ptr = ssl_handle<X509_STORE_CTX, X509_STORE_CTX_free>;
using x509_chain_ptr = ssl_handle<STACK_OF(X509), X509_pop_free>;
using x509_infos_ptr = ssl_handle<STACK_OF(X509_INFO), X509_INFO_pop_free>;
using general_names_ptr = ssl_handle<GENERAL_NAMES, GENERAL_NAME_pop_free>;
using pkcs12 = ssl_handle<PKCS12, PKCS12_free>;
using ssl_ctx_ptr = ssl_handle<SSL_CTX, SSL_CTX_free>;
using ssl_ptr = ssl_handle<SSL, SSL_free>;
using x509_verify_param_ptr = ssl_handle<X509_VERIFY_PARAM, X509_VERIFY_PARAM_free>;
using ssl_session_ptr = ssl_handle<SSL_SESSION, SSL_SESSION_free>;

/**
 * The purpose of this structure is to hold the AES and HMAC keys used to encrypt/decrypt TLS 1.3
 * session tickets given to and presented by, the TLS client.
 */
struct session_ticket_keys {
    std::array<uint8_t, 16> key_name;
    std::array<uint8_t, 32> aes_key;
    std::array<uint8_t, 32> hmac_key;

    void generate_keys() {
        generate_key(key_name);
        generate_key(aes_key);
        generate_key(hmac_key);
    }

    ~session_ticket_keys() {
        clear_key(aes_key);
        clear_key(hmac_key);
    }

private:
    /**
     * This will zeroize the key contents by writing 0xff, then 0x00,
     * and finally 0xff over the memory that held the key
     * @tparam N Size of the array
     * @param key The key to clear
     */
    template<std::size_t N>
    static void clear_key(std::array<uint8_t, N>& key) {
        // Zeroize sensitive key data
        OPENSSL_cleanse(key.data(), N);
    }

    /**
     * Generates a key
     * @tparam N The size of the key
     * @param key The key to generate
     * @throws ossl_error if unable to generate random data
     */
    template<std::size_t N>
    static void generate_key(std::array<uint8_t, N>& key) {
        if (RAND_priv_bytes(key.data(), N) <= 0) {
            throw make_ossl_error("Failed to generate key");
        }
    }
};

// sufficiently large enough to avoid collision with OpenSSL BIO controls
#define BIO_C_SET_POINTER 1000
// Index into ex data for SSL structure to fetch a pointer to session
#define SSL_EX_DATA_SESSION 0

BIO_METHOD* get_method();

/// TODO: Implement the DH params impl struct
///
class tls::dh_params::impl {
public:
    explicit impl(level) {}
    impl(const blob&, x509_crt_format){}

    const EVP_PKEY* get() const { return _pkey.get(); }

    explicit operator const EVP_PKEY*() const { return _pkey.get(); }

private:
    evp_pkey_ptr _pkey;
};

tls::dh_params::dh_params(level lvl) : _impl(std::make_unique<impl>(lvl))
{}

tls::dh_params::dh_params(const blob& b, x509_crt_format fmt)
        : _impl(std::make_unique<impl>(b, fmt)) {
}

// TODO(rob) some small amount of code duplication here
tls::dh_params::~dh_params() = default;

tls::dh_params::dh_params(dh_params&&) noexcept = default;
tls::dh_params& tls::dh_params::operator=(dh_params&&) noexcept = default;

class tls::certificate_credentials::impl {
    struct certkey_pair {
        x509_ptr cert;
        evp_pkey_ptr key;
        explicit operator bool() const noexcept {
            return cert != nullptr && key != nullptr;
        }
    };

    static const int credential_store_idx = 0;

public:
    // This callback is designed to intercept the verification process and to implement an additional
    // check, returning 0 or -1 will force verification to fail.
    //
    // However it has been implemented in this case soley to cache the last observed certificate so
    // that it may be inspected during the session::verify() method, if desired.
    //
    static int verify_callback(int preverify_ok, X509_STORE_CTX* store_ctx) {
        // Grab the 'this' pointer from the stores generic data cache, it should always exist
        auto store = X509_STORE_CTX_get0_store(store_ctx);
        auto credential_impl = static_cast<impl*>(X509_STORE_get_ex_data(store, credential_store_idx));
        assert(credential_impl != nullptr);
        // Store a pointer to the current connection certificate within the impl instance
        auto cert = X509_STORE_CTX_get_current_cert(store_ctx);
        X509_up_ref(cert);
        credential_impl->_last_cert = x509_ptr(cert);
        return preverify_ok;
    }

    impl() : _creds([] {
        auto store = X509_STORE_new();
        if(store == nullptr) {
            throw std::bad_alloc();
        }
        X509_STORE_set_verify_cb(store, verify_callback);
        return store;
    }()) {
        // The static verify_callback above will use the stored pointer to 'this' to store the last
        // observed x509 certificate
        assert(X509_STORE_set_ex_data(_creds.get(), credential_store_idx, this) == 1);
    }


    // Parses a PEM certificate file that may contain more then one entry, calls the callback provided
    // passing the associated X509_INFO* argument. The parameter is not retained so the caller must retain
    // the item before the end of the function call.
    template<typename LoadFunc>
    static void iterate_pem_certs(const bio_ptr& cert_bio, LoadFunc fn) {
        auto infos = x509_infos_ptr(PEM_X509_INFO_read_bio(cert_bio.get(), nullptr, nullptr, nullptr));
        auto num_elements = sk_X509_INFO_num(infos.get());
        if (num_elements <= 0) {
            throw make_ossl_error("Failed to parse PEM cert");
        }
        for (auto i=0; i < num_elements; i++) {
            auto object = sk_X509_INFO_value(infos.get(), i);
            fn(object);
        }
    }

    static x509_ptr parse_x509_cert(const blob& b, x509_crt_format fmt) {
        bio_ptr cert_bio(BIO_new_mem_buf(b.begin(), b.size()));
        x509_ptr cert;
        switch(fmt) {
        case tls::x509_crt_format::PEM:
            cert = x509_ptr(PEM_read_bio_X509(cert_bio.get(), nullptr, nullptr, nullptr));
            break;
        case tls::x509_crt_format::DER:
            cert = x509_ptr(d2i_X509_bio(cert_bio.get(), nullptr));
            break;
        }
        if (!cert) {
            throw make_ossl_error("Failed to parse x509 certificate");
        }
        return cert;
    }

    void set_x509_trust(const blob& b, x509_crt_format fmt) {
        bio_ptr cert_bio(BIO_new_mem_buf(b.begin(), b.size()));
        x509_ptr cert;
        switch(fmt) {
        case tls::x509_crt_format::PEM:
            iterate_pem_certs(cert_bio, [this](X509_INFO* info){
                if (!info->x509) {
                    throw make_ossl_error("Failed to parse x509 cert");
                }
                X509_STORE_add_cert(*this, info->x509);
            });
            break;
        case tls::x509_crt_format::DER:
            cert = x509_ptr(d2i_X509_bio(cert_bio.get(), nullptr));
            if (!cert) {
                throw make_ossl_error("Failed to parse x509 certificate");
            }
            X509_STORE_add_cert(*this, cert.get());
            break;
        }
    }

    void set_x509_crl(const blob& b, x509_crt_format fmt) {
        bio_ptr cert_bio(BIO_new_mem_buf(b.begin(), b.size()));
        x509_crl_ptr crl;
        switch(fmt) {
        case x509_crt_format::PEM:
            iterate_pem_certs(cert_bio, [this](X509_INFO* info) {
                if (!info->crl) {
                    throw make_ossl_error("Failed to parse CRL");
                }
                X509_STORE_add_crl(*this, info->crl);
            });
            break;
        case x509_crt_format::DER:
            crl = x509_crl_ptr(d2i_X509_CRL_bio(cert_bio.get(), nullptr));
            if (!crl) {
                throw make_ossl_error("Failed to parse x509 crl");
            }
            X509_STORE_add_crl(*this, crl.get());
            break;
        }

        enable_crl_checking();
    }

    void set_x509_key(const blob& cert, const blob& key, x509_crt_format fmt) {
        x509_ptr x509_cert{nullptr};
        bio_ptr key_bio(BIO_new_mem_buf(key.begin(), key.size()));
        evp_pkey_ptr pkey;
        switch(fmt) {
        case x509_crt_format::PEM:
            pkey = evp_pkey_ptr(PEM_read_bio_PrivateKey(key_bio.get(), nullptr, nullptr, nullptr));
            // The provided `cert` blob may contain more than one cert.  We need to be prepared
            // for this situation.  So we will parse through the blob using `iterate_pem_certs`.
            // The first cert encountered will be assigned to x509_cert and all subsequent certs
            // will be added to the X509_STORE's trusted certificates
            iterate_pem_certs(bio_ptr{BIO_new_mem_buf(cert.begin(), cert.size())}, [this, &x509_cert](X509_INFO* info) {
                if (!info->x509) {
                    throw make_ossl_error("Failed to parse X.509 certificate in loading key/cert chain");
                }
                if (!x509_cert) {
                    x509_cert = x509_ptr{info->x509};
                    // By setting x509 to nullptr, the sk_X509_INFO_pop_free function will not
                    // call X509_free on it.  We have 'transfered' ownership above to the
                    // x509_cert X509 ptr
                    info->x509 = nullptr;
                } else {
                    X509_STORE_add_cert(*this, info->x509);
                }
            });
            break;
        case x509_crt_format::DER:
            pkey = evp_pkey_ptr(d2i_PrivateKey_bio(key_bio.get(), nullptr));
            // We don't handle a chain of certs when encoded in DER
            x509_cert = parse_x509_cert(cert, fmt);
            break;
        default:
            __builtin_unreachable();
        }
        if (!pkey) {
            throw make_ossl_error("Error attempting to parse private key");
        }
        if (!X509_check_private_key(x509_cert.get(), pkey.get())) {
            throw make_ossl_error("Failed to verify cert/key pair");
        }
        _cert_and_key = certkey_pair{.cert = std::move(x509_cert), .key = std::move(pkey)};
    }

    void set_simple_pkcs12(const blob& b, x509_crt_format, const sstring& password) {
        // Load the PKCS12 file
        bio_ptr bio(BIO_new_mem_buf(b.begin(), b.size()));
        if (auto p12 = pkcs12(d2i_PKCS12_bio(bio.get(), nullptr))) {
            // Extract the certificate and private key from PKCS12, using provided password
            EVP_PKEY *pkey = nullptr;
            X509 *cert = nullptr;
            STACK_OF(X509) *ca = nullptr;
            if (!PKCS12_parse(p12.get(), password.c_str(), &pkey, &cert, &ca)) {
                throw make_ossl_error("Failed to extract cert key pair from pkcs12 file");
            }
            // Ensure signature validation checks pass before continuing
            if (!X509_check_private_key(cert, pkey)) {
                X509_free(cert);
                EVP_PKEY_free(pkey);
                throw make_ossl_error("Failed to verify cert/key pair");
            }
            _cert_and_key = certkey_pair{.cert = x509_ptr(cert), .key = evp_pkey_ptr(pkey)};

            // Iterate through all elements in the certificate chain, adding them to the store
            auto ca_ptr = x509_chain_ptr(ca);
            if (ca_ptr) {
                auto num_elements = sk_X509_num(ca_ptr.get());
                while (num_elements > 0) {
                    auto e = sk_X509_pop(ca_ptr.get());
                    X509_STORE_add_cert(*this, e);
                    // store retains certificate
                    X509_free(e);
                    num_elements -= 1;
                }
            }
        } else {
            throw make_ossl_error("Failed to parse pkcs12 file");
        }
    }

    void enable_crl_checking() {
        if (!std::exchange(_crl_check_flag_set, true)) {
            x509_verify_param_ptr x509_vfy(X509_VERIFY_PARAM_new());

            if (1 != X509_VERIFY_PARAM_set_flags(
                    x509_vfy.get(), X509_V_FLAG_CRL_CHECK|X509_V_FLAG_CRL_CHECK_ALL)) {
                throw make_ossl_error(
                    "Failed to set X509_V_FLAG_CRL_CHECK flag");
            }

            if (1 != X509_STORE_set1_param(*this, x509_vfy.get())) {
                throw make_ossl_error(
                    "Failed to set verification parameters on X509 store");
            }
        }
    }

    void dh_params(const tls::dh_params&) {}

    std::vector<cert_info> get_x509_info() const {
        if (_cert_and_key.cert) {
            return {
                cert_info{
                    .serial = extract_x509_serial(_cert_and_key.cert.get()),
                    .expiry = extract_x509_expiry(_cert_and_key.cert.get())}
            };
        }
        return {};
    }

    std::vector<cert_info> get_x509_trust_list_info() const {
        std::vector<cert_info> cert_infos;
        STACK_OF(X509_OBJECT) *chain = X509_STORE_get0_objects(_creds.get());
        auto num_elements = sk_X509_OBJECT_num(chain);
        for (auto i=0; i < num_elements; i++) {
            auto object = sk_X509_OBJECT_value(chain, i);
            auto type = X509_OBJECT_get_type(object);
            if (type == X509_LU_X509) {
                auto cert = X509_OBJECT_get0_X509(object);
                cert_infos.push_back(cert_info{
                        .serial = extract_x509_serial(cert),
                        .expiry = extract_x509_expiry(cert)});
            }
        }
        return cert_infos;
    }

    void set_client_auth(client_auth ca) {
        _client_auth = ca;
    }
    client_auth get_client_auth() const {
        return _client_auth;
    }
    void set_session_resume_mode(session_resume_mode m) {
        _session_resume_mode = m;
        if (m != session_resume_mode::NONE) {
            _session_ticket_keys.generate_keys();
        }
    }

    session_resume_mode get_session_resume_mode() {
        return _session_resume_mode;
    }

    const session_ticket_keys & get_session_ticket_keys() const {
        return _session_ticket_keys;
    }

    void set_dn_verification_callback(dn_callback cb) {
        _dn_callback = std::move(cb);
    }

    void set_cipher_string(const sstring& cipher_string) {
        _cipher_string = cipher_string;
    }

    void set_ciphersuites(const sstring& ciphersuites) {
        _ciphersuites = ciphersuites;
    }

    void enable_server_precedence() {
        _enable_server_precedence = true;
    }

    void set_minimum_tls_version(tls_version version) {
        _min_tls_version.emplace(version);
    }

    void set_maximum_tls_version(tls_version version) {
        _max_tls_version.emplace(version);
    }

    const sstring& get_cipher_string() const noexcept {
        return _cipher_string;
    }

    const sstring& get_ciphersuites() const noexcept {
        return _ciphersuites;
    }

    bool is_server_precedence_enabled() {
        return _enable_server_precedence;
    }

    const std::optional<tls_version>& minimum_tls_version() const noexcept {
        return _min_tls_version;
    }

    const std::optional<tls_version>& maximum_tls_version() const noexcept {
        return _max_tls_version;
    }

    // Returns the certificate of last attempted verification attempt, if there was no attempt,
    // this will not be updated and will remain stale
    const x509_ptr& get_last_cert() const { return _last_cert; }

    operator X509_STORE*() const { return _creds.get(); }

    const certkey_pair& get_certkey_pair() const {
        return _cert_and_key;
    }

private:
    friend class certificate_credentials;
    friend class credentials_builder;
    friend class session;

    void set_load_system_trust(bool trust) {
        _load_system_trust = trust;
    }

    bool need_load_system_trust() const {
        return _load_system_trust;
    }

    certkey_pair _cert_and_key;
    session_ticket_keys _session_ticket_keys;
    x509_ptr _last_cert;
    x509_store_ptr _creds;
    dn_callback _dn_callback;
    std::optional<tls_version> _min_tls_version;
    std::optional<tls_version> _max_tls_version;
    sstring _cipher_string;
    sstring _ciphersuites;

    client_auth _client_auth = client_auth::NONE;
    session_resume_mode _session_resume_mode = session_resume_mode::NONE;
    bool _load_system_trust = false;
    bool _enable_server_precedence = false;
    bool _crl_check_flag_set = false;

};

tls::certificate_credentials::certificate_credentials()
        : _impl(make_shared<impl>()) {
}

tls::certificate_credentials::~certificate_credentials() {
}

tls::certificate_credentials::certificate_credentials(
        certificate_credentials&&) noexcept = default;
tls::certificate_credentials& tls::certificate_credentials::operator=(
        certificate_credentials&&) noexcept = default;

void tls::certificate_credentials::set_x509_trust(const blob& b,
        x509_crt_format fmt) {
    _impl->set_x509_trust(b, fmt);
}

void tls::certificate_credentials::set_x509_crl(const blob& b,
        x509_crt_format fmt) {
    _impl->set_x509_crl(b, fmt);

}
void tls::certificate_credentials::set_x509_key(const blob& cert,
        const blob& key, x509_crt_format fmt) {
    _impl->set_x509_key(cert, key, fmt);
}

void tls::certificate_credentials::set_simple_pkcs12(const blob& b,
        x509_crt_format fmt, const sstring& password) {
    _impl->set_simple_pkcs12(b, fmt, password);
}

future<> tls::certificate_credentials::set_system_trust() {
    _impl->_load_system_trust = true;
    return make_ready_future<>();
}

void tls::certificate_credentials::set_cipher_string(const sstring& cipher_string) {
    _impl->set_cipher_string(cipher_string);
}

void tls::certificate_credentials::set_ciphersuites(const sstring& ciphersuites) {
    _impl->set_ciphersuites(ciphersuites);
}

void tls::certificate_credentials::enable_server_precedence() {
    _impl->enable_server_precedence();
}

void tls::certificate_credentials::set_minimum_tls_version(tls_version version) {
    _impl->set_minimum_tls_version(version);
}

void tls::certificate_credentials::set_maximum_tls_version(tls_version version) {
    _impl->set_maximum_tls_version(version);
}

void tls::certificate_credentials::set_dn_verification_callback(dn_callback cb) {
    _impl->set_dn_verification_callback(std::move(cb));
}

std::optional<std::vector<cert_info>> tls::certificate_credentials::get_cert_info() const noexcept {
    if (_impl == nullptr) {
        return std::nullopt;
    }

    try {
        auto result = _impl->get_x509_info();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<cert_info>> tls::certificate_credentials::get_trust_list_info() const noexcept {
    if (_impl == nullptr) {
        return std::nullopt;
    }

    try {
        auto result = _impl->get_x509_trust_list_info();
        return result;
    } catch (...) {
        return std::nullopt;
    }
}

void tls::certificate_credentials::enable_load_system_trust() {
    _impl->_load_system_trust = true;
}

void tls::certificate_credentials::set_client_auth(client_auth ca) {
    _impl->set_client_auth(ca);
}

void tls::certificate_credentials::set_session_resume_mode(session_resume_mode m) {
    _impl->set_session_resume_mode(m);
}

tls::server_credentials::server_credentials()
    : server_credentials(dh_params{})
{}

tls::server_credentials::server_credentials(shared_ptr<dh_params> dh)
    : server_credentials(*dh)
{}

tls::server_credentials::server_credentials(const dh_params& dh) {
    _impl->dh_params(dh);
}

tls::server_credentials::server_credentials(server_credentials&&) noexcept = default;
tls::server_credentials& tls::server_credentials::operator=(
        server_credentials&&) noexcept = default;

void tls::server_credentials::set_client_auth(client_auth ca) {
    _impl->set_client_auth(ca);
}

namespace tls {

int session_ticket_cb(SSL * s, unsigned char key_name[16],
                      unsigned char iv[EVP_MAX_IV_LENGTH],
                      EVP_CIPHER_CTX * ctx, EVP_MAC_CTX *hctx, int enc);

/**
 * Session wraps an OpenSSL SSL session and context,
 * and is the actual conduit for an TLS/SSL data flow.
 *
 * We use a connected_socket and its sink/source
 * for IO. Note that we need to keep ownership
 * of these, since we handle handshake etc.
 *
 * The implmentation below relies on OpenSSL, for the gnutls implementation
 * see tls.cc and the CMake option 'Seastar_WITH_OSSL'
 */
class session : public enable_shared_from_this<session>, public session_impl {
public:
    using buf_type = temporary_buffer<char>;
    using frag_iter = net::fragment*;

    session(session_type t, shared_ptr<tls::certificate_credentials> creds,
            std::unique_ptr<net::connected_socket_impl> sock, tls_options options = {})
      : _sock(std::move(sock))
      , _creds(creds->_impl)
      , _in(_sock->source())
      , _out(_sock->sink())
      , _in_sem(1)
      , _out_sem(1)
      , _options(std::move(options))
      , _output_pending(make_ready_future<>())
      , _ctx(make_ssl_context(t))
      , _ssl([this]() {
          auto ssl = SSL_new(_ctx.get());
          if (!ssl) {
              throw make_ossl_error("Failed to create SSL session");
          }
          return ssl;
      }())
      , _type(t) {
        if (1 != SSL_set_ex_data(_ssl.get(), SSL_EX_DATA_SESSION, this)) {
            throw make_ossl_error("Failed to set EX data for SSL session");
        }
        bio_ptr in_bio(BIO_new(get_method()));
        bio_ptr out_bio(BIO_new(get_method()));
        if (!in_bio || !out_bio) {
            throw std::runtime_error("Failed to create BIOs");
        }
        if (1 != BIO_ctrl(in_bio.get(), BIO_C_SET_POINTER, 0, this)) {
            throw make_ossl_error("Failed to set bio ptr to in bio");
        }
        if (1 != BIO_ctrl(out_bio.get(), BIO_C_SET_POINTER, 0, this)) {
            throw make_ossl_error("Failed to set bio ptr to out bio");
        }
        // SSL_set_bio transfers ownership of the read and write bios to the SSL
        // instance
        SSL_set_bio(_ssl.get(), in_bio.release(), out_bio.release());

        if (_type == session_type::SERVER) {
            SSL_set_accept_state(_ssl.get());
        } else {
            if (!_options.server_name.empty()) {
                SSL_set_tlsext_host_name(
                  _ssl.get(), _options.server_name.c_str());
            }
            SSL_set_connect_state(_ssl.get());
        }

        if (_type == session_type::CLIENT && !_options.session_resume_data.empty()) {
            auto data_ptr = std::as_const(_options.session_resume_data).data();
            long data_size = _options.session_resume_data.size();
            auto sess = ssl_session_ptr(d2i_SSL_SESSION(nullptr, &data_ptr, data_size));
            if (!sess) {
                throw make_ossl_error("Failed to decode SSL_SESSION data for session resumption");
            }
            if (1 != SSL_set_session(_ssl.get(), sess.get())) {
                throw make_ossl_error("Failed to set SSL_SESSION on SSL for session resumption");
            }
        }
        _options.session_resume_data.clear();
    }

    session(session_type t, shared_ptr<certificate_credentials> creds,
            connected_socket sock,
            tls_options options = {})
            : session(t, std::move(creds), net::get_impl::get(std::move(sock)), options) {}

    ~session() {
        assert(_output_pending.available());
    }

    // This function waits for the _output_pending future to resolve
    // If an error occurs, it is saved off into _error and returned
    future<> wait_for_output() {
        return std::exchange(_output_pending, make_ready_future())
          .handle_exception([this](auto ep) {
              _error = ep;
              return make_exception_future(ep);
          });
    }

    template<std::derived_from<std::exception> T>
    future<>
    handle_output_error(T err) {
        _error = std::make_exception_ptr(err);
        return wait_for_output().then_wrapped([this, err](auto f) {
            try {
                f.get();
                // output was ok/done, just generate error exception
                return make_exception_future(_error);
            } catch(...) {
                std::throw_with_nested(err);
            }
        });
    }

    // Helper function for handling the SSL errors in do_put
    future<stop_iteration> handle_do_put_ssl_err(const int ssl_err) {
        switch(ssl_err) {
        case SSL_ERROR_ZERO_RETURN:
            // Indicates a hang up somewhere
            // Mark _eof and stop iteratio
            _eof = true;
            return make_ready_future<stop_iteration>(stop_iteration::yes);
        case SSL_ERROR_NONE:
            // Should not have been reached in this situation
            // Continue iteration
            return make_ready_future<stop_iteration>(stop_iteration::no);
        case SSL_ERROR_SYSCALL:
        {
            auto err = make_ossl_error("System error encountered during SSL write");
            return handle_output_error(std::move(err)).then([] {
                return stop_iteration::yes;
            });
        }
        case SSL_ERROR_SSL: {
            auto ec = ERR_GET_REASON(ERR_peek_error());
            if (ec == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                // Probably shouldn't have during a write, but
                // let's handle this gracefully
                ERR_clear_error();
                _eof = true;
                return make_ready_future<stop_iteration>(stop_iteration::yes);
            }
            auto err = make_ossl_error(
                "Error occurred during SSL write");
            return handle_output_error(std::move(err)).then([] {
                return stop_iteration::yes;
            });
        }
        default:
        {
            // Some other unhandled situation
            auto err = std::runtime_error(
                "Unknown error encountered during SSL write");
            return handle_output_error(std::move(err)).then([] {
                return stop_iteration::yes;
            });
        }
        }
    }

    // Called post locking of the _out_sem
    // This function takes and holds the sempahore units for _out_sem and
    // will attempt to send the provided packet.  If a renegotiation is needed
    // any unprocessed part of the packet is returned.
    future<net::packet> do_put(net::packet p) {
        if (!connected()) {
            return make_ready_future<net::packet>(std::move(p));
        }
        assert(_output_pending.available());
        return do_with(std::move(p),
            [this](net::packet& p) {
                // This do_until runs until either a renegotiation occurs or the packet is empty
                return do_until(
                    [this, &p] { return eof() || !connected() || p.len() == 0;},
                    [this, &p]() mutable {
                        std::string_view frag_view =
                            {p.fragments().begin()->base, p.fragments().begin()->size};
                        return repeat([this, frag_view, &p]() mutable {
                            if (frag_view.empty()) {
                                return make_ready_future<stop_iteration>(stop_iteration::yes);
                            }
                            size_t bytes_written = 0;
                            auto write_rc = SSL_write_ex(
                                _ssl.get(), frag_view.data(), frag_view.size(), &bytes_written);
                            if (write_rc != 1) {
                                const auto ssl_err = SSL_get_error(_ssl.get(), write_rc);
                                if (ssl_err == SSL_ERROR_WANT_WRITE) {
                                    return wait_for_output().then([] {
                                        return stop_iteration::no;
                                    });
                                } else if (!connected() || ssl_err == SSL_ERROR_WANT_READ) {
                                    return make_ready_future<stop_iteration>(stop_iteration::yes);
                                }
                                return handle_do_put_ssl_err(ssl_err);
                            } else {
                                frag_view.remove_prefix(bytes_written);
                                p.trim_front(bytes_written);
                                return wait_for_output().then([] {
                                    return stop_iteration::no;
                                });
                            }
                        });
                    }
                ).then([&p] {
                    return std::move(p);
                });
            }
        );
    }

    // Used to push unencrypted data through OpenSSL, which will
    // encrypt it and then place it into the output bio.
    future<> put(net::packet p) override {
        constexpr size_t openssl_max_record_size = 16 * 1024;
        if (_error) {
            return make_exception_future(_error);
        }
        if (_shutdown) {
            return make_exception_future<>(
              std::system_error(EPIPE, std::system_category()));
        }
        if (!connected()) {
            return handshake().then(
              [this, p = std::move(p)]() mutable { return put(std::move(p)); });
        }

        // We want to make sure that we write to the underlying bio with as large
        // packets as possible. This is because eventually this translates to a
        // sendmsg syscall. Further it results in larger TLS records which makes
        // encryption/decryption faster. Hence to avoid cases where we would do
        // an extra syscall for something like a 100 bytes header we linearize the
        // packet if it's below the max TLS record size.
        if (p.nr_frags() > 1 && p.len() <= openssl_max_record_size) {
            p.linearize();
        }
        return with_semaphore(_out_sem, 1, [this, p = std::move(p)]() mutable {
            return do_put(std::move(p));
        }).then([this](net::packet p) {
            if (eof() || p.len() == 0) {
                return make_ready_future();
            } else {
                return handshake().then([this, p = std::move(p)]() mutable {
                    return put(std::move(p));
                });
            }
        });
    }

    // Called after locking the _in_sem and _out_sem semaphores.
    // This function will walk through the handshake with a remote peer
    // If EOF is encountered, ENOTCONN is thrown
    future<> do_handshake() {
        if (eof()) {
            // if we have experienced and eof, set the error and return
            // GnuTLS will probably return GNUTLS_E_PREMATURE_TERMINATION
            // from gnutls_handshake in this situation.
            _error = std::make_exception_ptr(std::system_error(
              ENOTCONN,
              std::system_category(),
              "EOF encountered during handshake"));
            return make_exception_future(_error);
        } else if (connected()) {
            return make_ready_future<>();
        }
        return do_until(
            [this] { return connected() || eof(); },
            [this] {
                try {
                    auto n = SSL_do_handshake(_ssl.get());
                    if (n <= 0) {
                        auto ssl_error = SSL_get_error(_ssl.get(), n);
                        switch(ssl_error) {
                        case SSL_ERROR_NONE:
                        // probably shouldn't have gotten here
                            break;
                        case SSL_ERROR_ZERO_RETURN:
                            // peer has closed
                            _eof = true;
                            break;
                        case SSL_ERROR_WANT_WRITE:
                            return wait_for_output();
                        case SSL_ERROR_WANT_READ:
                            return wait_for_output().then([this] {
                                return wait_for_input();
                            });
                        case SSL_ERROR_SYSCALL:
                        {
                            auto err = make_ossl_error("System error during handshake");
                            return handle_output_error(std::move(err));
                        }
                        case SSL_ERROR_SSL:
                        {
                            auto ec = ERR_GET_REASON(ERR_peek_error());
                            switch (ec) {
                            case SSL_R_UNEXPECTED_EOF_WHILE_READING:
                                // well in this situation, the remote end closed
                                ERR_clear_error();
                                _eof = true;
                                return make_ready_future<>();
                            case SSL_R_PEER_DID_NOT_RETURN_A_CERTIFICATE:
                            case SSL_R_CERTIFICATE_VERIFY_FAILED:
                            case SSL_R_NO_CERTIFICATES_RETURNED:
                                ERR_clear_error();
                                verify();
                                // may throw, otherwise fall through
                                [[fallthrough]];
                            default:
                                auto err = make_ossl_error("Failed to establish SSL handshake");
                                return handle_output_error(std::move(err));
                            }
                            break;
                        }
                        default:
                            auto err = std::runtime_error(
                            "Unknown error encountered during handshake");
                            return handle_output_error(std::move(err));
                        }
                    } else {
                        if (_type == session_type::CLIENT
                            || _creds->get_client_auth() != client_auth::NONE) {
                            verify();
                        }
                        return wait_for_output();
                    }
                } catch(...) {
                    return make_exception_future<>(std::current_exception());
                }
                return make_ready_future<>();
            }
        );
    }

    // This function will attempt to pull data off of the _in stream
    // if there isn't already data needing to be processed first.
    future<> wait_for_input() {
        // If we already have data, then it needs to be processed
        if (!_input.empty()) {
            return make_ready_future();
        }
        return _in.get()
          .then([this](buf_type buf) {
              // Set EOF if it's empty
              _eof |= buf.empty();
              _input = std::move(buf);
          })
          .handle_exception([this](auto ep) {
              _error = ep;
              return make_exception_future(ep);
          });
    }

    // Called after locking the _in_sem semaphore
    // This function attempts to pull unencrypted data off of the
    // SSL session using SSL_read.  If ther eis no data, then
    // we will call perform_pull and wait for data to arrive.
    future<buf_type> do_get() {
        // Data is available to be pulled of the SSL session if there is pending
        // data on the SSL session or there is data in the in_bio() which SSL reads
        // from
        auto data_to_pull = (BIO_ctrl_pending(in_bio()) + SSL_pending(_ssl.get())) > 0;
        auto f = make_ready_future<>();
        if (!data_to_pull) {
            // If nothing is in the SSL buffers then we may have to wait for
            // data to come in
            f = wait_for_input();
        }
        return f.then([this] {
            if (eof()) {
                return make_ready_future<buf_type>();
            }
            auto avail = BIO_ctrl_pending(in_bio()) + SSL_pending(_ssl.get());
            buf_type buf(avail);
            size_t bytes_read = 0;
            auto read_result = SSL_read_ex(
              _ssl.get(), buf.get_write(), avail, &bytes_read);
            if (read_result != 1) {
                const auto ssl_err = SSL_get_error(_ssl.get(), read_result);
                switch (ssl_err) {
                case SSL_ERROR_ZERO_RETURN:
                    // Remote end has closed
                    _eof = true;
                    [[fallthrough]];
                case SSL_ERROR_NONE:
                    // well we shouldn't be here at all
                    return make_ready_future<buf_type>();
                case SSL_ERROR_WANT_WRITE:
                    return wait_for_output().then([this] { return do_get(); });
                case SSL_ERROR_WANT_READ:
                    // This may be caused by a renegotiation request, in this situation
                    // return an empty buffer (the get() function will initiate a handshake)
                    return make_ready_future<buf_type>();
                case SSL_ERROR_SYSCALL:
                    if (ERR_peek_error() == 0) {
                        // SSL_get_error
                        // (https://www.openssl.org/docs/man3.0/man3/SSL_get_error.html)
                        // states that on OpenSSL versions prior to 3.0, an
                        // SSL_ERROR_SYSCALL with nothing on the stack and errno
                        // == 0 indicates EOF but future versions should report
                        // SSL_ERROR_SSL with SSL_R_UNEXPECTED_EOF_WHILE_READING
                        // on the stack.  However we are seeing situations on
                        // OpenSSL versions 3.0.9 and 3.0.14 where SSL_ERROR_SYSCALL
                        // is returned and errno == 0 and the stack is empty.
                        // We will treat this as EOF
                        _eof = true;
                        return make_ready_future<buf_type>();
                    }
                    _error = std::make_exception_ptr(
                        make_ossl_error("System error during SSL read"));
                    return make_exception_future<buf_type>(_error);
                case SSL_ERROR_SSL:
                    {
                        auto ec = ERR_GET_REASON(ERR_peek_error());
                        if (ec == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
                            // in this situation, the remote end hung up
                            ERR_clear_error();
                            _eof = true;
                            return make_ready_future<buf_type>();
                        }
                        _error = std::make_exception_ptr(
                          make_ossl_error(
                            "Failure during processing SSL read"));
                        return make_exception_future<buf_type>(_error);
                    }
                default:
                    _error = std::make_exception_ptr(std::runtime_error(
                      "Unexpected error condition during SSL read"));
                    return make_exception_future<buf_type>(_error);
                }
            } else {
                buf.trim(bytes_read);
                return make_ready_future<buf_type>(std::move(buf));
            }
        });
    }

    // Called by user applications to pull data off of the TLS session
    future<buf_type> get() override {
        if (_error) {
            return make_exception_future<buf_type>(_error);
        }
        if (_shutdown || eof()) {
            return make_ready_future<buf_type>(buf_type());
        }
        if (!connected()) {
            return handshake().then(std::bind(&session::get, this));
        }
        return with_semaphore(_in_sem, 1, std::bind(&session::do_get, this))
          .then([this](buf_type buf) {
              if (buf.empty() && !eof()) {
                  return handshake().then(std::bind(&session::get, this));
              }
              return make_ready_future<buf_type>(std::move(buf));
          });
    }

    // Performs shutdown
    future<> do_shutdown() {
        if (_error || !connected()) {
            return make_ready_future();
        }

        auto res = SSL_shutdown(_ssl.get());
        if (res == 1) {
            return wait_for_output();
        } else if (res == 0) {
            return yield().then([this] { return do_shutdown(); });
        } else {
            auto ssl_err = SSL_get_error(_ssl.get(), res);
            switch (ssl_err) {
            case SSL_ERROR_NONE:
                // this is weird, yield and try again
                return yield().then([this] { return do_shutdown(); });
            case SSL_ERROR_ZERO_RETURN:
                // Looks like the other end is done, so let's just assume we're
                // done as well
                return wait_for_output();
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
                return wait_for_output().then([this, ssl_err] {
                    // In neither case do we actually want to pull data off of the socket (yet)
                    // If we initiate the shutdown, then we just send the shutdown alert and wait
                    // for EOF (outside of this function)
                    if (ssl_err == SSL_ERROR_WANT_READ) {
                        return make_ready_future();
                    } else {
                        return do_shutdown();
                    }
                });
            case SSL_ERROR_SYSCALL:
            {
                auto err = make_ossl_error("System error during shutdown");
                return handle_output_error(std::move(err));
            }
            case SSL_ERROR_SSL:
            {
                if (ERR_GET_REASON(ERR_peek_error()) == SSL_R_APPLICATION_DATA_AFTER_CLOSE_NOTIFY) {
                    // This may have resulted in a race condition where we receive a packet immediately after
                    // sending out the close notify alert.  In this situation, retry shutdown silently
                    ERR_clear_error();
                    return yield().then([this] { return do_shutdown(); });
                }
                auto err = make_ossl_error("Error occurred during SSL shutdown");
                return handle_output_error(std::move(err));
            }
            default:
            {
                auto err = std::runtime_error(
                  "Unknown error occurred during SSL shutdown");
                return handle_output_error(std::move(err));
            }
            }
        }
    }

    void verify() {
        // A success return code (0) does not signify if a cert was presented or not, that
        // must be explicitly queried via SSL_get_peer_certificate
        auto res = SSL_get_verify_result(_ssl.get());
        if (res != X509_V_OK) {
            auto stat_str(X509_verify_cert_error_string(res));
            auto dn = extract_dn_information();
            if (dn) {
                std::string_view stat_str_view{stat_str};
                if (stat_str_view.ends_with(" ")) {
                    stat_str_view.remove_suffix(1);
                }
                throw verification_error(fmt::format(
                    R"|({} (Issuer=["{}"], Subject=["{}"]))|",
                    stat_str_view,
                    dn->issuer,
                    dn->subject));
            }
            throw verification_error(stat_str);
        } else if (SSL_get0_peer_certificate(_ssl.get()) == nullptr) {
            // If a peer certificate was not presented,
            // SSL_get_verify_result will return X509_V_OK:
            // https://www.openssl.org/docs/man3.0/man3/SSL_get_verify_result.html
            if (
              _type == session_type::SERVER
              && _creds->get_client_auth() == client_auth::REQUIRE) {
                throw verification_error("no certificate presented by peer");
            }
            return;
        }

        if (_creds->_dn_callback) {
            auto dn = extract_dn_information();
            assert(dn.has_value());
            _creds->_dn_callback(
              _type, std::move(dn->subject), std::move(dn->issuer));
        }
    }

    bool eof() const {
        return _eof;
    }

    bool connected() const {
        return SSL_is_init_finished(_ssl.get());
    }

    // This function waits for eof() to occur on the input stream
    // Unless wait_for_eof_on_shutdown is false
    future<> wait_for_eof() {
        if (!_options.wait_for_eof_on_shutdown) {
            // Seastar option to allow users to just bypass EOF waiting
            return make_ready_future();
        }
        return with_semaphore(_in_sem, 1, [this] {
            if (_error || !connected()) {
                return make_ready_future();
            }
            return do_until(
                [this] { return eof(); },
                [this] { return do_get().discard_result(); });
        });
    }

    // This function is called to kick off the handshake.  It will obtain
    // locks on the _in_sem and _out_sem semaphores and start the handshake.
    future<> handshake() {
        if (_creds->need_load_system_trust()) {
            if (!SSL_CTX_set_default_verify_paths(_ctx.get())) {
                throw make_ossl_error(
                  "Could not load system trust");
            }
            _creds->set_load_system_trust(false);
        }

        return with_semaphore(_in_sem, 1, [this] {
            return with_semaphore(_out_sem, 1, [this] {
                return do_handshake().handle_exception([this](auto ep) {
                    if (!_error) {
                        _error = ep;
                    }
                    return make_exception_future<>(_error);
                });
            });
        });
    }

    future<> shutdown() {
        // first, make sure any pending write is done.
        // bye handshake is a flush operation, but this
        // allows us to not pay extra attention to output state
        //
        // we only send a simple "bye" alert packet. Then we
        // read from input until we see EOF. Any other reader
        // before us will get it instead of us, and mark _eof = true
        // in which case we will be no-op. This is performed all
        // within do_shutdown
        return with_semaphore(_out_sem, 1,
                              std::bind(&session::do_shutdown, this)).then(
                              std::bind(&session::wait_for_eof, this)).finally([me = shared_from_this()] {});
        // note moved finally clause above. It is theorethically possible
        // that we could complete do_shutdown just before the close calls
        // below, get pre-empted, have "close()" finish, get freed, and
        // then call wait_for_eof on stale pointer.
    }

    void close() noexcept override {
        // only do once.
        if (!std::exchange(_shutdown, true)) {
            // running in background. try to bye-handshake us nicely, but after 10s we forcefully close.
            (void)with_timeout(
              timer<>::clock::now() + std::chrono::seconds(10), shutdown())
              .finally([this] {
                  _eof = true;
                  return _in.close();
              }).finally([this] {
                  return _out.close();
              }).finally([this] {
                  // make sure to wait for handshake attempt to leave semaphores. Must be in same order as
                  // handshake aqcuire, because in worst case, we get here while a reader is attempting
                  // re-handshake.
                  return with_semaphore(_in_sem, 1, [this] {
                      return with_semaphore(_out_sem, 1, [] { });
                  });
              }).handle_exception([me = shared_from_this()](std::exception_ptr){
              }).discard_result();
        }
    }
    // helper for sink
    future<> flush() noexcept override {
        return with_semaphore(_out_sem, 1, [this] { return _out.flush(); });
    }

    seastar::net::connected_socket_impl& socket() const override {
        return *_sock;
    }

    future<std::optional<session_dn>> get_distinguished_name() override {
        using result_t = std::optional<session_dn>;
        if (_error) {
            return make_exception_future<result_t>(_error);
        }
        if (_shutdown) {
            return make_exception_future<result_t>(
              std::system_error(ENOTCONN, std::system_category()));
        }
        if (!connected()) {
            return handshake().then(
              [this]() mutable { return get_distinguished_name(); });
        }
        result_t dn = extract_dn_information();
        return make_ready_future<result_t>(std::move(dn));
    }

    future<std::vector<subject_alt_name>> get_alt_name_information(
      std::unordered_set<subject_alt_name_type> types) override {
        using result_t = std::vector<subject_alt_name>;

        if (_error) {
            return make_exception_future<result_t>(_error);
        }
        if (_shutdown) {
            return make_exception_future<result_t>(
              std::system_error(ENOTCONN, std::system_category()));
        }
        if (!connected()) {
            return handshake().then([this, types = std::move(types)]() mutable {
                return get_alt_name_information(std::move(types));
            });
        }

        const auto& peer_cert = get_peer_certificate();
        if (!peer_cert) {
            return make_ready_future<result_t>();
        }
        return make_ready_future<result_t>(
          do_get_alt_name_information(peer_cert, types));
    }

    template<typename Func, typename... Args>
    auto state_checked_access(Func&& f, Args&& ...args) {
        using future_type = typename futurize<std::invoke_result_t<Func, Args...>>::type;
        using result_t = typename future_type::value_type;
        if (_error) {
            return make_exception_future<result_t>(_error);
        }
        if (_shutdown) {
            return make_exception_future<result_t>(std::system_error(ENOTCONN, std::system_category()));
        }
        if (!connected()) {
            return handshake().then([this, f = std::move(f), ...args = std::forward<Args>(args)]() mutable {
                return session::state_checked_access(std::move(f), std::forward<Args>(args)...);
            });
        }
        return futurize_invoke(f, std::forward<Args>(args)...);
    }

    future<bool> is_resumed() override {
        return state_checked_access([this] {
            return SSL_session_reused(_ssl.get()) == 1;
        });
    }

    future<session_data> get_session_resume_data() override {
        return state_checked_access([this] {
            // get0 does not increment reference counter so no clean up necessary
            auto sess = SSL_get0_session(_ssl.get());
            if (!sess || 0 == SSL_SESSION_is_resumable(sess)) {
                return session_data{};
            }
            auto len = i2d_SSL_SESSION(sess, nullptr);
            if (len == 0) {
                return session_data{};
            }
            session_data data(len);
            auto data_ptr = data.data();
            i2d_SSL_SESSION(sess, &data_ptr);
            return data;
        });
    }

private:
    std::vector<subject_alt_name> do_get_alt_name_information(const x509_ptr &peer_cert,
                                                              const std::unordered_set<subject_alt_name_type> &types) const {
        int ext_idx = X509_get_ext_by_NID(
          peer_cert.get(), NID_subject_alt_name, -1);
        if (ext_idx < 0) {
            return {};
        }
        auto ext = X509_get_ext(peer_cert.get(), ext_idx);
        if (!ext) {
            return {};
        }
        auto names = general_names_ptr(static_cast<GENERAL_NAMES*>(X509V3_EXT_d2i(ext)));
        if (!names) {
            return {};
        }
        int num_names = sk_GENERAL_NAME_num(names.get());
        std::vector<subject_alt_name> alt_names;
        alt_names.reserve(num_names);

        for (auto i = 0; i < num_names; i++) {
            GENERAL_NAME* name = sk_GENERAL_NAME_value(names.get(), i);
            if (auto known_t = field_to_san_type(name)) {
                if (types.empty() || types.count(known_t->type)) {
                    alt_names.push_back(std::move(*known_t));
                }
            }
        }
        return alt_names;
    }

    std::optional<subject_alt_name> field_to_san_type(GENERAL_NAME* name) const {
        subject_alt_name san;
        switch(name->type) {
            case GEN_IPADD:
            {
                san.type = subject_alt_name_type::ipaddress;
                const auto* data = ASN1_STRING_get0_data(name->d.iPAddress);
                const auto size = ASN1_STRING_length(name->d.iPAddress);
                if (size == sizeof(::in_addr)) {
                    ::in_addr addr;
                    memcpy(&addr, data, size);
                    san.value = net::inet_address(addr);
                } else if (size == sizeof(::in6_addr)) {
                    ::in6_addr addr;
                    memcpy(&addr, data, size);
                    san.value = net::inet_address(addr);
                } else {
                    throw std::runtime_error(fmt::format("Unexpected size: {} for ipaddress alt name value", size));
                }
                break;
            }
            case GEN_EMAIL:
            {
                san.type = subject_alt_name_type::rfc822name;
                san.value = asn1_str_to_str(name->d.rfc822Name);
                break;
            }
            case GEN_URI:
            {
                san.type = subject_alt_name_type::uri;
                san.value = asn1_str_to_str(name->d.uniformResourceIdentifier);
                break;
            }
            case GEN_DNS:
            {
                san.type = subject_alt_name_type::dnsname;
                san.value = asn1_str_to_str(name->d.dNSName);
                break;
            }
            case GEN_OTHERNAME:
            {
                san.type = subject_alt_name_type::othername;
                san.value = asn1_str_to_str(name->d.dNSName);
                break;
            }
            case GEN_DIRNAME:
            {
                san.type = subject_alt_name_type::dn;
                auto dirname = get_dn_string(name->d.directoryName);
                if (!dirname) {
                    throw std::runtime_error("Expected non null value for SAN dirname");
                }
                san.value = std::move(*dirname);
                break;
            }
            default:
                return std::nullopt;
        }
        return san;
    }

    const x509_ptr& get_peer_certificate() const {
        return _creds->get_last_cert();
    }

    std::optional<session_dn> extract_dn_information() const {
        const auto& peer_cert = get_peer_certificate();
        if (!peer_cert) {
            return std::nullopt;
        }
        auto subject = get_dn_string(X509_get_subject_name(peer_cert.get()));
        auto issuer = get_dn_string(X509_get_issuer_name(peer_cert.get()));
        if (!subject || !issuer) {
            throw make_ossl_error(
              "error while extracting certificate DN strings");
        }
        return session_dn{
          .subject = std::move(*subject), .issuer = std::move(*issuer)};
    }

    ssl_ctx_ptr make_ssl_context(session_type type) {
        auto ssl_ctx = ssl_ctx_ptr(SSL_CTX_new(TLS_method()));
        if (!ssl_ctx) {
            throw make_ossl_error(
              "Failed to initialize SSL context");
        }
        const auto& ck_pair = _creds->get_certkey_pair();
        if (type == session_type::SERVER) {
            if (!ck_pair) {
                throw make_ossl_error(
                  "Cannot start session without cert/key pair for server");
            }
            switch (_creds->get_client_auth()) {
            case client_auth::NONE:
            default:
                SSL_CTX_set_verify(ssl_ctx.get(), SSL_VERIFY_NONE, nullptr);
                break;
            case client_auth::REQUEST:
                SSL_CTX_set_verify(ssl_ctx.get(), SSL_VERIFY_PEER, nullptr);
                break;
            case client_auth::REQUIRE:
                SSL_CTX_set_verify(
                  ssl_ctx.get(),
                  SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                  nullptr);
                break;
            }

            auto options = SSL_OP_ALL | SSL_OP_ALLOW_CLIENT_RENEGOTIATION;
            if (_creds->is_server_precedence_enabled()) {
                options |= SSL_OP_CIPHER_SERVER_PREFERENCE;
            }

            SSL_CTX_set_options(ssl_ctx.get(), options);

            switch(_creds->get_session_resume_mode()) {
                case session_resume_mode::NONE:
                    SSL_CTX_set_session_cache_mode(ssl_ctx.get(), SSL_SESS_CACHE_OFF);
                    break;
                case session_resume_mode::TLS13_SESSION_TICKET:
                    // By default, SSL contexts have server size cache enabled
                    if (1 != SSL_CTX_set_tlsext_ticket_key_evp_cb(ssl_ctx.get(), &session_ticket_cb)) {
                        throw make_ossl_error("Failed to set session ticket callback function");
                    }
                    break;
            }
        } else {
            if (_creds->is_server_precedence_enabled()) {
                SSL_CTX_set_options(ssl_ctx.get(), SSL_OP_CIPHER_SERVER_PREFERENCE);
            }
        }

        auto & min_tls_version = _creds->minimum_tls_version();
        auto & max_tls_version = _creds->maximum_tls_version();

        if (min_tls_version.has_value()) {
            if (!SSL_CTX_set_min_proto_version(ssl_ctx.get(),
                tls_version_to_ossl(*min_tls_version))) {
                throw make_ossl_error(
                    fmt::format("Failed to set minimum TLS version to {}",
                        *min_tls_version));
            }
        }

        if (max_tls_version.has_value()) {
            if (!SSL_CTX_set_max_proto_version(ssl_ctx.get(),
                tls_version_to_ossl(*max_tls_version))) {
                    throw make_ossl_error(
                        fmt::format("Failed to set maximum TLS version to {}",
                            *max_tls_version));
            }
        }

        // This call is required to lower SSL's security level to permit TLSv1.0 and TLSv1.1
        // See https://www.openssl.org/docs/man3.0/man3/SSL_CTX_set_security_level.html
        SSL_CTX_set_security_level(ssl_ctx.get(), 0);

        // Servers must supply both certificate and key, clients may
        // optionally use these
        if (ck_pair) {
            if (!SSL_CTX_use_cert_and_key(
                  ssl_ctx.get(),
                  ck_pair.cert.get(),
                  ck_pair.key.get(),
                  nullptr,
                  1)) {
                throw make_ossl_error(
                  "Failed to load cert/key pair");
            }
        }
        // Increments the reference count of *_creds, now should have a
        // total ref count of two, will be deallocated when both OpenSSL and
        // the certificate_manager call X509_STORE_free
        SSL_CTX_set1_cert_store(ssl_ctx.get(), *_creds);

        if (!_creds->get_cipher_string().empty()) {
            if (SSL_CTX_set_cipher_list(ssl_ctx.get(),
                    _creds->get_cipher_string().c_str()) != 1) {
                throw make_ossl_error(
                    fmt::format(
                        "Failed to set cipher string '{}'", _creds->get_cipher_string()));
            }
        }

        if (!_creds->get_ciphersuites().empty()) {
            if (SSL_CTX_set_ciphersuites(ssl_ctx.get(), _creds->get_ciphersuites().c_str()) != 1) {
                throw make_ossl_error(
                    fmt::format(
                        "Failed to set ciphersuites '{}'", _creds->get_ciphersuites()));
            }
        }

        return ssl_ctx;
    }

    static std::optional<sstring> get_dn_string(X509_NAME* name) {
        auto out = bio_ptr(BIO_new(BIO_s_mem()));
        if (-1 == X509_NAME_print_ex(out.get(), name, 0, ASN1_STRFLGS_RFC2253 | XN_FLAG_SEP_COMMA_PLUS |
                                     XN_FLAG_FN_SN | XN_FLAG_DUMP_UNKNOWN_FIELDS)) {
            return std::nullopt;
        }
        char* bio_ptr = nullptr;
        auto len = BIO_get_mem_data(out.get(), &bio_ptr);
        if (len < 0) {
            throw make_ossl_error("Failed to allocate DN string");
        }
        return sstring(bio_ptr, len);
    }

    size_t in_avail() const { return _input.size(); }

    BIO* in_bio() { return SSL_get_rbio(_ssl.get()); }
    BIO* out_bio() { return SSL_get_wbio(_ssl.get()); }

private:
    std::unique_ptr<net::connected_socket_impl> _sock;
    shared_ptr<tls::certificate_credentials::impl> _creds;
    data_source _in;
    data_sink _out;
    std::exception_ptr _error;

    semaphore _in_sem;
    semaphore _out_sem;
    tls_options _options;

    future<> _output_pending;
    buf_type _input;
    ssl_ctx_ptr _ctx;
    ssl_ptr _ssl;
    session_type _type;
    bool _eof = false;
    bool _shutdown = false;

    friend int bio_write_ex(BIO* b, const char * data, size_t dlen, size_t * written);
    friend int bio_read_ex(BIO* b, char * data, size_t dlen, size_t *readbytes);
    friend long bio_ctrl(BIO * b, int ctrl, long num, void * data);
    friend int session_ticket_cb(SSL*, unsigned char[16], unsigned char[EVP_MAX_IV_LENGTH],
                                 EVP_CIPHER_CTX*, EVP_MAC_CTX*, int);
};

// The following callback function is used whenever session tickets are generated or received by
// the TLS server.  If TLS session resumption is enabled, then an AES and HMAC key are
// generated and stored within the certificate_credentials (which is stored within the TLS session).
// The call back uses these keys to initialize the encryption and MAC operations for both encryption (enc = 1)
// and decryption (enc = 0).  Because the key lives with the certificate_credentials which is passed
// to every instance of an SSL session, the same key can be used over and over again to encrypt/decrypt
// session tickets across multiple instances of server sessions.  For more information see:
// https://docs.openssl.org/3.0/man3/SSL_CTX_set_tlsext_ticket_key_cb/
int session_ticket_cb(SSL * s, unsigned char key_name[16],
                      unsigned char iv[EVP_MAX_IV_LENGTH],
                      EVP_CIPHER_CTX * ctx, EVP_MAC_CTX *hctx, int enc) {
    auto * sess = static_cast<const session *>(SSL_get_ex_data(s, SSL_EX_DATA_SESSION));
    std::span<unsigned char, 16> key_name_span(key_name, 16);
    const auto & gen_key_name = sess->_creds->get_session_ticket_keys().key_name;
    const auto & aes_key = sess->_creds->get_session_ticket_keys().aes_key;
    auto hmac_key_ptr = sess->_creds->get_session_ticket_keys().hmac_key.data();
    auto hmac_key_size = sess->_creds->get_session_ticket_keys().hmac_key.size();
    OSSL_PARAM params[3];
    params[0] = OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY,
                                                  const_cast<unsigned char *>(hmac_key_ptr),
                                                  hmac_key_size);
    params[1] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST,
                                                 const_cast<char *>("sha256"), 0);
    params[2] = OSSL_PARAM_construct_end();

    if (enc) {
        if (RAND_bytes(iv, EVP_MAX_IV_LENGTH) <= 0) {
            return -1;
        }

        std::copy(gen_key_name.begin(), gen_key_name.end(), key_name_span.begin());

        if (EVP_EncryptInit_ex2(ctx, EVP_aes_256_cbc(), aes_key.data(), iv, nullptr) == 0) {
            return -1;
        }

        if (EVP_MAC_CTX_set_params(hctx, params) == 0) {
            return -1;
        }

        return 1;
    } else {
        if (!std::equal(key_name_span.begin(), key_name_span.end(), gen_key_name.begin())) {
            return 0;
        }
        if (EVP_MAC_CTX_set_params(hctx, params) == 0) {
            return -1;
        }

        if (EVP_DecryptInit_ex2(ctx, EVP_aes_256_cbc(), aes_key.data(), iv, nullptr) == 0) {
            return -1;
        }
        return 1;
    }
}


tls::session* unwrap_bio_ptr(void * ptr) {
    return static_cast<tls::session*>(ptr);
}

tls::session* unwrap_bio_ptr(BIO * b) {
    return unwrap_bio_ptr(BIO_get_data(b));
}

/// The 'ioctl' for BIO
long bio_ctrl(BIO * b, int ctrl, long num, void * data) {
    if (BIO_get_init(b) <= 0 && ctrl != BIO_C_SET_POINTER) {
        return 0;
    }

    auto session = unwrap_bio_ptr(b);

    switch(ctrl) {
    case BIO_C_SET_POINTER:
        if (BIO_get_init(b) <= 0) {
            BIO_set_data(b, data);
            BIO_set_init(b, 1);
            return 1;
        } else {
            return 0;
        }
    case BIO_CTRL_GET_CLOSE:
        return BIO_get_shutdown(b);
    case BIO_CTRL_SET_CLOSE:
        BIO_set_shutdown(b, static_cast<int>(num));
        break;

    case BIO_CTRL_DUP:
    case BIO_CTRL_FLUSH:
        return 1;
    case BIO_CTRL_EOF:
        return BIO_test_flags(b, BIO_FLAGS_IN_EOF) != 0;
    case BIO_CTRL_PENDING:
        return static_cast<long>(session->_input.size());
    case BIO_CTRL_WPENDING:
        return session->_output_pending.available() ? 0 : 1;
    default:
        return 0;
    }

    return 0;
}

/// This is called when the BIO is created
///
/// It is important for this to be set, even if it doesn't do anything
/// because the BIO init flag won't get automatically set to '1'.
int bio_create(BIO*) {
    return 1;
}

/// Handles writes to the BIO
///
/// This function will attempt to call _out.put() and store the future in
/// _output_pending.  If _output_pending has not yet resolved, return '0'
/// and set the retry write flag.
int bio_write_ex(BIO* b, const char * data, size_t dlen, size_t * written) {
    auto session = unwrap_bio_ptr(b);
    BIO_clear_retry_flags(b);

    if (!session->_output_pending.available()) {
        BIO_set_retry_write(b);
        return 0;
    }

    try {
        size_t n;

        if (!session->_output_pending.failed()) {
            scattered_message<char> msg;
            msg.append(std::string_view(data, dlen));
            n = msg.size();
            session->_output_pending = session->_out.put(std::move(msg).release());
        }

        if (session->_output_pending.failed()) {
            std::rethrow_exception(session->_output_pending.get_exception());
        }

        if (written != nullptr) {
            *written = n;
        }
        
        return 1;
    } catch(const std::system_error & e) {
        ERR_raise_data(ERR_LIB_SYS, e.code().value(), e.what());
        session->_output_pending = make_exception_future<>(std::current_exception());
    } catch(...) {
        ERR_raise(ERR_LIB_SYS, EIO);
        session->_output_pending = make_exception_future<>(std::current_exception());
    }
    
    return 0;
}

/// Handles reading data from the BIO
///
/// This will check to see if EOF has been reached and set the EOF
/// flag if EOF has been reached.  It will set the retry read flag
/// if no data is available, otherwise it will copy data off of
/// the _input buffer and return it to the caller.
int bio_read_ex(BIO* b, char * data, size_t dlen, size_t *readbytes) {
    auto session = unwrap_bio_ptr(b);
    BIO_clear_retry_flags(b);
    if (session->eof()) {
        BIO_set_flags(b, BIO_FLAGS_IN_EOF);
        return 0;
    }

    if (session->_input.empty()) {
        BIO_set_retry_read(b);
        return 0;
    }

    auto n = std::min(dlen, session->_input.size());
    memcpy(data, session->_input.get(), n);
    session->_input.trim_front(n);
    if (readbytes != nullptr) {
        *readbytes = n;
    }
    
    return 1;
}

/// This function creates the custom BIO method
bio_method_ptr create_bio_method() {
    auto new_index = BIO_get_new_index();
    if (new_index == -1) {
        throw make_ossl_error("Failed to obtain new BIO index");
    }
    bio_method_ptr meth(BIO_meth_new(new_index, "SS-OSSL"));
    if (!meth) {
        throw make_ossl_error("Failed to create new BIO method");
    }

    if (1 != BIO_meth_set_create(meth.get(), bio_create)) {
        throw make_ossl_error("Failed to set the BIO creation method");
    }

    if (1 != BIO_meth_set_ctrl(meth.get(), bio_ctrl)) {
        throw make_ossl_error("Failed to set BIO control method");
    }

    if (1 != BIO_meth_set_write_ex(meth.get(), bio_write_ex)) {
        throw make_ossl_error("Failed to set BIO write_ex method");
    }

    if (1 != BIO_meth_set_read_ex(meth.get(), bio_read_ex)) {
        throw make_ossl_error("Failed to set BIO read_ex method");
    }

    return meth;
}

} // namespace tls

BIO_METHOD* get_method() {
    static thread_local bio_method_ptr method_ptr = [] {
        auto ptr = tls::create_bio_method();
        if (!ptr) {
            throw make_ossl_error("Failed to construct BIO method");
        }
        return ptr;
    }();
    
    return method_ptr.get();
}

future<connected_socket> tls::wrap_client(shared_ptr<certificate_credentials> cred, connected_socket&& s, sstring name) {
    tls_options options{.server_name = std::move(name)};
    return wrap_client(std::move(cred), std::move(s), std::move(options));
}

future<connected_socket> tls::wrap_client(shared_ptr<certificate_credentials> cred, connected_socket&& s, tls_options options) {
    session_ref sess(seastar::make_shared<session>(session_type::CLIENT, std::move(cred), std::move(s),  options));
    connected_socket sock(std::make_unique<tls_connected_socket_impl>(std::move(sess)));
    return make_ready_future<connected_socket>(std::move(sock));
}

future<connected_socket> tls::wrap_server(shared_ptr<server_credentials> cred, connected_socket&& s) {
    session_ref sess(seastar::make_shared<session>(session_type::SERVER, std::move(cred), std::move(s)));
    connected_socket sock(std::make_unique<tls_connected_socket_impl>(std::move(sess)));
    return make_ready_future<connected_socket>(std::move(sock));
}

} // namespace seastar

const int seastar::tls::ERROR_UNKNOWN_COMPRESSION_ALGORITHM = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNSUPPORTED_COMPRESSION_ALGORITHM);
const int seastar::tls::ERROR_UNKNOWN_CIPHER_TYPE = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNKNOWN_CIPHER_TYPE);
const int seastar::tls::ERROR_INVALID_SESSION = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_INVALID_SESSION_ID);
const int seastar::tls::ERROR_UNEXPECTED_HANDSHAKE_PACKET = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNEXPECTED_RECORD);
const int seastar::tls::ERROR_UNKNOWN_CIPHER_SUITE = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNSUPPORTED_PROTOCOL);
const int seastar::tls::ERROR_UNKNOWN_ALGORITHM = ERR_PACK(
  ERR_LIB_RSA, 0, RSA_R_UNKNOWN_ALGORITHM_TYPE);
const int seastar::tls::ERROR_UNSUPPORTED_SIGNATURE_ALGORITHM = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_NO_SUITABLE_SIGNATURE_ALGORITHM);
const int seastar::tls::ERROR_SAFE_RENEGOTIATION_FAILED = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_RENEGOTIATION_MISMATCH);
const int seastar::tls::ERROR_UNSAFE_RENEGOTIATION_DENIED = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNSAFE_LEGACY_RENEGOTIATION_DISABLED);
const int seastar::tls::ERROR_UNKNOWN_SRP_USERNAME = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_INVALID_SRP_USERNAME);
const int seastar::tls::ERROR_PREMATURE_TERMINATION = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNEXPECTED_EOF_WHILE_READING);
// System errors are not ERR_PACK'ed like other errors but instead
// are OR'ed with ((unsigned int)INT_MAX + 1)
const int seastar::tls::ERROR_PUSH = int(ERR_SYSTEM_FLAG | EPIPE);
const int seastar::tls::ERROR_PULL = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_READ_BIO_NOT_SET);
const int seastar::tls::ERROR_UNEXPECTED_PACKET = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNEXPECTED_MESSAGE);
const int seastar::tls::ERROR_UNSUPPORTED_VERSION = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_UNSUPPORTED_SSL_VERSION);
const int seastar::tls::ERROR_NO_CIPHER_SUITES = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_NO_CIPHERS_AVAILABLE);
const int seastar::tls::ERROR_DECRYPTION_FAILED = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_DECRYPTION_FAILED);
const int seastar::tls::ERROR_MAC_VERIFY_FAILED = ERR_PACK(
  ERR_LIB_SSL, 0, SSL_R_DECRYPTION_FAILED_OR_BAD_RECORD_MAC);

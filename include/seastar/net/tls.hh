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
#pragma once

#ifndef SEASTAR_MODULE
#include <functional>
#include <unordered_set>
#include <map>
#include <any>
#include <fmt/format.h>
#endif

#include <seastar/core/future.hh>
#include <seastar/core/internal/api-level.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/util/std-compat.hh>
#include <seastar/util/modules.hh>
#include <seastar/net/api.hh>

namespace seastar {

class socket;

class server_socket;
class connected_socket;
class socket_address;

/**
 * Relatively thin SSL wrapper for socket IO.
 * (Can be expanded to other IO forms).
 *
 * The current underlying mechanism is
 * gnutls, however, all interfaces are kept
 * agnostic, so in theory it could be replaced
 * with OpenSSL or similar.
 *
 */
SEASTAR_MODULE_EXPORT
namespace tls {

    enum class x509_crt_format {
        DER,
        PEM,
    };

    typedef std::basic_string_view<char> blob;

    class session;
    class server_session;
    class server_credentials;
    class certificate_credentials;
    class credentials_builder;

    /**
     * Diffie-Hellman parameters for
     * wire encryption.
     */
    class dh_params {
    public:
        // Key strength
        enum class level {
            LEGACY = 2,
            MEDIUM = 3,
            HIGH = 4,
            ULTRA = 5
        };
        dh_params(level = level::LEGACY);
        // loads a key from data
        dh_params(const blob&, x509_crt_format);
        ~dh_params();

        dh_params(dh_params&&) noexcept;
        dh_params& operator=(dh_params&&) noexcept;

        dh_params(const dh_params&) = delete;
        dh_params& operator=(const dh_params&) = delete;

        /** loads a key from file */
        static future<dh_params> from_file(const sstring&, x509_crt_format);
    private:
        class impl;
        friend class server_credentials;
        friend class certificate_credentials;
        std::unique_ptr<impl> _impl;
    };

    class x509_cert {
        x509_cert(const blob&, x509_crt_format);

        static future<x509_cert> from_file(const sstring&, x509_crt_format);
    private:
        class impl;
        x509_cert(shared_ptr<impl>);
        shared_ptr<impl> _impl;
    };

    class abstract_credentials {
    protected:
        abstract_credentials() = default;
        abstract_credentials(const abstract_credentials&) = default;
        abstract_credentials& operator=(abstract_credentials&) = default;
        abstract_credentials& operator=(abstract_credentials&&) = default;
        virtual ~abstract_credentials() {};
    public:
        virtual void set_x509_trust(const blob&, x509_crt_format) = 0;
        virtual void set_x509_crl(const blob&, x509_crt_format) = 0;
        virtual void set_x509_key(const blob& cert, const blob& key, x509_crt_format) = 0;

        virtual void set_simple_pkcs12(const blob&, x509_crt_format, const sstring& password) = 0;

        virtual future<> set_x509_trust_file(const sstring& cafile, x509_crt_format);
        virtual future<> set_x509_crl_file(const sstring& crlfile, x509_crt_format);
        virtual future<> set_x509_key_file(const sstring& cf, const sstring& kf, x509_crt_format);

        virtual future<> set_simple_pkcs12_file(const sstring& pkcs12file, x509_crt_format, const sstring& password);
    };

    template<typename Base>
    class reloadable_credentials;

    /**
     * Enum like tls::session::type but independent of gnutls headers
     *
     * \warning Uses a different internal encoding than tls::session::type
     */
    enum class session_type {
        CLIENT, SERVER,
    };

    /**
     * Callback prototype for receiving Distinguished Name (DN) information
     *
     * \param type Our own role in the TLS handshake (client vs. server)
     * \param subject The subject DN string
     * \param issuer The issuer DN string
     */
    using dn_callback = noncopyable_function<void(session_type type, sstring subject, sstring issuer)>;

    /**
     * Holds certificates and keys.
     *
     * Typically, credentials are shared for multiple client/server
     * sessions. Changes to the credentials object will affect all
     * sessions instantiated with it.
     * You should probably set it up once, before starting client/server
     * connections.
     */
    class certificate_credentials : public abstract_credentials {
    public:
        certificate_credentials();
        ~certificate_credentials();

        certificate_credentials(certificate_credentials&&) noexcept;
        certificate_credentials& operator=(certificate_credentials&&) noexcept;

        certificate_credentials(const certificate_credentials&) = delete;
        certificate_credentials& operator=(const certificate_credentials&) = delete;

        void set_x509_trust(const blob&, x509_crt_format) override;
        void set_x509_crl(const blob&, x509_crt_format) override;
        void set_x509_key(const blob& cert, const blob& key, x509_crt_format) override;
        void set_simple_pkcs12(const blob&, x509_crt_format, const sstring& password) override;

        /**
         * Loads default system cert trust file
         * into this object.
         */
        future<> set_system_trust();

        // TODO add methods for certificate verification

        /**
         * TLS handshake priority string. See gnutls docs and syntax at
         * https://gnutls.org/manual/html_node/Priority-Strings.html
         *
         * Allows specifying order and allowance for handshake alg.
         */
        void set_priority_string(const sstring&);

        /**
         * Register a callback for receiving Distinguished Name (DN) information
         * during the TLS handshake, extracted from the certificate as sent by the peer.
         *
         * The callback is not invoked in case the peer did not send a certificate.
         * (This could e.g. happen when we are the server, and a client connects while
         * client_auth is not set to REQUIRE.)
         *
         * If, based upon the extracted DN information, you want to abort the handshake,
         * then simply throw an exception (e.g., from the callback) like verification_error.
         *
         * Registering this callback does not bypass the 'standard' certificate verification
         * procedure; instead it merely extracts the DN information from the peer certificate
         * (i.e., the 'leaf' certificate from the chain of certificates sent by the peer)
         * and allows for extra checks.
         *
         * To keep the API simple, you can unregister the callback by means of registering
         * an empty callback, i.e. dn_callback{}
         *
         * The callback prototype is documented in the dn_callback typedef.
         */
        void set_dn_verification_callback(dn_callback);

        /**
         * Optional override to disable certificate verification
         */
        void set_enable_certificate_verification(bool enable);

    private:
        class impl;
        friend class session;
        friend class server_session;
        friend class server_credentials;
        friend class credentials_builder;
        template<typename Base>
        friend class reloadable_credentials;
        shared_ptr<impl> _impl;
    };

    /** Exception thrown on certificate validation error */
    class verification_error : public std::runtime_error {
    public:
        using runtime_error::runtime_error;
    };

    enum class client_auth {
        NONE, REQUEST, REQUIRE
    };

    /**
     * Session resumption support.
     * We only support TLS1.3 session tickets.
    */
    enum class session_resume_mode {
        NONE, TLS13_SESSION_TICKET
    };

    /**
     * Extending certificates and keys for server usage.
     * More probably goes in here...
     */
    class server_credentials : public certificate_credentials {
    public:
        server_credentials();
        server_credentials(shared_ptr<dh_params>);
        server_credentials(const dh_params&);

        server_credentials(server_credentials&&) noexcept;
        server_credentials& operator=(server_credentials&&) noexcept;

        server_credentials(const server_credentials&) = delete;
        server_credentials& operator=(const server_credentials&) = delete;

        void set_client_auth(client_auth);

        /**
         * Sets session resume mode.
         * If session resumption is set to TLS13 session tickets,
         * calling this also functions as key rotation, i.e. creates
         * a new window of TLS session keys.
        */
        void set_session_resume_mode(session_resume_mode);
    };

    class reloadable_credentials_base;
    class credentials_builder;

    using reload_callback = std::function<void(const std::unordered_set<sstring>&, std::exception_ptr)>;
    using reload_callback_ex = std::function<future<>(const credentials_builder&, const std::unordered_set<sstring>&, std::exception_ptr)>;

    /**
     * Intentionally "primitive", and more importantly, copyable
     * container for certificate credentials options.
     * The intendend use case is to be able to use across shards,
     * at, say, initialization of tls objects
     *
     * Note that loading invalid objects (malformed certs etc) will
     * _not_ generate exceptions until, earliest, the build functions
     * are called.
     */
    class credentials_builder : public abstract_credentials {
    public:
        void set_dh_level(dh_params::level = dh_params::level::LEGACY);

        void set_x509_trust(const blob&, x509_crt_format) override ;
        void set_x509_crl(const blob&, x509_crt_format) override;
        void set_x509_key(const blob& cert, const blob& key, x509_crt_format) override;
        void set_simple_pkcs12(const blob&, x509_crt_format, const sstring& password) override;

        future<> set_x509_trust_file(const sstring& cafile, x509_crt_format) override;
        future<> set_x509_crl_file(const sstring& crlfile, x509_crt_format) override;
        future<> set_x509_key_file(const sstring& cf, const sstring& kf, x509_crt_format) override;
        future<> set_simple_pkcs12_file(const sstring& pkcs12file, x509_crt_format, const sstring& password) override;

        future<> set_system_trust();
        void set_client_auth(client_auth);
        void set_priority_string(const sstring&);
        void set_session_resume_mode(session_resume_mode);

        void apply_to(certificate_credentials&) const;

        shared_ptr<certificate_credentials> build_certificate_credentials() const;
        shared_ptr<server_credentials> build_server_credentials() const;

        void rebuild(certificate_credentials&) const;
        void rebuild(server_credentials&) const;

        // same as above, but any files used for certs/keys etc will be watched
        // for modification and reloaded if changed
        future<shared_ptr<certificate_credentials>> build_reloadable_certificate_credentials(reload_callback_ex = {}, std::optional<std::chrono::milliseconds> tolerance = {}) const;
        future<shared_ptr<server_credentials>> build_reloadable_server_credentials(reload_callback_ex = {}, std::optional<std::chrono::milliseconds> tolerance = {}) const;

        future<shared_ptr<certificate_credentials>> build_reloadable_certificate_credentials(reload_callback, std::optional<std::chrono::milliseconds> tolerance = {}) const;
        future<shared_ptr<server_credentials>> build_reloadable_server_credentials(reload_callback, std::optional<std::chrono::milliseconds> tolerance = {}) const;
    private:
        friend class reloadable_credentials_base;

        std::multimap<sstring, std::any> _blobs;
        client_auth _client_auth = client_auth::NONE;
        session_resume_mode _session_resume_mode = session_resume_mode::NONE;
        sstring _priority;
    };

    using session_data = std::vector<uint8_t>;

    /// TLS configuration options
    struct tls_options {
        /// \brief whether to wait for EOF from server on session termination
        bool wait_for_eof_on_shutdown = true;
        /// \brief server name to be used for the SNI TLS extension
        sstring server_name = {};

        /// \brief whether server certificate should be verified. May be set to false
        /// in test environments.
        bool verify_certificate = true;

        /// \brief Optional session resume data. Must be retrieved via
        /// get_session_resume_data below.
        session_data session_resume_data;
    };

    /**
     * Creates a TLS client connection using the default network stack and the
     * supplied credentials.
     * Typically these should contain enough information
     * to validate the remote certificate (i.e. trust info).
     *
     * ATTN: The method is going to be deprecated
     *
     * \param name The expected server name for the remote end point
     */
    /// @{
    [[deprecated("Use overload with tls_options parameter")]]
    future<connected_socket> connect(shared_ptr<certificate_credentials>, socket_address, sstring name);
    [[deprecated("Use overload with tls_options parameter")]]
    future<connected_socket> connect(shared_ptr<certificate_credentials>, socket_address, socket_address local, sstring name);
    /// @}

    /**
     * Creates a TLS client connection using the default network stack and the
     * supplied credentials.
     * Typically these should contain enough information
     * to validate the remote certificate (i.e. trust info).
     *
     * \param options Optional additional session configuration
     */
    /// @{
    future<connected_socket> connect(shared_ptr<certificate_credentials>, socket_address, tls_options option = {});
    future<connected_socket> connect(shared_ptr<certificate_credentials>, socket_address, socket_address local, tls_options options = {});
    /// @}

    /**
     * Creates a socket through which a TLS client connection can be created,
     * using the default network stack and the supplied credentials.
     * Typically these should contain enough information
     * to validate the remote certificate (i.e. trust info).
     *
     * ATTN: The method is going to be deprecated
     *
     * \param name The expected server name for the remote end point
     */
    /// @{
    [[deprecated("Use overload with tls_options parameter")]]
    ::seastar::socket socket(shared_ptr<certificate_credentials>, sstring name);
    /// @}

    /**
     * Creates a socket through which a TLS client connection can be created,
     * using the default network stack and the supplied credentials.
     * Typically these should contain enough information
     * to validate the remote certificate (i.e. trust info).
     *
     * \param options Optional additional session configuration
     */
    /// @{
    ::seastar::socket socket(shared_ptr<certificate_credentials>, tls_options options = {});
    /// @}

    /**
     * Wraps an existing connection in SSL/TLS.
     *
     * ATTN: The method is going to be deprecated
     *
     * \param name The expected server name for the remote end point
     */
    /// @{
    [[deprecated("Use overload with tls_options parameter")]]
    future<connected_socket> wrap_client(shared_ptr<certificate_credentials>, connected_socket&&, sstring name);
    future<connected_socket> wrap_server(shared_ptr<server_credentials>, connected_socket&&);
    /// @}

    /**
     * Wraps an existing connection in SSL/TLS.
     *
     * \param options Optional additional session configuration
     */
    /// @{
    future<connected_socket> wrap_client(shared_ptr<certificate_credentials>, connected_socket&&, tls_options options = {});
    /// @}

    /**
     * Creates a server socket that accepts SSL/TLS clients using default network stack
     * and the supplied credentials.
     * The credentials object should contain certificate info
     * for the server and optionally trust/crl data.
     */
    /// @{
    server_socket listen(shared_ptr<server_credentials>, socket_address sa, listen_options opts = listen_options());
    // Wraps an existing server socket in SSL
    server_socket listen(shared_ptr<server_credentials>, server_socket);
    /// @}

    /**
     * Get distinguished name from the leaf certificate in the certificate chain that
     * the connected peer is using.
     * This function forces the TLS handshake. If the handshake didn't happen before the
     * call to 'get_dn_information' it will be completed when the returned future will become
     * ready.
     * The function returns DN information on success. If the peer didn't send the certificate
     * during the handshake the function returns nullopt. If the socket is not connected the
     * system_error exception will be thrown.
     */
    future<std::optional<session_dn>> get_dn_information(connected_socket& socket);

    /**
     * Subject alt name types.
    */
    enum class subject_alt_name_type {
        dnsname = 1, // string value representing a 'DNS' entry
        rfc822name, // string value representing an 'email' entry
        uri, // string value representing an 'uri' entry
        ipaddress, // inet_address value representing an 'IP' entry
        othername, // string value
        dn, // string value
    };

    // Subject alt name entry
    struct subject_alt_name {
        using value_type = std::variant<
            sstring,
            net::inet_address
        >;
        subject_alt_name_type type;
        value_type value;
    };

    /**
     * Returns the alt name entries of matching types, or all entries if 'types' is empty
     * The values are extracted from the client authentication certificate, if available.
     * If no certificate authentication is used in the connection, en empty list is returned.
     *
     * If the socket is not connected a system_error exception will be thrown.
     * If the socket is not a TLS socket an exception will be thrown.
    */
    future<std::vector<subject_alt_name>> get_alt_name_information(connected_socket& socket, std::unordered_set<subject_alt_name_type> types = {});

    using certificate_data = std::vector<uint8_t>;

    /**
     * Get the raw certificate (chain) that the connected peer is using.
     * This function forces the TLS handshake. If the handshake didn't happen before the
     * call to 'get_peer_certificate_chain' it will be completed when the returned future
     * will become ready.
     * The function returns the certificate chain on success. If the peer didn't send the
     * certificate during the handshake, the function returns an empty certificate chain.
     * If the socket is not connected the system_error exception will be thrown.
     */
    future<std::vector<certificate_data>> get_peer_certificate_chain(connected_socket& socket);

    /**
     * Checks if the socket was connected using session resume.
     * Will force handshake if not already done.
     *
     * If the socket is not connected a system_error exception will be thrown.
     * If the socket is not a TLS socket an exception will be thrown.
    */
    future<bool> check_session_is_resumed(connected_socket& socket);

    /**
     * Get session resume data from a connected client socket. Will force handshake if not already done.
     *
     * If the socket is not connected a system_error exception will be thrown.
     * If the socket is not a TLS socket an exception will be thrown.
     * If no session resumption data is available, returns empty buffer.
     *
     * Note: TLS13 session tickets most of the time require data to have been transferred
     * between client/server. To ensure getting the session data, it is advisable to
     * delay this call to sometime before shutting down/closing the socket.
    */
    future<session_data> get_session_resume_data(connected_socket&);

    std::ostream& operator<<(std::ostream&, const subject_alt_name::value_type&);
    std::ostream& operator<<(std::ostream&, const subject_alt_name&);

    /**
     * Alt name to string.
     * Note: because naming of alternative names is inconsistent between tools,
     * and because openssl is probably more popular when creating certs anyway,
     * this routine will be inconsistent with both gnutls and openssl (though more
     * in line with the latter) and name the constants as follows:
     *
     * dnsname: "DNS"
     * rfc822name: "EMAIL"
     * uri: "URI"
     * ipaddress "IP"
     * othername: "OTHERNAME"
     * dn: "DIRNAME"
    */
    std::string_view format_as(subject_alt_name_type);
    std::ostream& operator<<(std::ostream&, subject_alt_name_type);

    /**
     * Error handling.
     *
     * The error_category instance used by exceptions thrown by TLS
     */
    const std::error_category& error_category();

    /**
     * The more common error codes encountered in TLS.
     * Not an exhaustive list. Add exports as needed.
     */
    extern const int ERROR_UNKNOWN_COMPRESSION_ALGORITHM;
    extern const int ERROR_UNKNOWN_CIPHER_TYPE;
    extern const int ERROR_INVALID_SESSION;
    extern const int ERROR_UNEXPECTED_HANDSHAKE_PACKET;
    extern const int ERROR_UNKNOWN_CIPHER_SUITE;
    extern const int ERROR_UNKNOWN_ALGORITHM;
    extern const int ERROR_UNSUPPORTED_SIGNATURE_ALGORITHM;
    extern const int ERROR_SAFE_RENEGOTIATION_FAILED;
    extern const int ERROR_UNSAFE_RENEGOTIATION_DENIED;
    extern const int ERROR_UNKNOWN_SRP_USERNAME;
    extern const int ERROR_PREMATURE_TERMINATION;
    extern const int ERROR_PUSH;
    extern const int ERROR_PULL;
    extern const int ERROR_UNEXPECTED_PACKET;
    extern const int ERROR_UNSUPPORTED_VERSION;
    extern const int ERROR_NO_CIPHER_SUITES;
    extern const int ERROR_DECRYPTION_FAILED;
    extern const int ERROR_MAC_VERIFY_FAILED;
}
}

template <> struct fmt::formatter<seastar::tls::subject_alt_name_type> : fmt::formatter<string_view> {
    template <typename FormatContext>
    auto format(seastar::tls::subject_alt_name_type type, FormatContext& ctx) const {
        return formatter<string_view>::format(format_as(type), ctx);
    }
};

template <> struct fmt::formatter<seastar::tls::subject_alt_name::value_type> : fmt::formatter<string_view> {
    template <typename FormatContext>
    auto format(const seastar::tls::subject_alt_name::value_type& value, FormatContext& ctx) const {
        return std::visit([&](const auto& v) {
            return fmt::format_to(ctx.out(), "{}", v);
        }, value);
    }
};

template <> struct fmt::formatter<seastar::tls::subject_alt_name> : fmt::formatter<string_view> {
    template <typename FormatContext>
    auto format(const seastar::tls::subject_alt_name& name, FormatContext& ctx) const {
        return fmt::format_to(ctx.out(), "{}={}", name.type, name.value);
    }
};

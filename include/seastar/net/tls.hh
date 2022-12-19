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

#include <functional>
#include <unordered_set>
#include <map>

#include <boost/any.hpp>

#include <seastar/core/future.hh>
#include <seastar/core/internal/api-level.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/util/std-compat.hh>
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
    public:
        virtual ~abstract_credentials() {};

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
    };

    class reloadable_credentials_base;

    using reload_callback = std::function<void(const std::unordered_set<sstring>&, std::exception_ptr)>;

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

        void apply_to(certificate_credentials&) const;

        shared_ptr<certificate_credentials> build_certificate_credentials() const;
        shared_ptr<server_credentials> build_server_credentials() const;

        // same as above, but any files used for certs/keys etc will be watched
        // for modification and reloaded if changed
        future<shared_ptr<certificate_credentials>> build_reloadable_certificate_credentials(reload_callback = {}, std::optional<std::chrono::milliseconds> tolerance = {}) const;
        future<shared_ptr<server_credentials>> build_reloadable_server_credentials(reload_callback = {}, std::optional<std::chrono::milliseconds> tolerance = {}) const;
    private:
        friend class reloadable_credentials_base;

        std::multimap<sstring, boost::any> _blobs;
        client_auth _client_auth = client_auth::NONE;
        sstring _priority;
    };

    /**
     * Creates a TLS client connection using the default network stack and the
     * supplied credentials.
     * Typically these should contain enough information
     * to validate the remote certificate (i.e. trust info).
     *
     * \param name An optional expected server name for the remote end point
     */
    /// @{
    future<connected_socket> connect(shared_ptr<certificate_credentials>, socket_address, sstring name = {});
    future<connected_socket> connect(shared_ptr<certificate_credentials>, socket_address, socket_address local, sstring name = {});
    /// @}

    /**
     * Creates a socket through which a TLS client connection can be created,
     * using the default network stack and the supplied credentials.
     * Typically these should contain enough information
     * to validate the remote certificate (i.e. trust info).
     *
     * \param name An optional expected server name for the remote end point
     */
    /// @{
    ::seastar::socket socket(shared_ptr<certificate_credentials>, sstring name = {});
    /// @}

    /** Wraps an existing connection in SSL/TLS. */
    /// @{
    future<connected_socket> wrap_client(shared_ptr<certificate_credentials>, connected_socket&&, sstring name = {});
    future<connected_socket> wrap_server(shared_ptr<server_credentials>, connected_socket&&);
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
}
}


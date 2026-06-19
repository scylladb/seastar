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

#include <any>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_set>

#include <boost/range/iterator_range.hpp>

#include <seastar/core/future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/tls.hh>
#include <seastar/net/stack.hh>

namespace seastar {

class net::get_impl {
public:
    static std::unique_ptr<connected_socket_impl> get(connected_socket s) {
        return std::move(s._csi);
    }

    static connected_socket_impl* maybe_get_ptr(connected_socket& s) {
        if (s._csi) {
            return s._csi.get();
        }
        return nullptr;
    }
};

struct file_info {
    sstring filename;
    std::chrono::system_clock::time_point modified;
};

struct file_result {
    temporary_buffer<char> buf;
    file_info file;
    operator temporary_buffer<char>&&() && {
        return std::move(buf);
    }
};

future<file_result> read_fully(const sstring& name, const sstring& what);

using buffer_type = std::basic_string<tls::blob::value_type, tls::blob::traits_type, std::allocator<tls::blob::value_type>>;

buffer_type to_buffer(const temporary_buffer<char>& buf);

constexpr auto dh_level_key = std::string_view("dh_level");
constexpr auto x509_trust_key = std::string_view("x509_trust");
constexpr auto x509_crl_key = std::string_view("x509_crl");
constexpr auto x509_key_key = std::string_view("x509_key");
constexpr auto pkcs12_key = std::string_view("pkcs12");
constexpr auto system_trust = std::string_view("system_trust");

struct x509_simple {
    buffer_type data;
    tls::x509_crt_format format;
    file_info file;
};

struct x509_key {
    buffer_type cert;
    buffer_type key;
    tls::x509_crt_format format;
    file_info cert_file;
    file_info key_file;
};

struct pkcs12_simple {
    buffer_type data;
    tls::x509_crt_format format;
    sstring password;
    file_info file;
};

template<typename Blobs, typename Visitor>
void visit_blobs(Blobs& blobs, Visitor&& visitor) {
    auto visit = [&](const std::string_view& key, auto* vt) {
        auto tr = blobs.equal_range(key);
        for (auto& p : boost::make_iterator_range(tr.first, tr.second)) {
            auto* v = std::any_cast<std::decay_t<decltype(*vt)>>(&p.second);
            visitor(key, *v);
        }
    };
    visit(x509_trust_key, static_cast<x509_simple*>(nullptr));
    visit(x509_crl_key, static_cast<x509_simple*>(nullptr));
    visit(x509_key_key, static_cast<x509_key*>(nullptr));
    visit(pkcs12_key, static_cast<pkcs12_simple*>(nullptr));
}

} // namespace seastar

namespace seastar::tls {

/// Abstract interface for DH parameters.
///
/// GnuTLS has a full implementation; OpenSSL's is a stub.
class dh_params_impl {
public:
    virtual ~dh_params_impl() = default;
};


/// Abstract interface for TLS credentials.
///
/// This is the base class for backend-specific certificate_credentials::impl
/// classes. Methods that are specific to one backend are virtual with a
/// default no-op implementation, so each backend only overrides what it
/// supports.
class credentials_impl {
public:
    virtual ~credentials_impl() = default;

    // Common methods — both backends implement these.
    virtual void set_x509_trust(const blob&, x509_crt_format) = 0;
    virtual void set_x509_crl(const blob&, x509_crt_format) = 0;
    virtual void set_x509_key(const blob& cert, const blob& key, x509_crt_format) = 0;
    virtual void set_simple_pkcs12(const blob&, x509_crt_format, const sstring& password) = 0;
    virtual future<> set_system_trust() = 0;
    virtual void set_dn_verification_callback(dn_callback) = 0;
    virtual void set_enable_certificate_verification(bool) = 0;
    virtual void set_client_auth(client_auth) = 0;
    virtual void set_session_resume_mode(session_resume_mode, std::span<const uint8_t> key = {}) = 0;
    virtual void set_alpn_protocols(const std::vector<sstring>&) = 0;
    virtual void set_dh_params(const tls::dh_params&) = 0;

    // GnuTLS-specific — no-op for other backends.
    virtual void set_priority_string(const sstring&) {}

    // OpenSSL-specific — no-op for other backends.
    virtual void set_cipher_string(const sstring&) {}
    virtual void set_ciphersuites(const sstring&) {}
    virtual void enable_server_precedence() {}
    virtual void set_minimum_tls_version(tls_version) {}
    virtual void set_maximum_tls_version(tls_version) {}
    virtual void enable_tls_renegotiation() {}

    // Flag for lazy system trust loading.
    bool _load_system_trust = false;
};

/// Abstract interface for a TLS session.
///
/// This is the primary abstraction that TLS backends (GnuTLS, OpenSSL)
/// implement. Generic wrapper classes delegate all TLS operations
/// through this interface.
class session_impl {
public:
    virtual ~session_impl() = default;
    virtual future<> put(std::span<temporary_buffer<char>> bufs) = 0;
    virtual future<> flush() = 0;
    virtual future<temporary_buffer<char>> get() = 0;
    virtual void close() = 0;
    virtual seastar::net::connected_socket_impl& socket() const = 0;
    virtual future<std::optional<session_dn>> get_distinguished_name() = 0;
    virtual future<std::vector<subject_alt_name>> get_alt_name_information(
        std::unordered_set<subject_alt_name_type> types) = 0;
    virtual future<bool> is_resumed() = 0;
    virtual future<session_data> get_session_resume_data() = 0;
    virtual future<std::vector<certificate_data>> get_peer_certificate_chain() = 0;
    virtual future<std::optional<sstring>> get_selected_alpn_protocol() = 0;
    virtual future<sstring> get_cipher_suite() = 0;
    virtual future<sstring> get_protocol_version() = 0;
    virtual future<> force_rehandshake() = 0;
};

/// Shared-ownership wrapper for session_impl.
///
/// Initiates session close when the last owner is destroyed, since the
/// session cannot be revived from a destructor.
struct session_ref {
    session_ref() = default;
    session_ref(shared_ptr<session_impl> session)
        : _session(std::move(session)) {
    }
    session_ref(session_ref&&) = default;
    session_ref(const session_ref&) = default;
    ~session_ref() {
        if (_session && _session.use_count() == 1) {
            _session->close();
        }
    }

    session_ref& operator=(session_ref&&) = default;
    session_ref& operator=(const session_ref&) = default;

    shared_ptr<session_impl> _session;
};

class tls_connected_socket_impl : public net::connected_socket_impl, public session_ref {
public:
    tls_connected_socket_impl(session_ref&& sess)
        : session_ref(std::move(sess))
    {}

    class source_impl;
    class sink_impl;

    using net::connected_socket_impl::source;
    data_source source() override;
    data_sink sink() override;

    void shutdown_input() override {
        _session->close();
    }
    void shutdown_output() override {
        _session->close();
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
    void set_keepalive_parameters(const net::keepalive_params& p) override {
        _session->socket().set_keepalive_parameters(p);
    }
    net::keepalive_params get_keepalive_parameters() const override {
        return _session->socket().get_keepalive_parameters();
    }
    void set_sockopt(int level, int optname, const void* data, size_t len) override {
        _session->socket().set_sockopt(level, optname, data, len);
    }
    int get_sockopt(int level, int optname, void* data, size_t len) const override {
        return _session->socket().get_sockopt(level, optname, data, len);
    }
    socket_address local_address() const noexcept override {
        return _session->socket().local_address();
    }
    socket_address remote_address() const noexcept override {
        return _session->socket().remote_address();
    }
    future<std::optional<session_dn>> get_distinguished_name() {
        return _session->get_distinguished_name();
    }
    future<std::vector<subject_alt_name>> get_alt_name_information(std::unordered_set<subject_alt_name_type> types) {
        return _session->get_alt_name_information(std::move(types));
    }
    future<std::vector<certificate_data>> get_peer_certificate_chain() {
        return _session->get_peer_certificate_chain();
    }
    future<> wait_input_shutdown() override {
        return _session->socket().wait_input_shutdown();
    }
    future<bool> check_session_is_resumed() {
        return _session->is_resumed();
    }
    future<session_data> get_session_resume_data() {
        return _session->get_session_resume_data();
    }
    future<std::optional<sstring>> get_selected_alpn_protocol() {
        return _session->get_selected_alpn_protocol();
    }
    future<sstring> get_cipher_suite() const {
        return _session->get_cipher_suite();
    }
    future<sstring> get_protocol_version() const {
        return _session->get_protocol_version();
    }
    future<> force_rehandshake() {
        return _session->force_rehandshake();
    }
};

class tls_connected_socket_impl::source_impl: public data_source_impl, public session_ref {
public:
    using session_ref::session_ref;
private:
    future<temporary_buffer<char>> get() override {
        return _session->get();
    }
    future<> close() override {
        _session->close();
        return make_ready_future<>();
    }
};

// Note: source/sink, and by extension, the in/out streams
// produced, cannot exist outside the direct life span of
// the connected_socket itself. This is consistent with
// other sockets in seastar, though I am than less fond of it...
class tls_connected_socket_impl::sink_impl: public data_sink_impl, public session_ref {
public:
    using session_ref::session_ref;
private:
    future<> flush() override {
        return _session->flush();
    }
#if SEASTAR_API_LEVEL >= 9
    future<> put(std::span<temporary_buffer<char>> bufs) override {
        return _session->put(bufs);
    }
#else
    using data_sink_impl::put;
    future<> put(net::packet p) override {
        auto vec = p.release();
        return _session->put(std::span(vec));
    }
#endif
    future<> close() override {
        _session->close();
        return make_ready_future<>();
    }
    bool can_batch_flushes() const noexcept override { return true; }
    void on_batch_flush_error() noexcept override {
        _session->close();
    }
};

inline data_source tls::tls_connected_socket_impl::source() {
    return data_source(std::make_unique<source_impl>(_session));
}

inline data_sink tls::tls_connected_socket_impl::sink() {
    return data_sink(std::make_unique<sink_impl>(_session));
}

class server_session : public net::server_socket_impl {
public:
    server_session(shared_ptr<server_credentials> creds, server_socket sock)
            : _creds(std::move(creds)), _sock(std::move(sock)) {
    }
    future<accept_result> accept() override {
        // We're not actually doing anything very SSL until we get
        // an actual connection. Then we create a "server" session
        // and wrap it up after handshaking.
        return _sock.accept().then([this](accept_result ar) {
            return wrap_server(_creds, std::move(ar.connection)).then([addr = std::move(ar.remote_address)](connected_socket s) {
                return make_ready_future<accept_result>(accept_result{std::move(s), addr});
            });
        });
    }
    void abort_accept() override  {
        _sock.abort_accept();
    }
    socket_address local_address() const override {
        return _sock.local_address();
    }
    void set_listen_backlog(int backlog) override {
        _sock.set_listen_backlog(backlog);
    }
    int get_listen_backlog() const override {
        return _sock.get_listen_backlog();
    }
private:

    shared_ptr<server_credentials> _creds;
    server_socket _sock;
};

class tls_socket_impl : public net::socket_impl {
    shared_ptr<certificate_credentials> _cred;
    tls_options _options;
    ::seastar::socket _socket;
public:
    tls_socket_impl(shared_ptr<certificate_credentials> cred, tls_options options)
            : _cred(cred), _options(std::move(options)), _socket(make_socket()) {
    }
    virtual future<connected_socket> connect(socket_address sa, socket_address local, transport proto = transport::TCP) override {
        return _socket.connect(sa, local, proto).then([cred = std::move(_cred), options = std::move(_options)](connected_socket s) mutable {
            return wrap_client(cred, std::move(s), std::move(options));
        });
    }
    void set_reuseaddr(bool reuseaddr) override {
      _socket.set_reuseaddr(reuseaddr);
    }
    bool get_reuseaddr() const override {
      return _socket.get_reuseaddr();
    }
    virtual void shutdown() override {
        _socket.shutdown();
    }
};

} // namespace seastar::tls

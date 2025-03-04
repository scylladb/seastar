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
#include <chrono>
#endif

#include <seastar/core/sstring.hh>
#include <seastar/core/file.hh>
#include <seastar/net/tls.hh>
#include <seastar/net/stack.hh>
#include <seastar/core/reactor.hh>
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

inline tls::reload_callback_with_creds wrap_reload_callback(tls::reload_callback cb) {
    return [cb{std::move(cb)}](const std::unordered_set<sstring> &files,
                               const tls::certificate_credentials&,
                               std::exception_ptr ep,
                               std::optional<tls::blob>) {
        return cb(files, ep);
    };
}

namespace tls {

class session_impl {
public:
    virtual future<> put(net::packet) = 0;
    virtual future<> flush() noexcept = 0;
    virtual future<temporary_buffer<char>> get() = 0;
    virtual void close() = 0;
    virtual future<std::optional<session_dn>> get_distinguished_name(dn_format) = 0;
    virtual seastar::net::connected_socket_impl & socket() const = 0;
    virtual future<std::vector<subject_alt_name>> get_alt_name_information(std::unordered_set<subject_alt_name_type>) = 0;
    virtual future<bool> is_resumed() = 0;
    virtual future<session_data> get_session_resume_data() = 0;
};

struct session_ref {
    session_ref() = default;
    session_ref(shared_ptr<session_impl> session)
                    : _session(std::move(session)) {
    }
    session_ref(session_ref&&) = default;
    session_ref(const session_ref&) = default;
    ~session_ref() {
        // This is not super pretty. But we take some care to only own sessions
        // through session_ref, and we need to initiate shutdown on "last owner",
        // since we cannot revive the session in destructor.
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
    future<std::optional<session_dn>> get_distinguished_name(dn_format format) {
        return _session->get_distinguished_name(format);
    }
    future<std::vector<subject_alt_name>> get_alt_name_information(std::unordered_set<subject_alt_name_type> types) {
        return _session->get_alt_name_information(std::move(types));
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
    using data_sink_impl::put;
    future<> put(net::packet p) override {
        return _session->put(std::move(p));
    }
    future<> close() override {
        _session->close();
        return make_ready_future<>();
    }
};

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

} // namespace tls


}

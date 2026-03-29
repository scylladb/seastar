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
 * Copyright (C) 2026 Kefu Chai (tchaikov@gmail.com)
 */

#ifdef SEASTAR_HAVE_QUIC

#include <seastar/net/quic.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/core/timer.hh>
#include <seastar/net/api.hh>
#include <seastar/net/socket_defs.hh>
#include <seastar/util/log.hh>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>

#include <cstdarg>
#include <deque>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace seastar::experimental::quic {

static logger quic_log("quic");

__attribute__((format(printf, 2, 3)))
static void ngtcp2_log_printf(void*, const char* fmt, ...) noexcept {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
    quic_log.debug("[ngtcp2] {}", buf);
}

static constexpr size_t max_udp_payload = 1350;
static constexpr size_t recv_buf_size = 65536;

// ALPN identifier used for our raw QUIC transport layer.
// Both endpoints must agree on the same token; any ASCII string works.
static constexpr const char quic_alpn[] = "quic";

// GnuTLS priority string for TLS 1.3-only QUIC sessions.
// QUIC requires TLS 1.3; the compat mode flag is incompatible with QUIC.
static constexpr const char quic_tls_priority[] =
    "NORMAL:-VERS-ALL:+VERS-TLS1.3:"
    "-CIPHER-ALL:+AES-128-GCM:+AES-256-GCM:"
    "+CHACHA20-POLY1305:+AES-128-CCM:"
    "-GROUP-ALL:+GROUP-SECP256R1:+GROUP-X25519:"
    "+GROUP-SECP384R1:+GROUP-SECP521R1:"
    "%DISABLE_TLS13_COMPAT_MODE";

// Utility helpers

static ngtcp2_sockaddr_union to_ngtcp2_addr(const socket_address& sa) {
    ngtcp2_sockaddr_union naddr = {};
    if (sa.addr_length == sizeof(sockaddr_in)) {
        std::memcpy(&naddr.in, &sa.u.in, sizeof(sockaddr_in));
    } else {
        std::memcpy(&naddr.in6, &sa.u.in6, sizeof(sockaddr_in6));
    }
    return naddr;
}

// Returns the canonical on-wire size for a socket_address.
// We derive this from sa_family rather than socket_address::addr_length,
// because addr_length may be sizeof(sockaddr_storage) when the address comes
// from recvmsg (seastar's posix_datagram sets msg_namelen to the buffer size
// before the call but doesn't update addr_length from the kernel result).
static ngtcp2_socklen addr_wire_size(const socket_address& sa) noexcept {
    return sa.u.sa.sa_family == AF_INET6
        ? static_cast<ngtcp2_socklen>(sizeof(sockaddr_in6))
        : static_cast<ngtcp2_socklen>(sizeof(sockaddr_in));
}

// Returns a ngtcp2_path pointing to thread-local storage.
// Safe in seastar's one-thread-per-core model; the path is valid until
// the next call to make_path() on the same shard.
static ngtcp2_path make_path(const socket_address& local,
                             const socket_address& remote) {
    thread_local ngtcp2_sockaddr_union local_su, remote_su;
    local_su = to_ngtcp2_addr(local);
    remote_su = to_ngtcp2_addr(remote);
    ngtcp2_path path = {};
    path.local.addr = &local_su.sa;
    path.local.addrlen = addr_wire_size(local);
    path.remote.addr = &remote_su.sa;
    path.remote.addrlen = addr_wire_size(remote);
    return path;
}

static ngtcp2_tstamp quic_timestamp() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void generate_cid(ngtcp2_cid* cid, size_t len = NGTCP2_MAX_CIDLEN) {
    cid->datalen = len;
    gnutls_rnd(GNUTLS_RND_NONCE, cid->data, len);
}

// credentials_impl

class credentials_impl {
public:
    gnutls_certificate_credentials_t _xcred = nullptr;
    // Session ticket key for TLS 1.3 — required for the server GnuTLS state
    // machine even when GNUTLS_NO_AUTO_SEND_TICKET suppresses auto-sending.
    gnutls_datum_t _session_ticket_key = {nullptr, 0};
    // ALPN tokens advertised during the TLS 1.3 handshake.
    std::vector<std::string> _alpns = {quic_alpn};

    credentials_impl() {
        gnutls_certificate_allocate_credentials(&_xcred);
        gnutls_session_ticket_key_generate(&_session_ticket_key);
    }

    ~credentials_impl() {
        if (_xcred) {
            gnutls_certificate_free_credentials(_xcred);
        }
        if (_session_ticket_key.data) {
            gnutls_memset(_session_ticket_key.data, 0, _session_ticket_key.size);
            gnutls_free(_session_ticket_key.data);
        }
    }

    credentials_impl(credentials_impl&&) = delete;
    credentials_impl& operator=(credentials_impl&&) = delete;

    void set_x509_key_file(const std::string& cert_file,
                           const std::string& key_file) {
        auto rv = gnutls_certificate_set_x509_key_file(
            _xcred, cert_file.c_str(), key_file.c_str(), GNUTLS_X509_FMT_PEM);
        if (rv < 0) {
            throw std::runtime_error(fmt::format(
                "gnutls_certificate_set_x509_key_file: {}",
                gnutls_strerror(rv)));
        }
    }

    void set_x509_trust_file(const std::string& ca_file) {
        auto rv = gnutls_certificate_set_x509_trust_file(
            _xcred, ca_file.c_str(), GNUTLS_X509_FMT_PEM);
        if (rv < 0) {
            throw std::runtime_error(fmt::format(
                "gnutls_certificate_set_x509_trust_file: {}",
                gnutls_strerror(rv)));
        }
    }

    // Disable all certificate verification (for testing only).
    // Install a custom verifier that unconditionally accepts any certificate.
    void set_no_verify() {
        gnutls_certificate_set_verify_function(
            _xcred, [](gnutls_session_t) -> int { return 0; });
    }

    void set_alpn(std::string alpn) { _alpns = {std::move(alpn)}; }
    void add_alpn(std::string alpn) { _alpns.push_back(std::move(alpn)); }

    // Apply this credential to a GnuTLS session and negotiate ALPN.
    void apply_to_session(gnutls_session_t session, bool is_server) const {
        gnutls_credentials_set(session, GNUTLS_CRD_CERTIFICATE, _xcred);
        if (is_server) {
            // Don't require a client certificate.
            gnutls_certificate_server_set_request(session, GNUTLS_CERT_IGNORE);
        }
        // QUIC mandates ALPN negotiation.  Build a datum array from _alpns.
        std::vector<gnutls_datum_t> datums;
        datums.reserve(_alpns.size());
        for (auto& a : _alpns) {
            datums.push_back({
                const_cast<unsigned char*>(
                    reinterpret_cast<const unsigned char*>(a.data())),
                static_cast<unsigned int>(a.size())
            });
        }
        gnutls_alpn_set_protocols(
            session, datums.data(), static_cast<unsigned int>(datums.size()),
            is_server ? (GNUTLS_ALPN_MANDATORY | GNUTLS_ALPN_SERVER_PRECEDENCE)
                      : GNUTLS_ALPN_MANDATORY);
    }
};

// credentials (public API)

credentials::credentials()
    : _impl(std::make_unique<credentials_impl>()) {}

credentials::~credentials() = default;
credentials::credentials(credentials&&) noexcept = default;
credentials& credentials::operator=(credentials&&) noexcept = default;

void credentials::set_x509_key_file(const std::string& cert_file,
                                    const std::string& key_file) {
    _impl->set_x509_key_file(cert_file, key_file);
}

void credentials::set_x509_trust_file(const std::string& ca_file) {
    _impl->set_x509_trust_file(ca_file);
}

void credentials::set_no_verify() {
    _impl->set_no_verify();
}

void credentials::set_alpn(std::string alpn) {
    _impl->set_alpn(std::move(alpn));
}

void credentials::add_alpn(std::string alpn) {
    _impl->add_alpn(std::move(alpn));
}

// stream_impl

class stream_impl : public enable_lw_shared_from_this<stream_impl> {
public:
    int64_t _stream_id;

    std::deque<temporary_buffer<char>> _recv_bufs;
    std::optional<promise<>> _recv_ready;
    bool _recv_fin = false;
    bool _recv_closed = false;

    connection_impl* _conn;

    explicit stream_impl(int64_t stream_id, connection_impl* conn)
        : _stream_id(stream_id), _conn(conn) {}

    void push_recv_data(const uint8_t* data, size_t len) {
        if (len > 0) {
            _recv_bufs.emplace_back(reinterpret_cast<const char*>(data), len);
        }
        wake_reader();
    }

    void signal_recv_fin() {
        _recv_fin = true;
        wake_reader();
    }

    void signal_error() {
        if (_recv_ready) {
            _recv_ready->set_exception(std::make_exception_ptr(
                std::runtime_error("QUIC stream reset")));
            _recv_ready.reset();
        }
    }

private:
    void wake_reader() {
        if (_recv_ready) {
            _recv_ready->set_value();
            _recv_ready.reset();
        }
    }
};

// quic_data_source_impl

class quic_data_source_impl final : public data_source_impl {
    lw_shared_ptr<stream_impl> _stream;

public:
    explicit quic_data_source_impl(lw_shared_ptr<stream_impl> s)
        : _stream(std::move(s)) {}

    future<temporary_buffer<char>> get() override;

    future<> close() override {
        _stream->_recv_closed = true;
        co_return;
    }
};

// quic_data_sink_impl

class quic_data_sink_impl final : public data_sink_impl {
    lw_shared_ptr<stream_impl> _stream;

public:
    explicit quic_data_sink_impl(lw_shared_ptr<stream_impl> s)
        : _stream(std::move(s)) {}

    future<> put(std::span<temporary_buffer<char>> data) override;
    future<> close() override;
};

// connection_impl

class connection_impl : public enable_lw_shared_from_this<connection_impl> {
public:
    ngtcp2_conn* _conn = nullptr;
    gnutls_session_t _tls_session = nullptr;
    // ngtcp2_crypto_gnutls requires a conn_ref on the GnuTLS session so its
    // internal callbacks can find our ngtcp2_conn.  Must outlive _tls_session.
    ngtcp2_crypto_conn_ref _conn_ref = {};

    // Client connections own their channel here; server connections leave this
    // empty and use a pointer to the server's shared channel instead.
    std::optional<net::datagram_channel> _owned_channel;
    // Always valid: points to _owned_channel (client) or server's channel.
    net::datagram_channel* _channel;
    socket_address _local_addr;
    socket_address _remote_addr;

    std::unordered_map<int64_t, lw_shared_ptr<stream_impl>> _streams;
    queue<lw_shared_ptr<stream_impl>> _accept_queue{64};

    std::optional<promise<>> _handshake_done;
    // ALPN token selected by the peer during the TLS 1.3 handshake.
    // Populated in cb_handshake_completed; empty until then.
    std::string _negotiated_alpn;
    bool _closed = false;
    // Serialises all sends on _channel: flush_packets, write_stream_data, and
    // do_close all call send_packet() which acquires this semaphore so only one
    // send is outstanding at a time.
    semaphore _send_sem{1};
    // Prevents redundant flush_packets coroutines from stacking up.
    bool _flush_running = false;
    bool _flush_pending = false;

    timer<> _timer;
    std::vector<uint8_t> _send_buf;
    gate _gate;
    connection_config _config;

    // Signalled when something may have freed up send capacity:
    // either the congestion window grew (ACK received) or the peer extended
    // our flow-control window (MAX_STREAM_DATA / MAX_DATA).
    // write_stream_data co_awaits this when ngtcp2 returns nwrite==0.
    std::optional<promise<>> _write_unblocked;

    void wake_writers() {
        if (_write_unblocked) {
            _write_unblocked->set_value();
            _write_unblocked.reset();
        }
    }

    // Client constructor: owns the channel (used for both send and receive).
    explicit connection_impl(net::datagram_channel channel,
                             socket_address remote)
        : _owned_channel(std::move(channel))
        , _channel(&*_owned_channel)
        , _local_addr(_channel->local_address())
        , _remote_addr(remote)
        , _send_buf(max_udp_payload) {
        _timer.set_callback([this] { on_timer(); });
    }

    // Server connection constructor: shares the server's channel for sends.
    // The caller must ensure the server channel outlives this connection.
    explicit connection_impl(net::datagram_channel& server_channel,
                             socket_address local, socket_address remote)
        : _owned_channel()
        , _channel(&server_channel)
        , _local_addr(local)
        , _remote_addr(remote)
        , _send_buf(max_udp_payload) {
        _timer.set_callback([this] { on_timer(); });
    }

    ~connection_impl() {
        if (_tls_session) {
            gnutls_deinit(_tls_session);
        }
        if (_conn) {
            ngtcp2_conn_del(_conn);
        }
    }

    // ---- ngtcp2 callbacks ----

    static int cb_recv_stream_data(ngtcp2_conn*, uint32_t flags,
                                   int64_t stream_id, uint64_t,
                                   const uint8_t* data, size_t datalen,
                                   void* user_data, void*) {
        auto* self = static_cast<connection_impl*>(user_data);
        if (auto it = self->_streams.find(stream_id);
            it != self->_streams.end()) {
            it->second->push_recv_data(data, datalen);
            if (flags & NGTCP2_STREAM_DATA_FLAG_FIN) {
                it->second->signal_recv_fin();
            }
        }
        return 0;
    }

    static int cb_stream_open(ngtcp2_conn*, int64_t stream_id,
                              void* user_data) {
        auto* self = static_cast<connection_impl*>(user_data);
        auto s = make_lw_shared<stream_impl>(stream_id, self);
        self->_streams[stream_id] = s;
        if (!self->_accept_queue.full()) {
            self->_accept_queue.push(std::move(s));
        }
        return 0;
    }

    static int cb_stream_close(ngtcp2_conn*, uint32_t, int64_t stream_id,
                               uint64_t, void* user_data, void*) {
        auto* self = static_cast<connection_impl*>(user_data);
        if (auto it = self->_streams.find(stream_id);
            it != self->_streams.end()) {
            it->second->signal_recv_fin();
            self->_streams.erase(it);
        }
        return 0;
    }

    static int cb_acked_stream_data_offset(ngtcp2_conn*, int64_t, uint64_t,
                                           uint64_t, void* user_data, void*) {
        // Data was ACKed → the congestion window likely grew.  Wake any
        // writer that was blocked waiting for send capacity.
        static_cast<connection_impl*>(user_data)->wake_writers();
        return 0;
    }

    // Called when the peer extends our send window for a stream
    // (received MAX_STREAM_DATA).  Wake blocked writers.
    static int cb_extend_max_stream_data(ngtcp2_conn*, int64_t,
                                         uint64_t, void* user_data, void*) {
        static_cast<connection_impl*>(user_data)->wake_writers();
        return 0;
    }

    static int cb_handshake_completed(ngtcp2_conn* conn, void* user_data) {
        auto* self = static_cast<connection_impl*>(user_data);
        quic_log.debug("handshake completed ({})", self->_owned_channel ? "client" : "server");
        // Capture the ALPN agreed upon by both sides.
        gnutls_datum_t proto = {};
        auto session = static_cast<gnutls_session_t>(
            ngtcp2_conn_get_tls_native_handle(conn));
        if (gnutls_alpn_get_selected_protocol(session, &proto) == GNUTLS_E_SUCCESS) {
            self->_negotiated_alpn.assign(
                reinterpret_cast<const char*>(proto.data), proto.size);
            quic_log.debug("negotiated ALPN: {}", self->_negotiated_alpn);
        }
        if (self->_handshake_done) {
            self->_handshake_done->set_value();
            self->_handshake_done.reset();
        }
        return 0;
    }

    // Drives the GnuTLS TLS 1.3 handshake: feed incoming crypto bytes into the
    // session and advance gnutls_handshake() until the handshake completes.
    static int cb_recv_crypto_data(ngtcp2_conn* conn,
                                   ngtcp2_encryption_level level, uint64_t,
                                   const uint8_t* data, size_t datalen,
                                   void*) {
        quic_log.debug("cb_recv_crypto_data: level={} datalen={}", (int)level, datalen);
        auto session = static_cast<gnutls_session_t>(
            ngtcp2_conn_get_tls_native_handle(conn));
        auto gtls_level =
            ngtcp2_crypto_gnutls_from_ngtcp2_encryption_level(level);

        if (datalen > 0) {
            auto rv = gnutls_handshake_write(session, gtls_level, data, datalen);
            quic_log.debug("gnutls_handshake_write: rv={} ({})",
                           rv, rv != 0 ? gnutls_strerror(rv) : "ok");
            if (rv != 0) {
                if (!gnutls_error_is_fatal(rv)) {
                    return 0;
                }
                gnutls_alert_send_appropriate(session, rv);
                return NGTCP2_ERR_CRYPTO;
            }
        }

        if (!ngtcp2_conn_get_handshake_completed(conn)) {
            auto rv = gnutls_handshake(session);
            quic_log.debug("gnutls_handshake: rv={} ({})",
                           rv, rv < 0 ? gnutls_strerror(rv) : "ok");
            if (rv < 0) {
                if (!gnutls_error_is_fatal(rv)) {
                    return 0;
                }
                gnutls_alert_send_appropriate(session, rv);
                return NGTCP2_ERR_CRYPTO;
            }
            ngtcp2_conn_tls_handshake_completed(conn);
        }

        return 0;
    }

    static void cb_rand(uint8_t* dest, size_t destlen,
                        const ngtcp2_rand_ctx*) {
        gnutls_rnd(GNUTLS_RND_NONCE, dest, destlen);
    }

    static int cb_get_new_connection_id(ngtcp2_conn*, ngtcp2_cid* cid,
                                        uint8_t* token, size_t cidlen,
                                        void*) {
        generate_cid(cid, cidlen);
        gnutls_rnd(GNUTLS_RND_NONCE, token, NGTCP2_STATELESS_RESET_TOKENLEN);
        return 0;
    }

    // Wire the conn_ref so ngtcp2_crypto_gnutls callbacks can reach _conn.
    // Must be called after _tls_session is initialised and before
    // ngtcp2_crypto_gnutls_configure_{server,client}_session().
    void setup_conn_ref() {
        _conn_ref.get_conn = [](ngtcp2_crypto_conn_ref* ref) -> ngtcp2_conn* {
            return static_cast<connection_impl*>(ref->user_data)->_conn;
        };
        _conn_ref.user_data = this;
        gnutls_session_set_ptr(_tls_session, &_conn_ref);
    }

    ngtcp2_callbacks make_callbacks() {
        ngtcp2_callbacks cb = {};
        cb.recv_crypto_data = cb_recv_crypto_data;
        cb.encrypt = ngtcp2_crypto_encrypt_cb;
        cb.decrypt = ngtcp2_crypto_decrypt_cb;
        cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
        cb.recv_stream_data = cb_recv_stream_data;
        cb.acked_stream_data_offset = cb_acked_stream_data_offset;
        cb.stream_open = cb_stream_open;
        cb.stream_close = cb_stream_close;
        cb.rand = cb_rand;
        cb.get_new_connection_id = cb_get_new_connection_id;
        cb.update_key = ngtcp2_crypto_update_key_cb;
        cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
        cb.delete_crypto_cipher_ctx =
            ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
        cb.get_path_challenge_data =
            ngtcp2_crypto_get_path_challenge_data_cb;
        cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
        cb.handshake_completed = cb_handshake_completed;
        cb.extend_max_stream_data = cb_extend_max_stream_data;
        return cb;
    }

    ngtcp2_settings make_settings() {
        ngtcp2_settings settings;
        ngtcp2_settings_default(&settings);
        settings.initial_ts = quic_timestamp();
        settings.log_printf = ngtcp2_log_printf;
        return settings;
    }

    ngtcp2_transport_params make_transport_params() {
        ngtcp2_transport_params params;
        ngtcp2_transport_params_default(&params);
        params.initial_max_data = _config.initial_max_data;
        params.initial_max_stream_data_bidi_local =
            _config.initial_max_stream_data_bidi_local;
        params.initial_max_stream_data_bidi_remote =
            _config.initial_max_stream_data_bidi_remote;
        params.initial_max_stream_data_uni =
            _config.initial_max_stream_data_uni;
        params.initial_max_streams_bidi = _config.initial_max_streams_bidi;
        params.initial_max_streams_uni = _config.initial_max_streams_uni;
        params.max_idle_timeout =
            _config.max_idle_timeout_ms * NGTCP2_MILLISECONDS;
        return params;
    }

    // ---- I/O ----

    // Serialise a single packet send; acquires _send_sem for the duration.
    future<> send_packet(temporary_buffer<char> tbuf) {
        auto units = co_await get_units(_send_sem, 1);
        co_await _channel->send(
            _remote_addr,
            std::span<temporary_buffer<char>>(&tbuf, 1));
    }

    // Drain the ngtcp2 send queue, serialising all sends.
    // Only one instance runs at a time; if called while a flush is already
    // in progress, the caller sets _flush_pending and the running instance
    // will loop again after finishing.
    future<> flush_packets() {
        if (_flush_running) {
            _flush_pending = true;
            co_return;
        }
        _flush_running = true;
        do {
            _flush_pending = false;
            co_await do_flush_once();
        } while (_flush_pending && !_closed);
        _flush_running = false;
    }

    future<> do_flush_once() {
        if (_closed) {
            co_return;
        }
        auto path = make_path(_local_addr, _remote_addr);
        ngtcp2_pkt_info pi = {};
        int pkt_count = 0;

        for (;;) {
            // Refresh timestamp each iteration: after co_await send, time has
            // advanced and ngtcp2 asserts timestamps are non-decreasing.
            auto ts = quic_timestamp();
            auto nwrite = ngtcp2_conn_write_pkt(
                _conn, &path, &pi, _send_buf.data(), _send_buf.size(), ts);
            if (nwrite < 0) {
                if (nwrite == NGTCP2_ERR_WRITE_MORE) {
                    continue;
                }
                quic_log.warn("ngtcp2_conn_write_pkt: {}",
                              ngtcp2_strerror(static_cast<int>(nwrite)));
                co_return;
            }
            if (nwrite == 0) {
                break;
            }
            ++pkt_count;
            // Copy into a temporary_buffer before co_await so the send owns
            // its own memory and _send_buf is free for the next ngtcp2 call.
            auto tbuf = temporary_buffer<char>(
                reinterpret_cast<const char*>(_send_buf.data()), nwrite);
            try {
                co_await send_packet(std::move(tbuf));
            } catch (...) {
                // Channel closed or broken pipe — stop sending.
                co_return;
            }
        }
        quic_log.debug("flush_packets: sent {} packets to {}", pkt_count, _remote_addr);
        update_timer();
    }

    future<> write_stream_data(int64_t stream_id, const uint8_t* data,
                               size_t len, bool fin) {
        if (_closed) {
            throw std::runtime_error("connection closed");
        }
        auto path = make_path(_local_addr, _remote_addr);
        ngtcp2_pkt_info pi = {};

        size_t total_sent = 0;
        while (total_sent < len || (fin && total_sent == len)) {
            // Fresh timestamp each iteration (see flush_packets comment).
            auto ts = quic_timestamp();
            ngtcp2_vec datav;
            datav.base = const_cast<uint8_t*>(data + total_sent);
            datav.len = len - total_sent;

            ngtcp2_ssize ndatalen = 0;
            auto nwrite = ngtcp2_conn_writev_stream(
                _conn, &path, &pi, _send_buf.data(), _send_buf.size(),
                &ndatalen,
                fin ? NGTCP2_WRITE_STREAM_FLAG_FIN : NGTCP2_WRITE_STREAM_FLAG_NONE,
                stream_id, &datav, 1, ts);

            if (nwrite < 0) {
                if (nwrite == NGTCP2_ERR_STREAM_SHUT_WR) {
                    break;
                }
                throw std::runtime_error(fmt::format(
                    "ngtcp2_conn_writev_stream: {}",
                    ngtcp2_strerror(static_cast<int>(nwrite))));
            }

            if (ndatalen > 0) {
                total_sent += ndatalen;
            }
            if (nwrite > 0) {
                auto tbuf = temporary_buffer<char>(
                    reinterpret_cast<const char*>(_send_buf.data()), nwrite);
                co_await send_packet(std::move(tbuf));
            } else if (ndatalen <= 0) {
                // Blocked by congestion control or flow control: ngtcp2 has
                // nothing to send and couldn't consume any stream data.
                // Wait for capacity — either ACK arrival (cwnd grows, signalled by
                // cb_acked_stream_data_offset) or window extension (signalled by
                // cb_extend_max_stream_data).
                _write_unblocked.emplace();
                co_await _write_unblocked->get_future();
            }
            if (fin && total_sent >= len) {
                break;
            }
        }
        update_timer();
        co_return;
    }

    void feed_packet(const uint8_t* data, size_t len,
                     const socket_address& remote) {
        auto path = make_path(_local_addr, remote);
        ngtcp2_pkt_info pi = {};
        int rv = ngtcp2_conn_read_pkt(_conn, &path, &pi, data, len,
                                      quic_timestamp());
        if (rv != 0) {
            quic_log.warn("ngtcp2_conn_read_pkt: {}", ngtcp2_strerror(rv));
        }
    }

    void update_timer() {
        auto expiry = ngtcp2_conn_get_expiry(_conn);
        _timer.cancel();
        if (expiry == UINT64_MAX) {
            return;
        }
        auto now = quic_timestamp();
        if (expiry <= now) {
            on_timer();
            return;
        }
        _timer.arm(std::chrono::duration_cast<timer<>::duration>(
            std::chrono::nanoseconds(expiry - now)));
    }

    // Notify ngtcp2 that `n` bytes have been consumed from stream `stream_id`.
    // Called from the read path (quic_data_source_impl::get) after the
    // application dequeues data, so the peer receives updated flow-control
    // windows only once data has actually been consumed.
    void extend_stream_window(int64_t stream_id, size_t n) {
        ngtcp2_conn_extend_max_stream_offset(_conn, stream_id, n);
        ngtcp2_conn_extend_max_offset(_conn, n);
    }

    void on_timer() {
        if (_closed) {
            return;
        }
        if (auto rv = ngtcp2_conn_handle_expiry(_conn, quic_timestamp());
            rv != 0) {
            quic_log.warn("ngtcp2_conn_handle_expiry: {}",
                          ngtcp2_strerror(rv));
            return;
        }
        (void)with_gate(_gate, [this] { return flush_packets(); });
    }

    // Client receive loop.  Takes ci by value so the connection_impl stays
    // alive for the loop lifetime and the coroutine frame owns the reference
    // (avoids the lambda-closure use-after-return pitfall).
    static future<> recv_loop(lw_shared_ptr<connection_impl> ci) {
        while (!ci->_closed) {
            std::optional<net::datagram> dgram;
            try {
                dgram.emplace(co_await ci->_owned_channel->receive());
            } catch (...) {
                // shutdown_input() from do_close(); exit.
                break;
            }
            auto peer = dgram->get_src();
            for (auto& buf : dgram->get_buffers()) {
                ci->feed_packet(
                    reinterpret_cast<const uint8_t*>(buf.get()),
                    buf.size(), peer);
            }
            co_await ci->flush_packets();
        }
    }

    future<> do_close() {
        if (_closed) {
            co_return;
        }
        _closed = true;
        _timer.cancel();

        // Best-effort: send a CONNECTION_CLOSE frame before shutting down.
        // Skip if a flush is in progress to avoid a concurrent-send assertion;
        // the peer will time out or detect the closure through other means.
        if (!_flush_running) {
            auto path = make_path(_local_addr, _remote_addr);
            ngtcp2_pkt_info pi = {};
            ngtcp2_ccerr ccerr;
            ngtcp2_ccerr_default(&ccerr);
            auto nwrite = ngtcp2_conn_write_connection_close(
                _conn, &path, &pi, _send_buf.data(), _send_buf.size(), &ccerr,
                quic_timestamp());
            if (nwrite > 0) {
                auto tbuf = temporary_buffer<char>(
                    reinterpret_cast<const char*>(_send_buf.data()), nwrite);
                try {
                    co_await send_packet(std::move(tbuf));
                } catch (...) {
                    // Ignore: peer may already be gone.
                }
            }
        }

        // Unblock the recv loop (client only): shutdown the owned channel so
        // _owned_channel->receive() returns with an exception and the loop exits.
        if (_owned_channel) {
            _owned_channel->shutdown_input();
            _owned_channel->shutdown_output();
        }

        for (auto& [id, s] : _streams) {
            s->signal_recv_fin();
        }
        if (_write_unblocked) {
            _write_unblocked->set_exception(std::make_exception_ptr(
                std::runtime_error("connection closed")));
            _write_unblocked.reset();
        }
        _accept_queue.abort(std::make_exception_ptr(
            std::runtime_error("connection closed")));

        co_await _gate.close();
    }
};

// quic_data_source_impl methods (defined after connection_impl)

future<temporary_buffer<char>> quic_data_source_impl::get() {
    while (_stream->_recv_bufs.empty()) {
        if (_stream->_recv_fin || _stream->_recv_closed) {
            co_return temporary_buffer<char>();
        }
        _stream->_recv_ready.emplace();
        co_await _stream->_recv_ready->get_future();
    }
    auto buf = std::move(_stream->_recv_bufs.front());
    _stream->_recv_bufs.pop_front();
    // Advance the receive window now that the application has consumed
    // this buffer, so the peer can send the corresponding amount of new data.
    _stream->_conn->extend_stream_window(_stream->_stream_id, buf.size());
    (void)_stream->_conn->flush_packets();
    co_return std::move(buf);
}

// quic_data_sink_impl methods

future<> quic_data_sink_impl::put(std::span<temporary_buffer<char>> data) {
    for (auto& buf : data) {
        if (buf.size() > 0) {
            co_await _stream->_conn->write_stream_data(
                _stream->_stream_id,
                reinterpret_cast<const uint8_t*>(buf.get()), buf.size(),
                false);
        }
    }
}

future<> quic_data_sink_impl::close() {
    co_await _stream->_conn->write_stream_data(
        _stream->_stream_id, nullptr, 0, true);
}

// stream

stream::stream(lw_shared_ptr<stream_impl> p) : _impl(std::move(p)) {}
stream::stream(stream&&) noexcept = default;
stream& stream::operator=(stream&&) noexcept = default;
stream::~stream() = default;

int64_t stream::id() const noexcept { return _impl->_stream_id; }

input_stream<char> stream::input() {
    return input_stream<char>(
        data_source(std::make_unique<quic_data_source_impl>(_impl)));
}

output_stream<char> stream::output(size_t buffer_size) {
    return output_stream<char>(
        data_sink(std::make_unique<quic_data_sink_impl>(_impl)), buffer_size);
}

future<> stream::close() {
    co_await _impl->_conn->write_stream_data(
        _impl->_stream_id, nullptr, 0, true);
}

// connection

connection::connection(lw_shared_ptr<connection_impl> p)
    : _impl(std::move(p)) {}
connection::connection(connection&&) noexcept = default;
connection& connection::operator=(connection&&) noexcept = default;
connection::~connection() = default;

future<stream> connection::open_stream() {
    int64_t stream_id;
    if (auto rv = ngtcp2_conn_open_bidi_stream(_impl->_conn, &stream_id,
                                               nullptr);
        rv != 0) {
        co_return coroutine::exception(std::make_exception_ptr(
            std::runtime_error(fmt::format(
                "cannot open stream: {}", ngtcp2_strerror(rv)))));
    }
    auto s = make_lw_shared<stream_impl>(stream_id, _impl.get());
    _impl->_streams[stream_id] = s;
    co_return stream(std::move(s));
}

future<stream> connection::open_uni_stream() {
    int64_t stream_id;
    if (auto rv = ngtcp2_conn_open_uni_stream(_impl->_conn, &stream_id,
                                              nullptr);
        rv != 0) {
        co_return coroutine::exception(std::make_exception_ptr(
            std::runtime_error(fmt::format(
                "cannot open uni stream: {}", ngtcp2_strerror(rv)))));
    }
    auto s = make_lw_shared<stream_impl>(stream_id, _impl.get());
    _impl->_streams[stream_id] = s;
    co_return stream(std::move(s));
}

future<stream> connection::accept_stream() {
    auto s = co_await _impl->_accept_queue.pop_eventually();
    co_return stream(std::move(s));
}

future<> connection::write_stream_data(int64_t stream_id, const uint8_t* data,
                                       size_t len, bool fin) {
    return _impl->write_stream_data(stream_id, data, len, fin);
}

std::string_view connection::negotiated_alpn() const noexcept {
    return _impl->_negotiated_alpn;
}

socket_address connection::remote_address() const noexcept {
    return _impl->_remote_addr;
}

socket_address connection::local_address() const noexcept {
    return _impl->_local_addr;
}

future<> connection::close() { return _impl->do_close(); }

// server::impl

class server::impl {
public:
    net::datagram_channel _channel;
    socket_address _local_addr;
    shared_ptr<credentials> _creds;
    connection_config _config;

    std::unordered_map<std::string, lw_shared_ptr<connection_impl>> _connections;
    queue<lw_shared_ptr<connection_impl>> _accept_queue{64};

    gate _gate;
    bool _closed = false;

    impl(net::datagram_channel channel, socket_address local,
         shared_ptr<credentials> creds, connection_config config)
        : _channel(std::move(channel))
        , _local_addr(local)
        , _creds(std::move(creds))
        , _config(config) {}

    static std::string cid_key(const ngtcp2_cid& cid) {
        return {reinterpret_cast<const char*>(cid.data), cid.datalen};
    }

    void feed_and_flush(lw_shared_ptr<connection_impl> ci,
                        const uint8_t* data, size_t len,
                        const socket_address& remote) {
        ci->feed_packet(data, len, remote);
        (void)with_gate(ci->_gate, [ci] { return ci->flush_packets(); });
    }

    future<> recv_loop() {
        while (!_closed) {
            std::optional<net::datagram> dgram;
            try {
                dgram.emplace(co_await _channel.receive());
            } catch (...) {
                if (_closed) {
                    break;
                }
                throw;
            }
            auto peer = dgram->get_src();
            quic_log.debug("server: received {} buf(s) from {}", dgram->get_buffers().size(), peer);
            for (auto& buf : dgram->get_buffers()) {
                try {
                    handle_packet(reinterpret_cast<const uint8_t*>(buf.get()),
                                  buf.size(), peer);
                } catch (const std::exception& e) {
                    quic_log.warn("handle_packet: {}", e.what());
                }
            }
        }
    }

    void handle_packet(const uint8_t* data, size_t len,
                       const socket_address& remote) {
        ngtcp2_version_cid vc;
        if (ngtcp2_pkt_decode_version_cid(&vc, data, len,
                                          NGTCP2_MAX_CIDLEN) != 0) {
            return;
        }

        ngtcp2_cid dcid;
        ngtcp2_cid_init(&dcid, vc.dcid, vc.dcidlen);

        if (auto it = _connections.find(cid_key(dcid));
            it != _connections.end()) {
            quic_log.debug("handle_packet: existing conn, feed+flush to {}", remote);
            feed_and_flush(it->second, data, len, remote);
            return;
        }

        if (vc.version == 0) {
            return;
        }

        quic_log.debug("handle_packet: new conn from {}", remote);
        auto ci = create_server_connection(data, len, remote, dcid);
        if (!ci) {
            quic_log.warn("handle_packet: create_server_connection failed");
            return;
        }
        _connections[cid_key(dcid)] = ci;
        register_connection_scids(ci);
        quic_log.debug("handle_packet: stored conn, feed+flush");
        feed_and_flush(ci, data, len, remote);
    }

    lw_shared_ptr<connection_impl> create_server_connection(
        const uint8_t* data, size_t len, const socket_address& remote,
        const ngtcp2_cid& dcid) {
        ngtcp2_pkt_hd hd;
        if (ngtcp2_accept(&hd, data, len) < 0) {
            return {};
        }

        // Server connections share the server's channel for sends (no recv
        // loop — the server dispatches all incoming packets itself).
        auto ci = make_lw_shared<connection_impl>(
            _channel, _local_addr, remote);
        ci->_config = _config;

        // GnuTLS session for the QUIC handshake.
        // GNUTLS_ENABLE_EARLY_DATA is required for the TLS 1.3 state machine
        // to advance properly in QUIC mode even if 0-RTT is never used.
        if (gnutls_init(&ci->_tls_session,
                        GNUTLS_SERVER | GNUTLS_ENABLE_EARLY_DATA |
                        GNUTLS_NO_AUTO_SEND_TICKET |
                        GNUTLS_NO_END_OF_EARLY_DATA) != GNUTLS_E_SUCCESS) {
            quic_log.error("gnutls_init failed");
            return {};
        }
        if (gnutls_priority_set_direct(
                ci->_tls_session, quic_tls_priority, nullptr) != GNUTLS_E_SUCCESS) {
            quic_log.error("gnutls_priority_set_direct failed");
            return {};
        }
        // Session ticket key is required for TLS 1.3 even with NO_AUTO_SEND.
        if (gnutls_session_ticket_enable_server(
                ci->_tls_session,
                &_creds->_impl->_session_ticket_key) != GNUTLS_E_SUCCESS) {
            quic_log.error("gnutls_session_ticket_enable_server failed");
            return {};
        }
        // configure_server_session installs ngtcp2 crypto hooks (read_func,
        // secret_func, transport params extension).
        ngtcp2_crypto_gnutls_configure_server_session(ci->_tls_session);
        // Override the default ngtcp2 read_func to also log submission so
        // we can trace handshake progress at the QUIC CRYPTO frame level.
        gnutls_handshake_set_read_function(
            ci->_tls_session,
            [](gnutls_session_t session,
               gnutls_record_encryption_level_t gtls_level,
               gnutls_handshake_description_t htype,
               const void* data, size_t datalen) -> int {
                auto conn_ref = static_cast<ngtcp2_crypto_conn_ref*>(
                    gnutls_session_get_ptr(session));
                auto conn = conn_ref->get_conn(conn_ref);
                auto level = ngtcp2_crypto_gnutls_from_gnutls_record_encryption_level(
                    gtls_level);
                quic_log.debug("read_func: htype={} level={} datalen={}",
                               (int)htype, (int)level, datalen);
                if (htype == GNUTLS_HANDSHAKE_CHANGE_CIPHER_SPEC) {
                    return 0;
                }
                auto rv = ngtcp2_conn_submit_crypto_data(
                    conn, level, static_cast<const uint8_t*>(data), datalen);
                quic_log.debug("read_func: submit rv={}", rv);
                if (rv != 0) {
                    ngtcp2_conn_set_tls_error(conn, rv);
                    return -1;
                }
                return 0;
            });
        _creds->_impl->apply_to_session(ci->_tls_session, /*is_server=*/true);
        ci->setup_conn_ref();

        ngtcp2_cid scid;
        generate_cid(&scid);

        auto callbacks = ci->make_callbacks();
        callbacks.recv_client_initial =
            ngtcp2_crypto_recv_client_initial_cb;
        auto settings = ci->make_settings();
        auto params = ci->make_transport_params();
        // RFC 9001: server MUST echo the original DCID from the client's
        // Initial packet in transport parameters.
        params.original_dcid_present = 1;
        params.original_dcid = hd.dcid;
        auto path = make_path(_local_addr, remote);

        if (ngtcp2_conn_server_new(&ci->_conn, &hd.scid, &scid, &path,
                                   hd.version, &callbacks, &settings,
                                   &params, nullptr, ci.get()) != 0) {
            quic_log.error("ngtcp2_conn_server_new failed");
            return {};
        }
        ngtcp2_conn_set_tls_native_handle(ci->_conn, ci->_tls_session);

        ci->_handshake_done.emplace();
        // Enqueue the connection after handshake completes.  Handle exceptions
        // (e.g. connection dropped before handshake) so the returned future is
        // always resolved with value and safe to discard.
        (void)ci->_handshake_done->get_future()
            .then([this, weak = lw_shared_ptr<connection_impl>(ci)] {
                if (!_accept_queue.full()) {
                    _accept_queue.push(lw_shared_ptr<connection_impl>(weak));
                }
            })
            .handle_exception([](std::exception_ptr) {
                // Connection abandoned before handshake — nothing to do.
            });

        return ci;
    }

    // Register a connection under all its current server-side SCIDs so that
    // packets arriving with DCID = server_scid (as the client does after
    // receiving our Initial) are dispatched to the right connection_impl.
    void register_connection_scids(lw_shared_ptr<connection_impl> ci) {
        auto n = ngtcp2_conn_get_scid(ci->_conn, nullptr);
        if (n == 0) {
            return;
        }
        std::vector<ngtcp2_cid> scids(n);
        ngtcp2_conn_get_scid(ci->_conn, scids.data());
        for (auto& scid : scids) {
            _connections[cid_key(scid)] = ci;
        }
    }

    future<> do_close() {
        _closed = true;
        // Unblock the recv_loop waiting in _channel.receive().
        _channel.shutdown_input();
        _channel.shutdown_output();
        _accept_queue.abort(std::make_exception_ptr(
            std::runtime_error("server closed")));
        for (auto& [key, c] : _connections) {
            (void)c->do_close();
        }
        co_await _gate.close();
    }
};

// server

server::server(std::unique_ptr<impl> p) : _impl(std::move(p)) {}
server::server(server&&) noexcept = default;
server& server::operator=(server&&) noexcept = default;
server::~server() = default;

future<server> server::listen(socket_address addr,
                              shared_ptr<credentials> creds,
                              connection_config config) {
    auto channel = make_bound_datagram_channel(addr);
    auto local = channel.local_address();

    auto srv = std::make_unique<impl>(std::move(channel), local,
                                      std::move(creds), config);
    auto* raw = srv.get();
    (void)with_gate(raw->_gate, [raw] { return raw->recv_loop(); });

    co_return server(std::move(srv));
}

future<connection> server::accept() {
    auto ci = co_await _impl->_accept_queue.pop_eventually();
    co_return connection(std::move(ci));
}

future<> server::close() { return _impl->do_close(); }

socket_address server::local_address() const noexcept {
    return _impl->_local_addr;
}

// Client connect

future<connection> connect(socket_address addr, shared_ptr<credentials> creds,
                           connection_config config) {
    const socket_address local_any = addr.u.sa.sa_family == AF_INET
        ? socket_address(ipv4_addr{0})
        : socket_address(ipv6_addr{"::", 0});
    auto channel = make_bound_datagram_channel(local_any);

    auto ci = make_lw_shared<connection_impl>(std::move(channel), addr);
    ci->_config = config;

    // GnuTLS client session.
    if (auto rv = gnutls_init(&ci->_tls_session,
                              GNUTLS_CLIENT | GNUTLS_ENABLE_EARLY_DATA |
                              GNUTLS_NO_END_OF_EARLY_DATA);
        rv != GNUTLS_E_SUCCESS) {
        co_return coroutine::exception(std::make_exception_ptr(
            std::runtime_error(
                fmt::format("gnutls_init: {}", gnutls_strerror(rv)))));
    }

    if (auto rv = gnutls_priority_set_direct(
            ci->_tls_session, quic_tls_priority, nullptr);
        rv != GNUTLS_E_SUCCESS) {
        co_return coroutine::exception(std::make_exception_ptr(
            std::runtime_error(fmt::format(
                "gnutls_priority_set_direct: {}", gnutls_strerror(rv)))));
    }

    // configure_client_session installs ngtcp2 crypto hooks first, then
    // set the conn_ref pointer that those hooks will use.
    ngtcp2_crypto_gnutls_configure_client_session(ci->_tls_session);
    creds->_impl->apply_to_session(ci->_tls_session, /*is_server=*/false);
    ci->setup_conn_ref();

    ngtcp2_cid scid, dcid;
    generate_cid(&scid);
    generate_cid(&dcid);

    auto callbacks = ci->make_callbacks();
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_retry      = ngtcp2_crypto_recv_retry_cb;
    auto settings = ci->make_settings();
    auto params = ci->make_transport_params();
    auto path = make_path(ci->_local_addr, addr);

    if (auto rv = ngtcp2_conn_client_new(&ci->_conn, &dcid, &scid, &path,
                                         NGTCP2_PROTO_VER_V1, &callbacks,
                                         &settings, &params, nullptr,
                                         ci.get());
        rv != 0) {
        co_return coroutine::exception(std::make_exception_ptr(
            std::runtime_error(fmt::format(
                "ngtcp2_conn_client_new: {}", ngtcp2_strerror(rv)))));
    }

    ngtcp2_conn_set_tls_native_handle(ci->_conn, ci->_tls_session);

    ci->_handshake_done.emplace();
    auto handshake_fut = ci->_handshake_done->get_future();

    (void)ci->flush_packets();

    // Client receive loop — runs as a free coroutine (not a lambda) so that
    // ci is passed by value as a function parameter and lives in the
    // coroutine heap frame from the start.  Using a lambda coroutine here
    // would store the closure by pointer in the heap frame; when the lambda
    // is invoked inside with_gate the closure is on connect()'s C stack, and
    // after the first co_await that stack is gone (use-after-return).
    (void)with_gate(ci->_gate,
                    [ci]() mutable { return connection_impl::recv_loop(std::move(ci)); });

    co_await std::move(handshake_fut);
    co_return connection(std::move(ci));
}

} // namespace seastar::experimental::quic

#endif // SEASTAR_HAVE_QUIC

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
 * Copyright (C) 2026 ScyllaDB Ltd.
 */

#include <seastar/core/app-template.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/future.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/when_all.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/condition-variable.hh>

#include <seastar/net/api.hh>

#include <gnutls/gnutls.h>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_gnutls.h>

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <array>
#include <vector>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

// ---------------- Global Variables ----------------
static constexpr const char* SERVER_HOST = "localhost";
static constexpr const char* SERVER_IP   = "::1";
static constexpr uint16_t    SERVER_PORT = 4444;
static constexpr size_t      MAX_UDP_OUT = 65536;
static constexpr uint64_t IDLE_TIMEOUT = 60000000000; // 60s idle timeout

#define LOG(x) do { std::cerr << x << "\n"; } while (0)

static ngtcp2_tstamp now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

// ---------------- ngtcp2 memory ----------------
static void* my_malloc(size_t size, void*) { return std::malloc(size); }
static void  my_free(void *ptr, void*)     { std::free(ptr); }
static void* my_calloc(size_t n, size_t s, void*) { return std::calloc(n, s); }
static void* my_realloc(void *p, size_t s, void*) { return std::realloc(p, s); }
static const ngtcp2_mem g_mem = { nullptr, my_malloc, my_free, my_calloc, my_realloc };

// ---------------- helpers ----------------
static void init_ngtcp2_addr(ngtcp2_addr* addr, const sockaddr* sa, size_t len) {
    addr->addr    
= const_cast<sockaddr*>(sa);
    addr->addrlen = (socklen_t)len;
}

static void sa_to_storage_v6(const seastar::socket_address& sa, sockaddr_storage& out, socklen_t& outlen) {
    std::memset(&out, 0, sizeof(out));
    auto in6 = sa.as_posix_sockaddr_in6();
    outlen = sizeof(sockaddr_in6);
    std::memcpy(&out, &in6, sizeof(sockaddr_in6));
}

static seastar::temporary_buffer<char> packet_to_tb(seastar::net::packet& pkt) {
    const size_t n = pkt.len();
    seastar::temporary_buffer<char> tb(n);
    char* dst = tb.get_write();
    size_t off = 0;
    for (auto& frag : pkt.fragments()) {
        std::memcpy(dst + off, frag.base, frag.size);
        off += frag.size;
    }
    return tb;
}

// Datagram Socket - datagram channel, socket address local and remote.
class SeastarDgramSocket {
public:
    SeastarDgramSocket(seastar::net::datagram_channel ch,
                       seastar::socket_address local,
                       seastar::socket_address remote)
        : ch_(std::move(ch)), local_(local), remote_(remote) {}

    seastar::future<> send_to(const seastar::socket_address& to, seastar::temporary_buffer<char> tb) {
        return ch_.send(to, std::move(tb));
    }
    seastar::future<seastar::net::datagram> recv_one() { return ch_.receive(); }

    void shutdown() { ch_.close(); }

    const seastar::socket_address& local_address()  const { return local_; }
    const seastar::socket_address& remote_address() const { return remote_; }

private:
    seastar::net::datagram_channel ch_;
    seastar::socket_address local_;
    seastar::socket_address remote_;
};

// Client structure.
struct Client {
    ngtcp2_conn* conn = nullptr;

    gnutls_certificate_credentials_t cred = nullptr;
    gnutls_session_t tls = nullptr;
    ngtcp2_crypto_conn_ref conn_ref{};

    bool closing = false;
    bool handshake_done = false;

    int64_t stream_id = -1;
    bool stream_opened = false;

    sockaddr_storage local_ss{};
    socklen_t        local_ss_len = 0;
    sockaddr_storage remote_ss{};
    socklen_t        remote_ss_len = 0;

    seastar::condition_variable timer_cv;

    // Used to give signal after sending datagram and wake up timer_loop.
    void reschedule() {
        timer_cv.signal();
    }

    ~Client() {
        timer_cv.broken();
        if (conn) ngtcp2_conn_del(conn);
        if (tls)  gnutls_deinit(tls);
        if (cred) gnutls_certificate_free_credentials(cred);
    }

    void fill_path(ngtcp2_path& p) {
        init_ngtcp2_addr(&p.local,  (sockaddr*)&local_ss,  local_ss_len);
        init_ngtcp2_addr(&p.remote, (sockaddr*)&remote_ss, remote_ss_len);
    }
};

using client_ptr = seastar::lw_shared_ptr<Client>;
using sock_ptr   = seastar::lw_shared_ptr<SeastarDgramSocket>;

// ---------------- Callbacks ----------------
static ngtcp2_conn* get_conn(ngtcp2_crypto_conn_ref* conn_ref) {
    return (ngtcp2_conn*)conn_ref->user_data;
}

static void rand_cb(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx*) {
    for (size_t i = 0; i < destlen; ++i) dest[i] = (uint8_t)std::rand();
}

static int get_new_connection_id_cb(ngtcp2_conn*, ngtcp2_cid *cid, uint8_t *token,
                                    size_t cidlen, void*) {
    cid->datalen = cidlen;
    for (size_t i = 0; i < cidlen; ++i) cid->data[i] = (uint8_t)std::rand();
    for (size_t i = 0; i < NGTCP2_STATELESS_RESET_TOKENLEN; ++i) token[i] = (uint8_t)std::rand();
    return 0;
}

static int get_path_challenge_data_cb(ngtcp2_conn*, uint8_t *data, void*) {
    for (size_t i = 0; i < 8; ++i) data[i] = (uint8_t)std::rand();
    return 0;
}

static int handshake_completed_cb(ngtcp2_conn*, void* user_data) {
    auto* c = static_cast<Client*>(user_data);
    c->handshake_done = true;
    LOG("[Client] Handshake completed. Write text and press ENTER:");
    return 0;
}

static int recv_stream_data_cb(ngtcp2_conn*, uint32_t, int64_t,
                               uint64_t, const uint8_t* data, size_t datalen,
                               void*, void*) {
    std::cout << "[Server]: ";
    std::cout.write((const char*)data, (std::streamsize)datalen);
    std::cout.flush();
    return 0;
}

// Function initializing gnutls.
static void init_tls(Client& c) {
    int grv = gnutls_certificate_allocate_credentials(&c.cred);
    if (grv < 0) throw std::runtime_error(gnutls_strerror(grv));

    grv = gnutls_init(&c.tls, GNUTLS_CLIENT | GNUTLS_ENABLE_EARLY_DATA);
    if (grv < 0) throw std::runtime_error(gnutls_strerror(grv));

    grv = gnutls_credentials_set(c.tls, GNUTLS_CRD_CERTIFICATE, c.cred);
    if (grv < 0) throw std::runtime_error(gnutls_strerror(grv));

    const char* errpos = nullptr;
    grv = gnutls_priority_set_direct(c.tls, "NORMAL:-VERS-ALL:+VERS-TLS1.3", &errpos);
    if (grv < 0) throw std::runtime_error("priority: " + std::string(gnutls_strerror(grv)));

    const gnutls_datum_t alpns[] = {
        {(unsigned char*)"hq-interop", 9},
        {(unsigned char*)"h3", 2},
    };
    gnutls_alpn_set_protocols(c.tls, alpns, 2, 0);

    gnutls_server_name_set(c.tls, GNUTLS_NAME_DNS, SERVER_HOST, std::strlen(SERVER_HOST));

    if (ngtcp2_crypto_gnutls_configure_client_session(c.tls) != 0) {
        throw std::runtime_error("ngtcp2_crypto_gnutls_configure_client_session failed");
    }

    c.conn_ref.get_conn  = get_conn;
    c.conn_ref.user_data = nullptr;
    gnutls_session_set_ptr(c.tls, &c.conn_ref);
}

static void make_rand_cid(ngtcp2_cid& cid, size_t len) {
    cid.datalen = len;
    for (size_t i = 0; i < len; ++i) cid.data[i] = (uint8_t)std::rand();
}

// Function initializing ngtcp2.
static void init_ngtcp2(Client& c) {
    ngtcp2_callbacks callbacks{};
    callbacks.client_initial = ngtcp2_crypto_client_initial_cb;
    callbacks.recv_retry = ngtcp2_crypto_recv_retry_cb;
    callbacks.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    callbacks.encrypt = ngtcp2_crypto_encrypt_cb;
    callbacks.decrypt = ngtcp2_crypto_decrypt_cb;
    callbacks.hp_mask = ngtcp2_crypto_hp_mask_cb;
    callbacks.update_key = ngtcp2_crypto_update_key_cb;
    callbacks.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    callbacks.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    callbacks.rand = rand_cb;
    callbacks.get_new_connection_id = get_new_connection_id_cb;
    callbacks.get_path_challenge_data = get_path_challenge_data_cb;
    callbacks.recv_stream_data = recv_stream_data_cb;
    callbacks.handshake_completed = handshake_completed_cb;

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = now_ns();

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.initial_max_stream_data_bidi_local = 256 * 1024;
    params.initial_max_stream_data_bidi_remote = 256 * 1024;
    params.initial_max_data = 4 * 1024 * 1024;
    params.initial_max_streams_bidi = 128;
    params.max_idle_timeout = IDLE_TIMEOUT;

    ngtcp2_cid dcid{}, scid{};
    make_rand_cid(dcid, 8);
    make_rand_cid(scid, 8);

    ngtcp2_path path{};
    c.fill_path(path);

    int rv = ngtcp2_conn_client_new(&c.conn, &dcid, &scid, &path,
                                    NGTCP2_PROTO_VER_V1,
                                    &callbacks, &settings, &params,
                                    &g_mem, &c);
    if (rv != 0) throw std::runtime_error(std::string("client_new: ") + ngtcp2_strerror(rv));

    ngtcp2_conn_set_tls_native_handle(c.conn, c.tls);
    c.conn_ref.user_data = c.conn;
}

// Checking if there are any data to send by ngtcp2_conn_write_pkt and sending it if needed.
static seastar::future<> send_pending_pkts(client_ptr c, sock_ptr sock) {
    if (!c || !sock || !c->conn || c->closing) co_return;

    std::array<uint8_t, MAX_UDP_OUT> outbuf{};
    
    while (true) {
        ngtcp2_path path{};
        ngtcp2_pkt_info pi{};
        std::memset(&pi, 0, sizeof(pi));
        c->fill_path(path);

        ngtcp2_ssize nwrite =
            ngtcp2_conn_write_pkt(c->conn, &path, &pi, outbuf.data(), outbuf.size(), now_ns());

        if (nwrite == 0) co_return;
        
        if (nwrite < 0) {
            if (nwrite != NGTCP2_ERR_DRAINING && nwrite != NGTCP2_ERR_WRITE_MORE) {
                LOG("[client] write_pkt: " << ngtcp2_strerror((int)nwrite));
            }
            co_return;
        }

        auto tb = seastar::temporary_buffer<char>((size_t)nwrite);
        std::memcpy(tb.get_write(), outbuf.data(), (size_t)nwrite);
        co_await sock->send_to(sock->remote_address(), std::move(tb));
    }
}

// Opening stream.
static void try_open_stream(Client& c) {
    if (c.stream_opened) return;
    int rv = ngtcp2_conn_open_bidi_stream(c.conn, &c.stream_id, nullptr);
    if (rv == 0) {
        c.stream_opened = true;
        LOG("[Client] opened stream id=" << c.stream_id);
        return;
    }
    if (rv == NGTCP2_ERR_STREAM_ID_BLOCKED) {
        return;
    }
    LOG("[Client] open_bidi_stream failed: " << ngtcp2_strerror(rv));
}

static seastar::future<> send_message(client_ptr c, sock_ptr sock, std::string msg) {
    if (!c->handshake_done || !c->conn || c->closing) co_return;

    try_open_stream(*c);
    if (!c->stream_opened) {
        LOG("[Client] Stream not opened");
        co_return;
    }

    std::array<uint8_t, MAX_UDP_OUT> outbuf{};

    ngtcp2_path path{};
    ngtcp2_pkt_info pi{};
    std::memset(&pi, 0, sizeof(pi));
    c->fill_path(path);

    ngtcp2_vec vec;
    vec.base = (uint8_t*)msg.data();
    vec.len  = msg.size();

    ngtcp2_ssize ndatalen = 0;
    ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
        c->conn, &path, &pi,
        outbuf.data(), outbuf.size(),
        &ndatalen, 0, c->stream_id,
        &vec, 1, now_ns()
    );

    if (nwrite <= 0) co_return;

    auto tb = seastar::temporary_buffer<char>((size_t)nwrite);
    std::memcpy(tb.get_write(), outbuf.data(), (size_t)nwrite);

    co_await sock->send_to(sock->remote_address(), std::move(tb));
    
    co_await send_pending_pkts(c, sock);
}


// Sending connection close to server after pressing CTRL+D.
static seastar::future<> send_connection_close(client_ptr c, sock_ptr sock) {
    if (!c || !sock || !c->conn) co_return;

    std::array<uint8_t, MAX_UDP_OUT> outbuf{};
    ngtcp2_path path{};
    ngtcp2_pkt_info pi{};
    std::memset(&pi, 0, sizeof(pi));
    c->fill_path(path);

    ngtcp2_ccerr err;
    ngtcp2_ccerr_default(&err);

    ngtcp2_ssize nwrite = ngtcp2_conn_write_connection_close(
        c->conn, &path, &pi,
        outbuf.data(), outbuf.size(),
        &err,
        now_ns()
    );

    if (nwrite > 0) {
        auto tb = seastar::temporary_buffer<char>((size_t)nwrite);
        std::memcpy(tb.get_write(), outbuf.data(), (size_t)nwrite);
        co_await sock->send_to(sock->remote_address(), std::move(tb));
        LOG("[Client] Sending CONNECTION_CLOSE.");
    }
}

// Loop getting data from server.
static seastar::future<> net_loop(client_ptr c, sock_ptr sock) {
    while (true) {
        try {
            auto d = co_await sock->recv_one();
            auto& pkt = d.get_data();
            auto tb = packet_to_tb(pkt);

            sockaddr_storage peer_ss{};
            socklen_t peer_len = 0;
            sa_to_storage_v6(d.get_src(), peer_ss, peer_len);

            ngtcp2_path rpath{};
            ngtcp2_pkt_info pi{};
            std::memset(&pi, 0, sizeof(pi));
            init_ngtcp2_addr(&rpath.local,  (sockaddr*)&c->local_ss, c->local_ss_len);
            init_ngtcp2_addr(&rpath.remote, (sockaddr*)&peer_ss,    peer_len);

            int rv = ngtcp2_conn_read_pkt(
                c->conn, &rpath, &pi,
                (const uint8_t*)tb.get(), tb.size(),
                now_ns()
            );

            if (rv < 0) {
                if (rv == NGTCP2_ERR_DRAINING) {
                    LOG("[Client] server closed connection (draining)");
                    c->closing = true;
                    c->reschedule();
                    sock->shutdown();
                    break;
                }
                LOG("[Client] read_pkt error: " << ngtcp2_strerror(rv));
                continue;
            }

            c->reschedule();

            co_await send_pending_pkts(c, sock);
        } catch (...) {
            if (c->closing) break;
        }
    }
}

// Loop getting data from input.
static seastar::future<> input_loop(client_ptr c, sock_ptr sock) {
    int flags = fcntl(0, F_GETFL, 0);
    fcntl(0, F_SETFL, flags | O_NONBLOCK);

    char buf[1024];

    while (!c->closing) {
        ssize_t n = ::read(0, buf, sizeof(buf));

        if (n > 0) {
            std::string msg(buf, n);

            co_await send_message(c, sock, msg);
            
            c->reschedule();
        } 
        else if (n == 0) {
            co_await send_connection_close(c, sock);
            c->closing = true;
            c->reschedule();
            sock->shutdown();
            break;
        } 
        else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                co_await seastar::sleep(std::chrono::milliseconds(50));
            } else {
                LOG("[client] stdin error: " << strerror(errno));
                c->closing = true;
                c->reschedule();
                break;
            }
        }
    }
}

// Handling retransmission if needed.
static seastar::future<> timer_loop(client_ptr c, sock_ptr sock) {
    while (!c->closing) {
        auto now = now_ns();
        auto expiry = ngtcp2_conn_get_expiry(c->conn);

        try {
            if (expiry == UINT64_MAX) {
                co_await c->timer_cv.wait();
            } 
            else if (expiry > now) {
                co_await c->timer_cv.wait(std::chrono::nanoseconds(expiry - now));
            }
        }
        catch (const seastar::condition_variable_timed_out&) {}
        catch (seastar::broken_condition_variable&) {
            break;
        }
        
        if (c->closing) break;

        now = now_ns();
        if (ngtcp2_conn_get_expiry(c->conn) <= now) {
            int rv = ngtcp2_conn_handle_expiry(c->conn, now);
            // If ngtcp2_conn_handle_expiry returned idle close flag, it means
            // idle timeout - server closed connection so we want to end client.
            if (rv == NGTCP2_ERR_IDLE_CLOSE) {
                LOG("[Client] connection idle timeout");
                c->closing = true;
                c->reschedule();
                sock->shutdown();
                break;
            }
        }

        co_await send_pending_pkts(c, sock);
    }
}

int main(int argc, char** argv) {
    std::srand((unsigned)time(nullptr));

    if (gnutls_global_init() < 0) {
        std::cerr << "gnutls_global_init failed\n";
        return 1;
    }

    seastar::app_template app;
    int rc = app.run(argc, argv, []() -> seastar::future<int> {
        try {
            auto c = seastar::make_lw_shared<Client>();
            init_tls(*c);

            seastar::socket_address local = seastar::socket_address(seastar::ipv6_addr{0});

            sockaddr_in6 r{};
            r.sin6_family = AF_INET6;
            r.sin6_port   = htons(SERVER_PORT);
            if (inet_pton(AF_INET6, SERVER_IP, &r.sin6_addr) != 1) {
                throw std::runtime_error("inet_pton failed for SERVER_IP");
            }
            seastar::socket_address remote = seastar::socket_address(r);

            auto ch = seastar::engine().net().make_bound_datagram_channel(local);
            auto real_local = ch.local_address();
            auto sock = seastar::make_lw_shared<SeastarDgramSocket>(std::move(ch), real_local, remote);

            sa_to_storage_v6(sock->local_address(),  c->local_ss,  c->local_ss_len);
            sa_to_storage_v6(sock->remote_address(), c->remote_ss, c->remote_ss_len);

            init_ngtcp2(*c);

            LOG("[client] sending Initial");
            co_await send_pending_pkts(c, sock);
            
            co_await seastar::when_all_succeed(
                net_loop(c, sock),
                input_loop(c, sock),
                timer_loop(c, sock)
            ).discard_result();
            
            co_return 0;

        } catch (const std::exception& e) {
            LOG("[client] fatal: " << e.what());
            co_return 1;
        }
    });

    // Cleanup.
    gnutls_global_deinit();
    return rc;
}

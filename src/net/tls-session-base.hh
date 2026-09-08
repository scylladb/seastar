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
#pragma once

#include <cerrno>
#include <span>
#include <system_error>
#include <vector>

#include <fmt/format.h>

#include <seastar/core/future.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/net/tls.hh>
#include <seastar/util/log.hh>

#include "tls-impl.hh"

namespace seastar {

extern logger tls_log;

namespace tls {

/**
 * Common base for the backend TLS session implementations
 * (GnuTLS, OpenSSL).
 *
 * A TLS session is the conduit for one TLS/SSL data flow: it owns the
 * underlying connected_socket and its source/sink (we keep ownership
 * because we drive the handshake, re-handshakes and the shutdown
 * sequence ourselves), pumps plaintext between the caller and the
 * backend's TLS engine, and pumps ciphertext between the engine and
 * the socket.
 *
 * The class splits along that line (template-method style):
 *
 *  - session_base implements, once, the async plumbing that is
 *    independent of the TLS library: the put()/linearization path, the
 *    read/write semaphore protocol, waiting for input, the
 *    shutdown/close lifecycle, and the error/eof bookkeeping.
 *
 *  - The backend implements the engine-shaped pieces via the protected
 *    virtual hooks below: the actual handshake/read/write/shutdown
 *    state machines, engine error decoding, and certificate
 *    introspection. Hooks are called at record granularity, so the
 *    virtual dispatch cost is noise.
 *
 * Note that the pending-output future (_output_pending in both
 * backends) deliberately remains backend-owned: its semantics
 * currently differ between the backends (plain future vs shared_future
 * acting as a circuit breaker), and no method here needs to touch it.
 */
class session_base : public enable_shared_from_this<session_base>, public session_impl {
public:
    session_base(session_type t, std::unique_ptr<net::connected_socket_impl> sock,
            tls_options options);

    // Send plaintext: handshakes first if needed, linearizes small
    // scattered writes, then encrypts via do_put_one() under _out_sem.
    future<> put(std::span<temporary_buffer<char>> bufs) override;
    // Flush the underlying sink, serialized against the write path.
    future<> flush() noexcept override;
    // Idempotent, non-blocking teardown: runs the bye handshake in the
    // background (bounded by _options.bye_timeout), then closes both
    // streams and waits for in-flight operations to leave.
    void close() noexcept override;
    // The wrapped transport, for socket options and addresses.
    seastar::net::connected_socket_impl& socket() const override;

    // True once we have observed end-of-stream on the input, either as
    // a real socket EOF or because close() forced it.
    bool eof() const {
        return _eof;
    }

    // "Client" or "Server"; used in log messages.
    const char* get_type_string() const {
        return _type == session_type::CLIENT ? "Client" : "Server";
    }

    // Returns the local address formatted as a string (e.g. 'ip:port') or
    // `DISCONNECTED` if not connected
    const sstring& local_address() const noexcept {
        return _local_address;
    }

    // Returns the remote address formatted as a string (e.g. 'ip:port') or
    // `DISCONNECTED` if not connected
    const sstring& remote_address() const noexcept {
        return _remote_address;
    }

protected:
    // -- backend hooks -------------------------------------------------

    // True once the TLS handshake has completed on this session.
    virtual bool connected() const = 0;
    // Performs the handshake, including any backend-specific preparation
    // (e.g. loading system trust), serializing via do_handshake_sync().
    virtual future<> handshake() = 0;
    // Reads and decrypts one chunk of data. Called with _in_sem held.
    virtual future<temporary_buffer<char>> do_get() = 0;
    // Encrypts and writes the given bytes. Called with _out_sem held.
    virtual future<> do_put_one(const char* ptr, size_t size) = 0;
    // Initiates the TLS shutdown (close alert). Called with _out_sem held.
    virtual future<> do_shutdown() = 0;

    // -- shared plumbing -----------------------------------------------

    // Feed the buffer(s) to do_put_one() under _out_sem, keeping them
    // alive until the write completes. put() dispatches here once the
    // session is connected.
    future<> do_put(std::vector<temporary_buffer<char>> bufs);
    future<> do_put(temporary_buffer<char> buf);
    // Resolves once ciphertext is available in _input (or on eof/error):
    // reads from _in if the buffer is currently empty.
    future<> wait_for_input();
    // Drains records via do_get() until the peer's EOF (close alert) is
    // seen, or, if !_options.wait_for_data_on_shutdown, for at most one
    // record. Used by shutdown() to complete the bye handshake.
    future<> wait_for_eof();
    // Sends the close alert via do_shutdown() and then waits for the
    // peer's EOF. close() runs this in the background, bounded by
    // _options.bye_timeout.
    future<> shutdown();

    // Bytes of ciphertext currently buffered in _input.
    size_t in_avail() const {
        return _input.size();
    }

    // Runs \c func with both semaphores held (input, then output --
    // everything serializing against both paths must use this order),
    // recording the first error.
    template<std::invocable Func>
    future<> do_handshake_sync(Func func) {
        return with_semaphore(_in_sem, 1, [this, func = std::move(func)]() mutable {
            return with_semaphore(_out_sem, 1, [this, func = std::move(func)]() mutable {
                return futurize_invoke(func).handle_exception([this](auto ep) {
                    if (!_error) {
                        _error = ep;
                    }
                    return make_exception_future<>(_error);
                });
            });
        });
    }

    // Runs \c f once the session is usable: raises any recorded error,
    // refuses shut-down sessions, and handshakes first if needed.
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
                // always recurse, in case malicious api caller does a shutdown while the above handshake is
                // happening. I.e. misuses the api.
                return state_checked_access(std::move(f), std::forward<Args>(args)...);
            });
        }
        return futurize_invoke(f, std::forward<Args>(args)...);
    }

    // -- shared state --------------------------------------------------

    // The transport under the TLS session. Owned by us: the sockets
    // handed out to users wrap this session, not the raw socket.
    std::unique_ptr<net::connected_socket_impl> _sock;
    // Peer addresses as strings ('ip:port', or "DISCONNECTED"),
    // captured at construction for use in log messages.
    sstring _local_address;
    sstring _remote_address;
    // Ciphertext streams of _sock. All socket I/O goes through these;
    // close() closes them to wake any waiters.
    data_source _in;
    data_sink _out;

    // Reads and writes run concurrently (full duplex), serialized by two
    // disjoint semaphores: _in_sem guards the read path (get()), _out_sem
    // the write path (put(), plus flush, rehandshake and shutdown).
    // Operations spanning both take them in the order _in_sem then
    // _out_sem, so there is no deadlock.
    semaphore _in_sem;
    semaphore _out_sem;

    // Per-session options (server name, ALPN, shutdown behavior, ...),
    // fixed at construction.
    tls_options _options;
    // Whether this is the client or server end of the session.
    session_type _type;

    // Ciphertext read from _in but not yet consumed by the TLS engine.
    // The backend's engine pull path (GnuTLS pull callback, OpenSSL
    // read BIO) drains this; wait_for_input() refills it.
    temporary_buffer<char> _input;
    // First error observed on the session; once set, all subsequent
    // operations fail with it.
    std::exception_ptr _error;

    // Input has reached end-of-stream (see eof()).
    bool _eof = false;
    // close() has been called; the session is (being) torn down and
    // rejects new operations.
    bool _shutdown = false;
};

} // namespace tls
} // namespace seastar

template <>
struct fmt::formatter<seastar::tls::session_base> : public fmt::formatter<string_view> {
    auto format(const seastar::tls::session_base& s, fmt::format_context& ctx) const -> decltype(ctx.out());
};

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
#include "tls-session-base.hh"

#include <algorithm>
#include <numeric>

#include <fmt/ostream.h>

#include <seastar/core/loop.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/with_timeout.hh>
#include <seastar/net/stack.hh>

namespace seastar {

logger tls_log("tls");

namespace tls {

session_base::session_base(session_type t, std::unique_ptr<net::connected_socket_impl> sock,
        tls_options options)
    : _sock(std::move(sock))
    , _local_address([this]() -> sstring {
        try {
            return fmt::to_string(_sock->local_address());
        } catch (const std::system_error&) {
            return "DISCONNECTED";
        }
    }())
    , _remote_address([this]() -> sstring {
        try {
            return fmt::to_string(_sock->remote_address());
        } catch (const std::system_error&) {
            return "DISCONNECTED";
        }
    }())
    , _in(_sock->source())
    , _out(_sock->sink())
    , _in_sem(1)
    , _out_sem(1)
    , _options(std::move(options))
    , _type(t) {
}

// Used to push unencrypted data through the TLS engine, which will
// encrypt it and then send it down the underlying socket.
future<> session_base::put(std::span<temporary_buffer<char>> bufs) {
    tls_log.trace("{} put", *this);
    // The TLS default maximum record size (2^14 bytes). This only
    // controls whether we linearize small writes below -- the backends
    // still chunk the actual record sends to the negotiated record size.
    constexpr size_t max_tls_record_size = 16 * 1024;
    if (_error) {
        return make_exception_future<>(_error);
    }
    if (_shutdown) {
        return make_exception_future<>(std::system_error(EPIPE, std::system_category()));
    }
    if (!connected()) {
        tls_log.trace("{} put: not connected, performing handshake", *this);
        std::vector<temporary_buffer<char>> p;
        p.reserve(bufs.size());
        p.insert(p.end(), std::make_move_iterator(bufs.begin()), std::make_move_iterator(bufs.end()));
        return handshake().then([this, p = std::move(p)]() mutable {
           return put(std::span(p));
        });
    }

    if (bufs.size() == 1) {
        return do_put(std::move(bufs.front()));
    }

    // We want to make sure that we hand the TLS engine as large packets
    // as possible. This is because each write eventually translates to a
    // sendmsg syscall. Further it results in larger TLS records which
    // makes encryption/decryption faster. Hence to avoid cases where we
    // would do an extra syscall for something like a 100 bytes header we
    // linearize the packet if it's below the max TLS record size.
    size_t size = std::accumulate(bufs.begin(), bufs.end(), size_t(0), [] (size_t s, const auto& b) { return s + b.size(); });
    if (size <= max_tls_record_size) {
        temporary_buffer<char> linear(size);
        char* pos = linear.get_write();
        for (auto& buf : bufs) {
            std::copy_n(buf.get(), buf.size(), pos);
            pos += buf.size();
        }
        return do_put(std::move(linear));
    }

    std::vector<temporary_buffer<char>> p;
    p.reserve(bufs.size());
    p.insert(p.end(), std::make_move_iterator(bufs.begin()), std::make_move_iterator(bufs.end()));
    return do_put(std::move(p));
}

future<> session_base::do_put(std::vector<temporary_buffer<char>> bufs) {
    auto i = bufs.begin();
    auto e = bufs.end();
    return with_semaphore(_out_sem, 1, [this, i, e] {
        return do_for_each(i, e, [this](temporary_buffer<char>& b) {
            return do_put_one(b.get(), b.size());
        });
    }).finally([b = std::move(bufs)] {});
}

future<> session_base::do_put(temporary_buffer<char> buf) {
    auto ptr = buf.get();
    auto size = buf.size();
    return with_semaphore(_out_sem, 1, [this, ptr, size] {
        return do_put_one(ptr, size);
    }).finally([b = std::move(buf)] {});
}

// This function will attempt to pull data off of the _in stream
// if there isn't already data needing to be processed first.
future<> session_base::wait_for_input() {
    tls_log.trace("{} wait_for_input", *this);
    // If we already have data, then it needs to be processed
    if (!_input.empty()) {
        return make_ready_future<>();
    }
    return _in.get().then([this](temporary_buffer<char> buf) {
        // Set EOF if it's empty
        tls_log.debug("{} wait_for_input: buffer is {}empty", *this, buf.empty() ? "": "not ");
        _eof |= buf.empty();
        _input = std::move(buf);
    }).handle_exception([this](auto ep) {
        tls_log.debug("{} wait_for_input: exception: {}", *this, seastar::formattable(ep));
        _error = ep;
        return make_exception_future(ep);
    });
}

// This function waits for eof() to occur on the input stream
// Unless wait_for_data_on_shutdown is false
future<> session_base::wait_for_eof() {
    tls_log.trace("{} wait_for_eof", *this);
    // read records until we get an eof alert
    // since this call could time out, we must not ac
    return with_semaphore(_in_sem, 1, [this] {
        if (_error || !connected()) {
            return make_ready_future();
        }
        if (!_options.wait_for_data_on_shutdown) {
            // Read at most one record. If unread data arrives instead
            // of EOF, abort the wait immediately rather than looping.
            if (eof()) {
                return make_ready_future<>();
            }
            return do_get().discard_result();
        }
        return do_until(
            [this] { return eof(); },
            [this] { return do_get().discard_result(); });
    });
}

future<> session_base::shutdown() {
    tls_log.trace("{} shutdown", *this);
    // first, make sure any pending write is done.
    // bye handshake is a flush operation, but this
    // allows us to not pay extra attention to output state
    //
    // we only send a simple "bye" alert packet. Then we
    // read from input until we see EOF. Any other reader
    // before us will get it instead of us, and mark _eof = true
    // in which case we will be no-op.
    return with_semaphore(_out_sem, 1, [this] {
                            return do_shutdown();
                          }).then([this] {
                            return wait_for_eof();
                          }).finally([me = shared_from_this()] {});
    // note moved finally clause above. It is theorethically possible
    // that we could complete do_shutdown just before the close calls
    // below, get pre-empted, have "close()" finish, get freed, and
    // then call wait_for_eof on stale pointer.
}

void session_base::close() noexcept {
    tls_log.trace("{} close", *this);
    // only do once.
    if (!std::exchange(_shutdown, true)) {
        auto me = shared_from_this();
        auto f = _options.bye_timeout.count() > 0 && (_options.wait_for_eof_on_shutdown._value == true)
            // try to bye-handshake us nicely, but after a timeout we forcefully close.
            ? with_timeout(timer<>::clock::now() + _options.bye_timeout, shutdown())
            : make_ready_future<>();
        engine().run_in_background(std::move(f).finally([this] {
            _eof = true;
            return _in.close(); // should wake any waiters
        }).finally([this] {
            return _out.close();
        }).finally([this] {
            // make sure to wait for handshake attempt to leave semaphores. Must be in same order as
            // handshake aqcuire, because in worst case, we get here while a reader is attempting
            // re-handshake.
            return with_semaphore(_in_sem, 1, [this] {
                return with_semaphore(_out_sem, 1, [] {});
            });
        }).handle_exception([me = std::move(me)](std::exception_ptr) { // must keep object alive until here.
        }).discard_result());
    }
}

// helper for sink
future<> session_base::flush() noexcept {
    return with_semaphore(_out_sem, 1, [this] {
        return _out.flush();
    });
}

seastar::net::connected_socket_impl& session_base::socket() const {
    return *_sock;
}

} // namespace tls
} // namespace seastar

auto fmt::formatter<seastar::tls::session_base>::format(
    const seastar::tls::session_base& s, fmt::format_context& ctx) const -> decltype(ctx.out()) {

    return fmt::format_to(ctx.out(), "{}:{}:{} -",
        s.get_type_string(), s.local_address(), s.remote_address());
}

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

#include "quic_impl.hh"

#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

#include <ngtcp2/ngtcp2.h>

#include <seastar/coroutine/maybe_yield.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/queue.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/shared_future.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/net/stack.hh>

namespace seastar::quic::experimental {

namespace internal {

namespace {

uint64_t transport_now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

temporary_buffer<char> coalesce_buffers(std::span<temporary_buffer<char>> bufs) {
    // Seastar output streams can batch fragments; ngtcp2 wants one contiguous span.
    if (bufs.empty()) {
        return temporary_buffer<char>();
    }
    if (bufs.size() == 1) {
        return std::move(bufs.front());
    }

    size_t total = 0;
    for (const auto& buf : bufs) {
        total += buf.size();
    }

    temporary_buffer<char> merged(total);
    auto* dst = merged.get_write();
    size_t offset = 0;
    for (auto& buf : bufs) {
        std::memcpy(dst + offset, buf.get(), buf.size());
        offset += buf.size();
    }
    return merged;
}

temporary_buffer<char>& ensure_tx_packet_buffer(connection_transport& transport) {
    // Reuse one scratch packet buffer per connection to avoid per-packet allocation.
    auto required = transport.tx_payload_limit_bytes();
    auto& scratch = transport.tx_packet_buffer();
    if (scratch.size() < required) {
        scratch = temporary_buffer<char>(required);
    }
    return scratch;
}

constexpr size_t default_stream_read_queue_fragments = 1024;
constexpr size_t stream_read_queue_fragment_size_hint = 1024;

size_t make_stream_read_queue_capacity(const connection_options& options) noexcept {
    // Queue capacity is fragment-based, so derive a conservative fragment count
    // from the byte budget and keep the default burst allowance.
    if (!options.max_pending_receive_bytes) {
        return default_stream_read_queue_fragments;
    }
    auto fragments = (options.max_pending_receive_bytes + stream_read_queue_fragment_size_hint - 1)
                     / stream_read_queue_fragment_size_hint;
    return fragments + default_stream_read_queue_fragments;
}

} // namespace

class receive_budget final : public enable_shared_from_this<receive_budget> {
public:
    explicit receive_budget(size_t limit)
        : _limit(limit) {
    }

    bool try_acquire(size_t size) {
        // A zero limit means unlimited buffering; nonzero limits are connection-wide.
        if (!size) {
            return true;
        }
        if (_limit && _pending_bytes + size > _limit) {
            return false;
        }
        _pending_bytes += size;
        return true;
    }

    void release(size_t size) {
        if (!size) {
            return;
        }
        if (size >= _pending_bytes) {
            _pending_bytes = 0;
            return;
        }
        _pending_bytes -= size;
    }

private:
    size_t _limit = 0;
    size_t _pending_bytes = 0;
};

// Internal state for a single stream, including its Seastar IO queues and flow control.
class stream_state final : public enable_shared_from_this<stream_state> {
    class source_impl;
    class sink_impl;

public:
    stream_state(
      command_runtime_ptr command_runtime,
      shared_ptr<receive_budget> receive_budget,
      size_t read_queue_capacity,
      stream_id sid,
      stream_type type,
      bool peer_initiated)
        : _command_runtime(std::move(command_runtime))
        , _receive_budget(std::move(receive_budget))
        , _id(sid)
        , _type(type)
        , _can_read(type == stream_type::bidirectional || peer_initiated)
        // Locally initiated unidirectional streams are write-only; peer-initiated
        // unidirectional streams are read-only from this endpoint's perspective.
        , _can_write(type == stream_type::bidirectional || !peer_initiated)
        , _read_queue(read_queue_capacity) {
        if (!_can_read) {
            notify_input_shutdown();
        }
    }

    bool is_open() const noexcept {
        return !_transport_closed
               && ((_can_read && !_input_shutdown_notified) || (_can_write && !_output_closed));
    }

    stream_id id() const noexcept {
        return _id;
    }

    stream_type type() const noexcept {
        return _type;
    }

    bool can_read() const noexcept {
        return _can_read;
    }

    bool can_write() const noexcept {
        return _can_write;
    }

    bool mark_accepted_for_delivery() noexcept {
        if (_accepted_for_delivery) {
            return false;
        }
        _accepted_for_delivery = true;
        return true;
    }

    input_stream<char> input(connected_socket_input_stream_config cfg);
    output_stream<char> output(size_t buffer_size);

    future<> close_output();
    future<> reset(application_error_code app_error_code);
    future<> stop_sending(application_error_code app_error_code);
    future<> wait_input_shutdown() {
        return _input_shutdown.get_shared_future();
    }

    data_source source(connected_socket_input_stream_config);
    data_sink sink();

    socket_address local_address() const {
        return _command_runtime ? _command_runtime->local_address() : socket_address();
    }

    socket_address peer_address() const {
        return _command_runtime ? _command_runtime->peer_address() : socket_address();
    }

    void on_data(temporary_buffer<char> payload, bool fin);
    void on_reset(application_error_code app_error_code);
    void on_stop_sending_input(application_error_code app_error_code);
    void on_stop_sending_output(application_error_code app_error_code);
    void on_batch_flush_error() noexcept;
    void on_transport_closed();
    void on_data_consumed(size_t size);

private:
    future<> send_one(temporary_buffer<char> payload, bool fin);

    void overload_input(const char* detail) {
        // Input overload is scoped to this stream; the connection can keep serving
        // other streams while this reader is failed.
        if (_input_shutdown_notified) {
            return;
        }
        abort_read_queue(std::make_exception_ptr(quic_error(quic_error_code::io, detail)));
    }

    bool push_read_fragment(temporary_buffer<char> payload) {
        const auto payload_size = payload.size();
        // The receive budget is shared by all streams on this connection, which
        // keeps one slow consumer from growing memory without bound.
        if (payload_size && _receive_budget && !_receive_budget->try_acquire(payload_size)) {
            overload_input("receive queue limit exceeded");
            return false;
        }
        if (_read_queue.push(std::move(payload))) {
            _queued_read_bytes += payload_size;
            return true;
        }
        if (payload_size && _receive_budget) {
            _receive_budget->release(payload_size);
        }
        overload_input("stream read queue is full");
        return false;
    }

    void notify_input_shutdown() {
        if (_input_shutdown_notified) {
            return;
        }
        _input_shutdown_notified = true;
        _input_shutdown.set_value();
    }

    void release_queued_read_bytes() {
        // Abort paths must return all queued credit because readers will not drain it.
        if (_queued_read_bytes && _receive_budget) {
            _receive_budget->release(_queued_read_bytes);
        }
        _queued_read_bytes = 0;
    }

    void release_read_bytes(size_t size) {
        if (!size) {
            return;
        }
        if (_queued_read_bytes >= size) {
            _queued_read_bytes -= size;
        } else {
            _queued_read_bytes = 0;
        }
        if (_receive_budget) {
            _receive_budget->release(size);
        }
    }

    void abort_read_queue(std::exception_ptr ex) {
        // Abort the queue and complete input shutdown together so waiters agree.
        release_queued_read_bytes();
        _read_queue.abort(ex);
        notify_input_shutdown();
    }

    command_runtime_ptr _command_runtime;
    shared_ptr<receive_budget> _receive_budget;
    stream_id _id = invalid_stream_id;
    stream_type _type = stream_type::bidirectional;
    bool _can_read = true;
    bool _can_write = true;

    queue<temporary_buffer<char>> _read_queue;
    shared_promise<> _input_shutdown;
    bool _input_shutdown_notified = false;
    bool _output_closed = false;
    bool _transport_closed = false;
    bool _accepted_for_delivery = false;
    size_t _queued_read_bytes = 0;
};

class connection_state::impl {
public:
    impl(command_runtime_ptr command_runtime_arg, connection_options options_arg)
        : command_runtime(std::move(command_runtime_arg))
        , options(std::move(options_arg))
        , accepted_streams(1024)
        , receive_window(make_shared<receive_budget>(options.max_pending_receive_bytes))
        , stream_read_queue_capacity(make_stream_read_queue_capacity(options)) {
        _timer.set_callback([this] {
            // Coalesce timer expirations until the actor processes the pending tick.
            if (transport_closed || tick_pending) {
                return;
            }
            tick_pending = true;
            if (!actor_waiter) {
                return;
            }
            auto waiter = std::move(*actor_waiter);
            actor_waiter.reset();
            waiter.set_value();
        });
    }

    void refill_pending_accepted_streams() {
        // Transport callbacks may discover streams faster than accept() drains them,
        // so overflow is retried whenever queue capacity returns.
        while (!pending_accepted_streams.empty()) {
            auto st = pending_accepted_streams.front();
            if (!accepted_streams.push(shared_ptr<stream_state>(st))) {
                break;
            }
            pending_accepted_streams.pop_front();
        }
    }

    void enqueue_accepted_stream(const shared_ptr<stream_state>& st) {
        // Keep stream delivery ordered even when the public accept queue is full.
        if (!accepted_streams.push(shared_ptr<stream_state>(st))) {
            pending_accepted_streams.push_back(st);
        }
    }

    // Shared state behind the public connection handle.
    command_runtime_ptr command_runtime;
    connection_options options;
    queue<shared_ptr<stream_state>> accepted_streams;
    std::deque<shared_ptr<stream_state>> pending_accepted_streams;
    std::unordered_map<stream_id, shared_ptr<stream_state>> streams;
    shared_ptr<receive_budget> receive_window;
    size_t stream_read_queue_capacity;
    bool transport_closed = false;

    std::deque<transport_command> blocked_bidi_open_streams;
    std::deque<transport_command> blocked_uni_open_streams;
    std::optional<promise<>> actor_waiter;
    timer<> _timer;
    bool tick_pending = false;
    bool blocked_bidi_open_stream_retry_pending = false;
    bool blocked_uni_open_stream_retry_pending = false;
};

future<> stream_state::send_one(temporary_buffer<char> payload, bool fin) {
    if (!_can_write) {
        return make_exception_future<>(quic_error(quic_error_code::invalid_state, "stream output is unavailable"));
    }
    if (!_command_runtime) {
        return make_exception_future<>(quic_error(quic_error_code::closed, "stream command runtime is gone"));
    }
    if (_transport_closed || (_output_closed && !fin)) {
        // A final FIN is allowed through the close path; later payload writes fail.
        return make_exception_future<>(quic_error(quic_error_code::closed, "stream output is closed"));
    }
    return _command_runtime->send(internal::quic_message{
      .stream = _id,
      .payload = std::move(payload),
      .fin = fin,
    });
}

future<> stream_state::close_output() {
    if (!_can_write) {
        throw_quic_error(quic_error_code::invalid_state, "stream output is unavailable");
    }
    if (_output_closed) {
        co_return;
    }
    _output_closed = true;
    co_await send_one(temporary_buffer<char>(), true);
}

future<> stream_state::reset(application_error_code app_error_code) {
    if (!_can_write) {
        throw_quic_error(quic_error_code::invalid_state, "stream output is unavailable");
    }
    if (_output_closed) {
        co_return;
    }
    _output_closed = true;
    if (!_command_runtime) {
        co_return;
    }
    co_await _command_runtime->reset_stream(_id, app_error_code);
}

future<> stream_state::stop_sending(application_error_code app_error_code) {
    if (!_can_read) {
        throw_quic_error(quic_error_code::invalid_state, "stream input is unavailable");
    }
    if (_input_shutdown_notified) {
        co_return;
    }
    if (!_command_runtime) {
        co_return;
    }
    // Reflect STOP_SENDING locally before the transport actor emits the frame.
    abort_read_queue(std::make_exception_ptr(quic_error(quic_error_code::closed, "stop_sending")));
    co_await _command_runtime->stop_sending(_id, app_error_code);
}

void stream_state::on_data(temporary_buffer<char> payload, bool fin) {
    if (_transport_closed || _input_shutdown_notified) {
        return;
    }
    if (!payload.empty() && !push_read_fragment(std::move(payload))) {
        return;
    }
    if (fin) {
        notify_input_shutdown();
        // Preserve EOF ordering by queueing the terminal empty fragment after data.
        (void)push_read_fragment(temporary_buffer<char>());
    }
}

void stream_state::on_reset(application_error_code) {
    abort_read_queue(std::make_exception_ptr(quic_error(quic_error_code::closed, "peer reset stream")));
}

void stream_state::on_stop_sending_input(application_error_code) {
    if (!_can_read || _input_shutdown_notified) {
        return;
    }
    abort_read_queue(std::make_exception_ptr(quic_error(quic_error_code::closed, "stop_sending")));
}

void stream_state::on_stop_sending_output(application_error_code) {
    if (!_can_write || _output_closed) {
        return;
    }
    _output_closed = true;
}

void stream_state::on_batch_flush_error() noexcept {
    _output_closed = true;
}

void stream_state::on_transport_closed() {
    if (_transport_closed) {
        return;
    }
    _transport_closed = true;
    abort_read_queue(std::make_exception_ptr(quic_error(quic_error_code::closed, "transport closed")));
}

void stream_state::on_data_consumed(size_t size) {
    if (!size || !_command_runtime || _transport_closed || !_can_read || _input_shutdown_notified) {
        return;
    }
    _command_runtime->consume_stream_data(_id, size);
}

class stream_state::source_impl final : public data_source_impl {
public:
    explicit source_impl(shared_ptr<stream_state> state)
        : _state(std::move(state)) {
    }

    future<temporary_buffer<char>> get() override {
        auto buf = co_await _state->_read_queue.pop_eventually();
        auto consumed = buf.size();
        // Empty buffers are EOF markers and do not replenish receive credit.
        _state->release_read_bytes(consumed);
        _state->on_data_consumed(consumed);
        co_return std::move(buf);
    }

    future<> close() override {
        // Closing the public input side maps to QUIC STOP_SENDING.
        return _state->stop_sending(0);
    }

private:
    shared_ptr<stream_state> _state;
};

class stream_state::sink_impl final : public data_sink_impl {
public:
    explicit sink_impl(shared_ptr<stream_state> state)
        : _state(std::move(state)) {
    }

    future<> put(std::span<temporary_buffer<char>> bufs) override {
        if (bufs.empty()) {
            return make_ready_future<>();
        }
        return _state->send_one(coalesce_buffers(bufs), false);
    }

    future<> close() override {
        return _state->close_output();
    }

    size_t buffer_size() const noexcept override {
        return 8192;
    }

    bool can_batch_flushes() const noexcept override {
        return true;
    }

    void on_batch_flush_error() noexcept override {
        _state->on_batch_flush_error();
    }

private:
    shared_ptr<stream_state> _state;
};

input_stream<char> stream_state::input(connected_socket_input_stream_config cfg) {
    if (!_can_read) {
        throw_quic_error(quic_error_code::invalid_state, "stream input is unavailable");
    }
    return input_stream<char>(source(cfg));
}

output_stream<char> stream_state::output(size_t buffer_size) {
    if (!_can_write) {
        throw_quic_error(quic_error_code::invalid_state, "stream output is unavailable");
    }
    if (_output_closed) {
        throw_quic_error(quic_error_code::closed, "stream output is closed");
    }
    return output_stream<char>(sink(), buffer_size);
}

data_source stream_state::source(connected_socket_input_stream_config) {
    if (!_can_read) {
        throw_quic_error(quic_error_code::invalid_state, "stream input is unavailable");
    }
    return data_source(std::make_unique<source_impl>(shared_from_this()));
}

data_sink stream_state::sink() {
    if (!_can_write) {
        throw_quic_error(quic_error_code::invalid_state, "stream output is unavailable");
    }
    if (_output_closed) {
        throw_quic_error(quic_error_code::closed, "stream output is closed");
    }
    return data_sink(std::make_unique<sink_impl>(shared_from_this()));
}

connection_state::connection_state(command_runtime_ptr command_runtime, connection_options options)
    : _impl(std::make_unique<impl>(std::move(command_runtime), std::move(options))) {
}

connection_state::~connection_state() = default;

bool connection_state::is_open() const noexcept {
    return _impl->command_runtime && _impl->command_runtime->is_open();
}

socket_address connection_state::local_address() const {
    return _impl->command_runtime ? _impl->command_runtime->local_address() : socket_address();
}

socket_address connection_state::peer_address() const {
    return _impl->command_runtime ? _impl->command_runtime->peer_address() : socket_address();
}

sstring connection_state::selected_alpn() const {
    return _impl->command_runtime ? _impl->command_runtime->selected_alpn() : sstring();
}

future<stream> connection_state::open_stream(stream_open_options options) {
    // Stream state is created after transport credit reserves a concrete id.
    auto sid = co_await _impl->command_runtime->open_stream(options.type);
    auto [it, inserted] = _impl->streams.emplace(sid, shared_ptr<stream_state>{});
    if (inserted || !it->second) {
        it->second = make_shared<stream_state>(
          _impl->command_runtime, _impl->receive_window, _impl->stream_read_queue_capacity, sid, options.type, false);
    }
    auto st = it->second;
    co_return stream(std::make_unique<stream::impl>(std::move(st)));
}

future<stream> connection_state::accept_stream() {
    _impl->refill_pending_accepted_streams();
    auto st = co_await _impl->accepted_streams.pop_eventually();
    _impl->refill_pending_accepted_streams();
    co_return stream(std::make_unique<stream::impl>(std::move(st)));
}

future<> connection_state::close() {
    if (!_impl->command_runtime) {
        co_return;
    }
    co_await _impl->command_runtime->close();
}

void connection_state::on_stream_data(stream_id sid, stream_type type, bool peer_initiated, temporary_buffer<char> payload, bool fin) {
    // Transport callbacks can be the first time a peer-initiated stream is seen.
    auto [it, inserted] = _impl->streams.emplace(sid, shared_ptr<stream_state>{});
    if (inserted || !it->second) {
        it->second = make_shared<stream_state>(
          _impl->command_runtime, _impl->receive_window, _impl->stream_read_queue_capacity, sid, type, peer_initiated);
    }
    auto st = it->second;
    if (peer_initiated && st->mark_accepted_for_delivery()) {
        // Deliver each peer-initiated stream to accept_stream() exactly once.
        _impl->enqueue_accepted_stream(st);
    }
    st->on_data(std::move(payload), fin);
}

void connection_state::on_stream_reset(
  stream_id sid,
  stream_type type,
  bool peer_initiated,
  application_error_code app_error_code) {
    auto [it, inserted] = _impl->streams.emplace(sid, shared_ptr<stream_state>{});
    if (inserted || !it->second) {
        it->second = make_shared<stream_state>(
          _impl->command_runtime, _impl->receive_window, _impl->stream_read_queue_capacity, sid, type, peer_initiated);
    }
    auto st = it->second;
    if (peer_initiated && st->mark_accepted_for_delivery()) {
        // Deliver each peer-initiated stream to accept_stream() exactly once.
        _impl->enqueue_accepted_stream(st);
    }
    st->on_reset(app_error_code);
}

void connection_state::on_stream_stop_sending(
  stream_id sid,
  stream_type type,
  bool peer_initiated,
  application_error_code app_error_code,
  stream_shutdown_side shutdown_side) {
    auto [it, inserted] = _impl->streams.emplace(sid, shared_ptr<stream_state>{});
    if (inserted || !it->second) {
        it->second = make_shared<stream_state>(
          _impl->command_runtime, _impl->receive_window, _impl->stream_read_queue_capacity, sid, type, peer_initiated);
    }
    auto st = it->second;
    if (peer_initiated && st->mark_accepted_for_delivery()) {
        // Deliver each peer-initiated stream to accept_stream() exactly once.
        _impl->enqueue_accepted_stream(st);
    }
    if (shutdown_side == stream_shutdown_side::write) {
        st->on_stop_sending_output(app_error_code);
    } else {
        st->on_stop_sending_input(app_error_code);
    }
}

void connection_state::on_stream_closed(stream_id sid) {
    _impl->streams.erase(sid);
}

void connection_state::on_transport_closed(std::exception_ptr ex) {
    // Fan out one terminal transport error to every public stream surface.
    if (_impl->transport_closed) {
        return;
    }
    _impl->transport_closed = true;
    _impl->_timer.cancel();
    if (!ex) {
        ex = std::make_exception_ptr(quic_error(quic_error_code::closed, "transport closed"));
    }
    _impl->accepted_streams.abort(ex);
    _impl->pending_accepted_streams.clear();
    for (auto& [_, st] : _impl->streams) {
        st->on_transport_closed();
    }
    _impl->streams.clear();
}

future<> connection_state::wait_for_actor_wakeup(bool has_pending_work, bool closing) {
    // Recheck before sleeping so edge-triggered wakeups cannot be missed.
    if (has_pending_work || closing) {
        co_return;
    }
    _impl->actor_waiter.emplace();
    try {
        co_await _impl->actor_waiter->get_future();
    } catch (...) {
    }
    co_return;
}

void connection_state::wake_actor() {
    if (!_impl->actor_waiter) {
        return;
    }
    auto waiter = std::move(*_impl->actor_waiter);
    _impl->actor_waiter.reset();
    waiter.set_value();
}

void connection_state::arm_timer(std::chrono::nanoseconds delay, bool closing) {
    // Timer ownership stays here so client and server actors use identical expiry logic.
    if (closing || _impl->transport_closed) {
        _impl->_timer.cancel();
        return;
    }
    if (delay <= std::chrono::nanoseconds::zero()) {
        _impl->_timer.cancel();
        if (_impl->tick_pending) {
            return;
        }
        _impl->tick_pending = true;
        wake_actor();
        return;
    }
    _impl->_timer.cancel();
    _impl->_timer.arm(delay);
}

void connection_state::rearm_timer_from_expiry(uint64_t expiry_ns, uint64_t now_ns, bool closing) {
    constexpr auto max_timer_sleep = std::chrono::hours(24);
    if (expiry_ns <= now_ns) {
        // Expired deadlines are converted into an immediate actor tick.
        arm_timer(std::chrono::nanoseconds(0), closing);
        return;
    }
    auto wait_ns = expiry_ns - now_ns;
    auto max_wait_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(max_timer_sleep).count());
    auto sleep_ns = wait_ns > max_wait_ns ? max_wait_ns : wait_ns;
    arm_timer(std::chrono::nanoseconds(sleep_ns), closing);
}

bool connection_state::tick_pending() const noexcept {
    return _impl->tick_pending;
}

void connection_state::clear_tick() noexcept {
    _impl->tick_pending = false;
}

void connection_state::cancel_timer() noexcept {
    _impl->_timer.cancel();
}

void connection_state::defer_blocked_open_stream(transport_command cmd) {
    // Keep bidi and uni waits separate because ngtcp2 extends their credits separately.
    auto& q = cmd.type == stream_type::bidirectional
                ? _impl->blocked_bidi_open_streams
                : _impl->blocked_uni_open_streams;
    q.push_back(std::move(cmd));
}

std::optional<transport_command> connection_state::pop_blocked_open_stream(stream_type type) {
    auto& q = type == stream_type::bidirectional
                ? _impl->blocked_bidi_open_streams
                : _impl->blocked_uni_open_streams;
    if (q.empty()) {
        return std::nullopt;
    }
    auto cmd = std::move(q.front());
    q.pop_front();
    return cmd;
}

void connection_state::request_blocked_open_stream_retry(stream_type type) {
    auto& pending = type == stream_type::bidirectional
                      ? _impl->blocked_bidi_open_stream_retry_pending
                      : _impl->blocked_uni_open_stream_retry_pending;
    pending = true;
    wake_actor();
}

bool connection_state::blocked_open_stream_retry_pending(stream_type type) const noexcept {
    return type == stream_type::bidirectional
             ? _impl->blocked_bidi_open_stream_retry_pending
             : _impl->blocked_uni_open_stream_retry_pending;
}

void connection_state::clear_blocked_open_stream_retry(stream_type type) noexcept {
    auto& pending = type == stream_type::bidirectional
                      ? _impl->blocked_bidi_open_stream_retry_pending
                      : _impl->blocked_uni_open_stream_retry_pending;
    pending = false;
}

bool connection_state::has_blocked_open_stream_retry_work() const noexcept {
    return (_impl->blocked_bidi_open_stream_retry_pending && !_impl->blocked_bidi_open_streams.empty())
           || (_impl->blocked_uni_open_stream_retry_pending && !_impl->blocked_uni_open_streams.empty());
}

void connection_state::fail_blocked_open_streams(quic_error_code error, std::string_view detail) {
    // Open-stream promises live above the transport actor and must be completed on teardown.
    if (!_impl->command_runtime) {
        _impl->blocked_bidi_open_streams.clear();
        _impl->blocked_uni_open_streams.clear();
        _impl->blocked_bidi_open_stream_retry_pending = false;
        _impl->blocked_uni_open_stream_retry_pending = false;
        return;
    }

    auto fail_queue = [this, error, detail] (auto& q) {
        for (auto& cmd : q) {
            _impl->command_runtime->fail_open_stream(cmd.open_result, error, sstring(detail));
        }
        q.clear();
    };

    fail_queue(_impl->blocked_bidi_open_streams);
    fail_queue(_impl->blocked_uni_open_streams);
    _impl->blocked_bidi_open_stream_retry_pending = false;
    _impl->blocked_uni_open_stream_retry_pending = false;
}

future<> flush_pending_transport_packets(connection_transport& transport) {
    if (!transport.has_transport_connection()) {
        co_return;
    }

    auto& outbuf = ensure_tx_packet_buffer(transport);
    while (transport.transport_active()) {
        // Drain ACKs, probes and other control frames not tied to an app write.
        auto nwrite = transport.write_pending_packet(
          reinterpret_cast<uint8_t*>(outbuf.get_write()),
          outbuf.size());
        if (nwrite == 0) {
            co_return;
        }
        if (nwrite < 0) {
            if (ngtcp2_is_write_more(static_cast<int>(nwrite))) {
                // ngtcp2 filled internal coalescing state; ask it for the next packet.
                continue;
            }
            if (ngtcp2_is_draining(static_cast<int>(nwrite))) {
                transport.stop_transport();
                co_return;
            }
            transport.fail_transport(
              classify_ngtcp2_error(static_cast<int>(nwrite)),
              ngtcp2_error_message(static_cast<int>(nwrite)));
            co_return;
        }
        co_await transport.send_datagram_packet(outbuf.share(0, static_cast<size_t>(nwrite)));
    }
}

namespace {

void complete_send_progress(connection_transport& transport, quic_message& msg, size_t consumed) {
    if (!consumed) {
        return;
    }
    if (consumed > msg.payload.size()) {
        consumed = msg.payload.size();
    }
    // Retain bytes by stream offset until ngtcp2 reports them acknowledged.
    transport.retain_stream_data(msg.stream, msg.payload.share(0, consumed));
    transport.complete_send_bytes(consumed);
    msg.payload.trim_front(consumed);
}

void discard_unsent_send_bytes(connection_transport& transport, quic_message& msg) {
    // Release command-runtime backpressure for bytes ngtcp2 will never consume.
    if (msg.payload.empty()) {
        return;
    }
    transport.complete_send_bytes(msg.payload.size());
    msg.payload = temporary_buffer<char>();
}

} // namespace

future<std::optional<quic_message>> send_stream_message(connection_transport& transport, quic_message msg) {
    if (!transport.has_transport_connection() || !transport.transport_active() || msg.stream == invalid_stream_id) {
        discard_unsent_send_bytes(transport, msg);
        co_return std::nullopt;
    }

    bool send_fin = msg.fin;
    auto& outbuf = ensure_tx_packet_buffer(transport);

    while (transport.transport_active()) {
        const bool remaining = !msg.payload.empty();
        if (!remaining && !send_fin) {
            break;
        }

        auto* ptr = remaining ? msg.payload.get() : nullptr;
        auto len = remaining ? msg.payload.size() : 0;
        auto result = transport.write_stream_packet(
          msg.stream,
          ptr,
          len,
          send_fin,
          reinterpret_cast<uint8_t*>(outbuf.get_write()),
          outbuf.size());

        if (result.nwrite < 0) {
            // Negative write results can still report consumed stream bytes.
            if (ngtcp2_is_write_more(static_cast<int>(result.nwrite))) {
                complete_send_progress(transport, msg, result.consumed);
                co_await flush_pending_transport_packets(transport);
                continue;
            }
            if (ngtcp2_is_draining(static_cast<int>(result.nwrite))) {
                discard_unsent_send_bytes(transport, msg);
                transport.stop_transport();
                co_return std::nullopt;
            }
            if (result.nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                complete_send_progress(transport, msg, result.consumed);
                co_await flush_pending_transport_packets(transport);
                transport.rearm_transport_timer();
                // Hand the unsent suffix back to the actor for a later credit retry.
                msg.fin = send_fin;
                co_return std::make_optional(std::move(msg));
            }
            if (result.nwrite == NGTCP2_ERR_STREAM_SHUT_WR || result.nwrite == NGTCP2_ERR_STREAM_NOT_FOUND) {
                // Treat peer-side write shutdown as stream-local, not connection-fatal.
                complete_send_progress(transport, msg, result.consumed);
                discard_unsent_send_bytes(transport, msg);
                transport.on_stream_write_closed(msg.stream);
                co_await flush_pending_transport_packets(transport);
                transport.rearm_transport_timer();
                co_return std::nullopt;
            }
            discard_unsent_send_bytes(transport, msg);
            transport.fail_transport(
              classify_ngtcp2_error(static_cast<int>(result.nwrite)),
              ngtcp2_error_message(static_cast<int>(result.nwrite)));
            co_return std::nullopt;
        }
        complete_send_progress(transport, msg, result.consumed);
        if (result.nwrite == 0) {
            co_await flush_pending_transport_packets(transport);
            if (result.consumed) {
                // ngtcp2 consumed stream bytes while only producing control packets.
                continue;
            }
            transport.rearm_transport_timer();
            msg.fin = send_fin;
            co_return std::make_optional(std::move(msg));
        }

        co_await transport.send_datagram_packet(outbuf.share(0, static_cast<size_t>(result.nwrite)));
        if (send_fin && msg.payload.empty()) {
            send_fin = false;
        }

        if (msg.payload.empty() && !send_fin) {
            break;
        }
    }

    if ((!transport.transport_active() || !transport.has_transport_connection())
        && (!msg.payload.empty() || send_fin)) {
        discard_unsent_send_bytes(transport, msg);
        send_fin = false;
    }

    co_await flush_pending_transport_packets(transport);
    transport.rearm_transport_timer();
    msg.fin = false;
    co_return std::nullopt;
}

future<bool> open_stream(connection_transport& transport, transport_command cmd) {
    if (!transport.has_transport_connection() || !cmd.open_result) {
        co_return false;
    }

    auto result = transport.try_open_stream(cmd.type);
    if (result.rv == 0) {
        // Complete the public promise only after ngtcp2 assigns a concrete id.
        transport.complete_open_stream(cmd.open_result, result.sid);
        co_await flush_pending_transport_packets(transport);
        transport.rearm_transport_timer();
        co_return false;
    }

    if (result.rv == NGTCP2_ERR_STREAM_ID_BLOCKED) {
        // Defer until MAX_STREAMS credit is extended by the peer.
        transport.defer_blocked_open_stream(std::move(cmd));
        co_await flush_pending_transport_packets(transport);
        transport.rearm_transport_timer();
        co_return true;
    }

    auto error = classify_ngtcp2_error(result.rv);
    auto detail = ngtcp2_error_message(result.rv);
    transport.fail_open_stream(cmd.open_result, error, detail);
    transport.fail_transport(error, detail);
    co_return false;
}

future<> consume_stream_data(connection_transport& transport, stream_id sid, size_t len) {
    if (!transport.has_transport_connection() || sid == invalid_stream_id || !len) {
        co_return;
    }

    auto rv = transport.consume_stream_data(sid, len);
    if (rv < 0) {
        transport.fail_transport(classify_ngtcp2_error(rv), ngtcp2_error_message(rv));
        co_return;
    }

    co_await flush_pending_transport_packets(transport);
    transport.rearm_transport_timer();
}

future<> reset_stream(connection_transport& transport, stream_id sid, application_error_code app_error_code) {
    if (!transport.has_transport_connection()) {
        co_return;
    }
    auto rv = transport.shutdown_stream_write(sid, app_error_code);
    if (rv < 0) {
        transport.fail_transport(classify_ngtcp2_error(rv), ngtcp2_error_message(rv));
        co_return;
    }
    co_await flush_pending_transport_packets(transport);
    transport.rearm_transport_timer();
}

future<> stop_sending(connection_transport& transport, stream_id sid, application_error_code app_error_code) {
    if (!transport.has_transport_connection()) {
        co_return;
    }
    auto rv = transport.shutdown_stream_read(sid, app_error_code);
    if (rv < 0) {
        transport.fail_transport(classify_ngtcp2_error(rv), ngtcp2_error_message(rv));
        co_return;
    }
    co_await flush_pending_transport_packets(transport);
    transport.rearm_transport_timer();
}

future<> retry_blocked_open_streams(connection_transport& transport, stream_type type) {
    if (!transport.can_retry_blocked_open_streams() || !transport.blocked_open_stream_retry_pending(type)) {
        co_return;
    }

    transport.clear_blocked_open_stream_retry(type);
    // Retry deferred opens until the queue drains or credit is exhausted again.
    while (transport.can_retry_blocked_open_streams()) {
        auto cmd = transport.pop_blocked_open_stream(type);
        if (!cmd) {
            co_return;
        }
        auto blocked = co_await open_stream(transport, std::move(*cmd));
        if (blocked) {
            co_return;
        }
    }
}

future<std::optional<transport_command>> handle_transport_command(connection_transport& transport, transport_command cmd) {
    // Returning a command means only that this specific operation is blocked and
    // should be requeued by the owning actor.
    switch (cmd.op) {
    case transport_command::kind::send: {
        auto blocked = co_await send_stream_message(transport, std::move(cmd.msg));
        if (blocked) {
            cmd.msg = std::move(*blocked);
            co_return std::make_optional(std::move(cmd));
        }
        break;
    }
    case transport_command::kind::open_stream:
        (void)co_await open_stream(transport, std::move(cmd));
        break;
    case transport_command::kind::consume_stream_data:
        co_await consume_stream_data(transport, cmd.msg.stream, cmd.consumed_bytes);
        break;
    case transport_command::kind::reset_stream:
        co_await reset_stream(transport, cmd.msg.stream, cmd.app_error_code);
        break;
    case transport_command::kind::stop_sending:
        co_await stop_sending(transport, cmd.msg.stream, cmd.app_error_code);
        break;
    case transport_command::kind::close_connection:
        transport.request_close();
        break;
    }
    co_return std::nullopt;
}

future<> recv_transport_datagram(connection_transport& transport, const socket_address& src, temporary_buffer<char> pkt) {
    if (!transport.transport_active() || !transport.has_transport_connection()) {
        co_return;
    }

    auto rv = transport.read_transport_datagram(src, pkt.get(), pkt.size());
    if (rv < 0) {
        if (ngtcp2_is_draining(rv)) {
            transport.stop_transport();
            co_return;
        }
        transport.fail_transport(classify_ngtcp2_error(rv), ngtcp2_error_message(rv));
        co_return;
    }

    // Packet processing can update the active path used for replies.
    transport.sync_transport_path();
    co_await flush_pending_transport_packets(transport);
    transport.rearm_transport_timer();
}

future<> handle_transport_timer(connection_transport& transport) {
    if (!transport.transport_active() || !transport.has_transport_connection()) {
        co_return;
    }

    auto now_local = transport_now_ns();
    // ngtcp2 uses the same expiry hook for PTO, loss recovery and idle timeout.
    if (transport.transport_expiry_ns() <= now_local) {
        auto rv = transport.handle_transport_expiry(now_local);
        if (rv < 0) {
            if (ngtcp2_is_idle_close(rv) || ngtcp2_is_draining(rv)) {
                transport.stop_transport();
                co_return;
            }
            transport.fail_transport(classify_ngtcp2_error(rv), ngtcp2_error_message(rv));
            co_return;
        }
    }

    co_await flush_pending_transport_packets(transport);
    transport.rearm_transport_timer();
}

future<> send_connection_close(connection_transport& transport) {
    if (!transport.can_send_connection_close()) {
        // Once the connection or datagram channel is gone, no close packet can be sent.
        co_return;
    }

    auto& outbuf = ensure_tx_packet_buffer(transport);
    auto nwrite = transport.write_connection_close_packet(
      reinterpret_cast<uint8_t*>(outbuf.get_write()),
      outbuf.size());
    if (nwrite == 0) {
        co_return;
    }
    if (nwrite < 0) {
        // Draining and idle-close already mean the peer does not need a new close frame.
        if (!ngtcp2_is_draining(static_cast<int>(nwrite)) && !ngtcp2_is_idle_close(static_cast<int>(nwrite))) {
            transport.fail_transport(
              classify_ngtcp2_error(static_cast<int>(nwrite)),
              ngtcp2_error_message(static_cast<int>(nwrite)));
        }
        co_return;
    }

    co_await transport.send_datagram_packet(outbuf.share(0, static_cast<size_t>(nwrite)));
}

connection_state_ptr make_connection_state(command_runtime_ptr command_runtime, connection_options options) {
    return make_shared<connection_state>(std::move(command_runtime), std::move(options));
}

} // namespace internal

namespace {

class quic_connected_socket_impl final : public net::connected_socket_impl {
public:
    explicit quic_connected_socket_impl(shared_ptr<internal::stream_state> state)
        : _state(std::move(state)) {
    }

    data_source source() override {
        return _state->source({});
    }

    data_source source(connected_socket_input_stream_config cfg) override {
        return _state->source(cfg);
    }

    data_sink sink() override {
        return _state->sink();
    }

    void shutdown_input() override {
        (void)_state->stop_sending(0).handle_exception([] (std::exception_ptr) {
        });
    }

    void shutdown_output() override {
        (void)_state->close_output().handle_exception([] (std::exception_ptr) {
        });
    }

    void set_nodelay(bool) override {
    }

    bool get_nodelay() const override {
        return true;
    }

    void set_keepalive(bool) override {
    }

    bool get_keepalive() const override {
        return false;
    }

    void set_keepalive_parameters(const net::keepalive_params&) override {
    }

    net::keepalive_params get_keepalive_parameters() const override {
        return net::tcp_keepalive_params{std::chrono::seconds(0), std::chrono::seconds(0), 0};
    }

    void set_sockopt(int, int, const void*, size_t) override {
    }

    int get_sockopt(int, int, void*, size_t) const override {
        return 0;
    }

    socket_address local_address() const noexcept override {
        return _state->local_address();
    }

    socket_address remote_address() const noexcept override {
        return _state->peer_address();
    }

    future<> wait_input_shutdown() override {
        return _state->wait_input_shutdown();
    }

private:
    shared_ptr<internal::stream_state> _state;
};

} // namespace

stream::stream() = default;
stream::~stream() = default;
stream::stream(stream&&) noexcept = default;
stream& stream::operator=(stream&&) noexcept = default;

stream::stream(std::unique_ptr<impl> state)
    : _impl(std::move(state)) {
}

bool stream::is_open() const noexcept {
    return _impl && _impl->state && _impl->state->is_open();
}

bool stream::can_read() const noexcept {
    return _impl && _impl->state && _impl->state->can_read();
}

bool stream::can_write() const noexcept {
    return _impl && _impl->state && _impl->state->can_write();
}

stream_id stream::id() const noexcept {
    return (_impl && _impl->state) ? _impl->state->id() : invalid_stream_id;
}

stream_type stream::type() const noexcept {
    return (_impl && _impl->state) ? _impl->state->type() : stream_type::bidirectional;
}

input_stream<char> stream::input(connected_socket_input_stream_config cfg) {
    if (!_impl || !_impl->state) {
        throw_quic_error(quic_error_code::invalid_state, "stream state is null");
    }
    return _impl->state->input(cfg);
}

output_stream<char> stream::output(size_t buffer_size) {
    if (!_impl || !_impl->state) {
        throw_quic_error(quic_error_code::invalid_state, "stream state is null");
    }
    return _impl->state->output(buffer_size);
}

future<> stream::close_output() {
    if (!_impl || !_impl->state) {
        return make_exception_future<>(quic_error(quic_error_code::invalid_state, "stream state is null"));
    }
    return _impl->state->close_output();
}

future<> stream::reset(application_error_code app_error_code) {
    if (!_impl || !_impl->state) {
        return make_exception_future<>(quic_error(quic_error_code::invalid_state, "stream state is null"));
    }
    return _impl->state->reset(app_error_code);
}

future<> stream::stop_sending(application_error_code app_error_code) {
    if (!_impl || !_impl->state) {
        return make_exception_future<>(quic_error(quic_error_code::invalid_state, "stream state is null"));
    }
    return _impl->state->stop_sending(app_error_code);
}

future<> stream::wait_input_shutdown() {
    if (!_impl || !_impl->state) {
        return make_exception_future<>(quic_error(quic_error_code::invalid_state, "stream state is null"));
    }
    return _impl->state->wait_input_shutdown();
}

connection::connection() = default;
connection::~connection() = default;
connection::connection(connection&&) noexcept = default;
connection& connection::operator=(connection&&) noexcept = default;

connection::connection(std::unique_ptr<impl> state)
    : _impl(std::move(state)) {
}

bool connection::is_open() const noexcept {
    return _impl && _impl->state && _impl->state->is_open();
}

socket_address connection::local_address() const {
    return (_impl && _impl->state) ? _impl->state->local_address() : socket_address();
}

socket_address connection::peer_address() const {
    return (_impl && _impl->state) ? _impl->state->peer_address() : socket_address();
}

sstring connection::selected_alpn() const {
    return (_impl && _impl->state) ? _impl->state->selected_alpn() : sstring();
}

future<stream> connection::open_stream(stream_open_options options) {
    if (!_impl || !_impl->state) {
        return make_exception_future<stream>(quic_error(quic_error_code::invalid_state, "connection state is null"));
    }
    return _impl->state->open_stream(options);
}

future<stream> connection::accept_stream() {
    if (!_impl || !_impl->state) {
        return make_exception_future<stream>(quic_error(quic_error_code::invalid_state, "connection state is null"));
    }
    return _impl->state->accept_stream();
}

future<> connection::close() {
    if (!_impl || !_impl->state) {
        co_return;
    }
    co_await _impl->state->close();
}

connected_socket to_connected_socket(stream&& s) {
    if (!s._impl || !s._impl->state) {
        throw_quic_error(quic_error_code::invalid_state, "stream state is null");
    }
    auto state = std::move(s._impl->state);
    if (!state->can_read() || !state->can_write()) {
        throw_quic_error(quic_error_code::invalid_state, "connected_socket requires a bidirectional stream");
    }
    return connected_socket(std::make_unique<quic_connected_socket_impl>(std::move(state)));
}

} // namespace seastar::quic::experimental

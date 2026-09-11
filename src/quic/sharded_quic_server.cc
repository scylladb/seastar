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

#include <seastar/quic/sharded_quic_server.hh>

#include <arpa/inet.h>
#include <errno.h>

#include <exception>
#include <memory>
#include <optional>
#include <ranges>
#include <system_error>
#include <unordered_map>
#include <utility>

#include <seastar/core/coroutine.hh>
#include <seastar/core/deleter.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/internal/run_in_background.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/log.hh>

#include "quic_common.hh"
#include "quic_error_impl.hh"

namespace seastar::quic::experimental {
namespace {

logger sharded_quic_server_log("sharded_quic_server");

uint16_t socket_address_port(const socket_address& address) noexcept {
    switch (address.family()) {
    case AF_INET:
        return ntohs(address.as_posix_sockaddr_in().sin_port);
    case AF_INET6:
        return ntohs(address.as_posix_sockaddr_in6().sin6_port);
    default:
        return 0;
    }
}

void set_socket_address_port(socket_address& address, uint16_t port) noexcept {
    switch (address.family()) {
    case AF_INET:
        address.as_posix_sockaddr_in().sin_port = htons(port);
        break;
    case AF_INET6:
        address.as_posix_sockaddr_in6().sin6_port = htons(port);
        break;
    default:
        break;
    }
}

bool is_bind_conflict(std::exception_ptr ep) noexcept {
    try {
        if (ep) {
            std::rethrow_exception(ep);
        }
    } catch (const std::system_error& e) {
        return e.code().value() == EADDRINUSE;
    } catch (...) {
    }
    return false;
}

using forwarded_packet_ptr = foreign_ptr<std::unique_ptr<temporary_buffer<char>>>;

temporary_buffer<char> make_shard_local_packet(forwarded_packet_ptr packet) {
    if (!packet) {
        return temporary_buffer<char>();
    }
    if (packet.get_owner_shard() == this_shard_id()) {
        return std::move(*packet);
    }
    auto* data = packet->get_write();
    auto size = packet->size();
    auto deleter = make_object_deleter(std::move(packet));
    return temporary_buffer<char>(data, size, std::move(deleter));
}

} // namespace

namespace internal {

class quic_server_shard;

thread_local std::unordered_map<socket_address, quic_server_shard*> quic_server_shards;

class quic_server_shard final : public quic_packet_router {
public:
    future<> start(quic_server_config config, quic_server_cid_key cid_key) {
        if (_started) {
            throw_quic_error(quic_error_code::invalid_state, "sharded QUIC server shard already started");
        }
        _server.set_packet_router(this);
        try {
            co_await _server.start_shard(
              std::move(config),
              std::move(cid_key),
              this_smp_shard_count() > 1);
        } catch (...) {
            _server.set_packet_router(nullptr);
            throw;
        }
        _local_address = _server.local_address();
        auto registration = quic_server_shards.emplace(_local_address, this);
        if (!registration.second) {
            _server.set_packet_router(nullptr);
            co_await _server.stop();
            throw_quic_error(quic_error_code::invalid_state, "another sharded QUIC server is already registered on this endpoint");
        }
        _registered = true;
        _started = true;
    }

    future<> serve(sharded_quic_server::accept_handler handler) {
        if (!_started) {
            throw_quic_error(quic_error_code::invalid_state, "sharded QUIC server shard is not started");
        }
        if (_accept_task) {
            throw_quic_error(quic_error_code::invalid_state, "sharded QUIC server shard is already serving");
        }

        _handler = std::move(handler);
        _accept_task.emplace(accept_loop());
        co_return;
    }

    future<> stop() {
        if (!_started && !_accept_task) {
            co_return;
        }

        unregister_router();
        _server.set_packet_router(nullptr);

        std::exception_ptr error;
        try {
            co_await _server.stop();
        } catch (...) {
            error = std::current_exception();
        }

        if (_accept_task) {
            try {
                co_await std::move(*_accept_task);
            } catch (...) {
                if (!error) {
                    error = std::current_exception();
                }
            }
            _accept_task.reset();
        }

        try {
            co_await _connections.close();
        } catch (...) {
            if (!error) {
                error = std::current_exception();
            }
        }

        _handler = {};
        _started = false;

        if (error) {
            std::rethrow_exception(error);
        }
    }

    socket_address local_address() const noexcept {
        return _server.local_address();
    }

    future<> route_quic_packet(unsigned shard, socket_address local_address, socket_address src, temporary_buffer<char> packet) override {
        if (shard >= this_smp_shard_count()) {
            co_return;
        }
        auto forwarded = make_foreign(std::make_unique<temporary_buffer<char>>(std::move(packet)));
        co_await smp::submit_to(shard, [local_address, src, packet = std::move(forwarded)] () mutable {
            auto it = quic_server_shards.find(local_address);
            if (it == quic_server_shards.end() || !it->second) {
                sharded_quic_server_log.debug("drop forwarded QUIC datagram: no server registered on shard {} for {}", this_shard_id(), local_address);
                return make_ready_future<>();
            }
            return it->second->inject_datagram(src, make_shard_local_packet(std::move(packet)));
        });
    }

    future<> inject_datagram(socket_address src, temporary_buffer<char> packet) {
        co_await _server.inject_datagram(src, std::move(packet));
    }

private:
    void unregister_router() noexcept {
        if (!_registered) {
            return;
        }
        auto it = quic_server_shards.find(_local_address);
        if (it != quic_server_shards.end() && it->second == this) {
            quic_server_shards.erase(it);
        }
        _registered = false;
    }

    future<> accept_loop() {
        while (true) {
            connection session;
            try {
                session = co_await _server.accept();
            } catch (const quic_error& e) {
                if (e.code() == quic_error_code::closed) {
                    co_return;
                }
                throw;
            }

            (void)with_gate(_connections, [this, session = std::move(session)] () mutable {
                return _handler(std::move(session));
            }).handle_exception([] (std::exception_ptr ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    sharded_quic_server_log.warn("accepted QUIC session handler failed on shard {}: {}", this_shard_id(), e.what());
                } catch (...) {
                    sharded_quic_server_log.warn("accepted QUIC session handler failed on shard {}", this_shard_id());
                }
            }).or_terminate();
        }
    }

private:
    quic_server _server;
    gate _connections;
    sharded_quic_server::accept_handler _handler;
    std::optional<future<>> _accept_task;
    socket_address _local_address;
    bool _registered = false;
    bool _started = false;
};

} // namespace internal

class sharded_quic_server::impl final {
public:
    impl()
      : _shards(std::make_unique<sharded<internal::quic_server_shard>>()) {
    }

    ~impl() {
        request_stop_detached();
    }

    future<> start(quic_server_config config) {
        if (_started || _shards_started) {
            throw_quic_error(quic_error_code::invalid_state, "sharded QUIC server already started");
        }

        ensure_gnutls_global();
        internal::quic_server_cid_key cid_key{};
        rand_bytes_or_throw(cid_key.data(), cid_key.size(), "sharded server CID routing key generation");

        co_await _shards->start();
        _shards_started = true;

        std::exception_ptr error;
        try {
            if (socket_address_port(config.listen_address) == 0) {
                co_await start_with_ephemeral_port(std::move(config), cid_key);
            } else {
                co_await start_on_all_shards(std::move(config), cid_key);
            }
            _started = true;
        } catch (...) {
            error = std::current_exception();
        }

        if (error) {
            try {
                co_await _shards->stop();
            } catch (...) {
            }
            _shards_started = false;
            if (this_smp_shard_count() > 1 && is_bind_conflict(error)) {
                throw_quic_error(quic_error_code::unsupported, "sharded QUIC server requires UDP SO_REUSEPORT for multi-shard bind");
            }
            std::rethrow_exception(error);
        }
    }

    future<> serve(sharded_quic_server::accept_handler_factory make_handler) {
        if (!_started) {
            throw_quic_error(quic_error_code::invalid_state, "sharded QUIC server is not started");
        }
        if (_serving) {
            throw_quic_error(quic_error_code::invalid_state, "sharded QUIC server is already serving");
        }

        std::exception_ptr error;
        try {
            co_await _shards->invoke_on_all([make_handler = std::move(make_handler)] (internal::quic_server_shard& shard) mutable {
                return shard.serve(make_handler());
            });
            _serving = true;
        } catch (...) {
            error = std::current_exception();
        }

        if (error) {
            co_await stop();
            std::rethrow_exception(error);
        }
    }

    future<> stop() {
        if (!_shards_started) {
            co_return;
        }

        std::exception_ptr error;
        try {
            co_await _shards->stop();
        } catch (...) {
            error = std::current_exception();
        }

        _started = false;
        _serving = false;
        _shards_started = false;
        _local_address = socket_address{};

        if (error) {
            std::rethrow_exception(error);
        }
    }

    socket_address local_address() const noexcept {
        return _local_address;
    }

private:
    void request_stop_detached() noexcept {
        if (!_shards_started || !_shards) {
            return;
        }

        sharded_quic_server_log.warn("sharded_quic_server destroyed without awaiting stop(); shutting down detached");
        auto shards = std::move(_shards);
        _started = false;
        _serving = false;
        _shards_started = false;
        _local_address = socket_address{};

        auto cleanup = shards->stop()
          .handle_exception([] (std::exception_ptr ep) {
              try {
                  std::rethrow_exception(ep);
              } catch (const std::exception& e) {
                  sharded_quic_server_log.warn("detached sharded QUIC server stop failed: {}", e.what());
              } catch (...) {
                  sharded_quic_server_log.warn("detached sharded QUIC server stop failed");
              }
          })
          .finally([shards = std::move(shards)] {});
        seastar::internal::run_in_background(std::move(cleanup));
    }

private:
    future<> start_with_ephemeral_port(
            quic_server_config config,
            internal::quic_server_cid_key cid_key) {
        co_await _shards->invoke_on(0, [config, cid_key] (internal::quic_server_shard& shard) mutable {
            return shard.start(std::move(config), std::move(cid_key));
        });

        auto local = co_await _shards->invoke_on(0, [] (internal::quic_server_shard& shard) {
            return make_ready_future<socket_address>(shard.local_address());
        });
        _local_address = local;
        set_socket_address_port(config.listen_address, socket_address_port(local));

        auto remaining_shards = std::views::iota(1u, this_smp_shard_count());
        co_await _shards->invoke_on(remaining_shards, [config = std::move(config), cid_key] (internal::quic_server_shard& shard) mutable {
            return shard.start(config, cid_key);
        });
    }

    future<> start_on_all_shards(
            quic_server_config config,
            internal::quic_server_cid_key cid_key) {
        co_await _shards->invoke_on_all([config, cid_key] (internal::quic_server_shard& shard) mutable {
            return shard.start(config, cid_key);
        });
        _local_address = co_await _shards->invoke_on(0, [] (internal::quic_server_shard& shard) {
            return make_ready_future<socket_address>(shard.local_address());
        });
    }

private:
    std::unique_ptr<sharded<internal::quic_server_shard>> _shards;
    socket_address _local_address;
    bool _started = false;
    bool _serving = false;
    bool _shards_started = false;
};

sharded_quic_server::sharded_quic_server()
  : _impl(std::make_unique<impl>()) {
}

sharded_quic_server::~sharded_quic_server() = default;

future<> sharded_quic_server::start(quic_server_config config) {
    return _impl->start(std::move(config));
}

future<> sharded_quic_server::serve(accept_handler_factory make_handler) {
    return _impl->serve(std::move(make_handler));
}

future<> sharded_quic_server::stop() {
    return _impl->stop();
}

socket_address sharded_quic_server::local_address() const noexcept {
    return _impl->local_address();
}

} // namespace seastar::quic::experimental

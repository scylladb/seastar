// Pub/Sub: SUBSCRIBE, UNSUBSCRIBE and PUBLISH, with subscribers on every
// shard.
#include "pubsub.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/exception.hh>
#include <fmt/format.h>
#include <functional>
#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

std::string lowercase(std::string_view s) {
    std::string ret(s);
    std::ranges::transform(ret, ret.begin(), [] (unsigned char c) {
        return std::tolower(c);
    });
    return ret;
}

sstring wrong_arguments(std::string_view command) {
    return error_reply(fmt::format(
        "wrong number of arguments for '{}' command", command));
}

// The confirmation of a SUBSCRIBE or UNSUBSCRIBE: its kind, the channel,
// and the number of channels the client is now subscribed to. A null
// channel says that the client wasn't subscribed to any.
sstring confirmation(std::string_view kind, const sstring* channel, size_t count) {
    return array_header_reply(3) + bulk_string_reply(kind)
           + (channel ? bulk_string_reply(*channel) : null_reply())
           + integer_reply(count);
}

// A published message, as a subscriber receives it.
sstring message_reply(std::string_view channel, std::string_view message) {
    return array_header_reply(3) + bulk_string_reply("message")
           + bulk_string_reply(channel) + bulk_string_reply(message);
}

} // anonymous namespace

// snippet: send
// What a message waiting to be written costs in memory: its bytes, rounded
// up by the allocator by up to a quarter, and the queue's slot. (We
// measured Shoal's memory with a subscriber that never reads: 157 bytes
// for a message of 130, 1,321 for a message of 1,030.)
size_t waiting_cost(const sstring& data) {
    return data.size() + data.size() / 4 + 64;
}

// The writer stops when the queue is aborted with this.
struct writer_stopped {};

client::client(connected_socket& socket, output_stream<char>& out)
    : _socket(socket), _out(out)
    , _queue(std::numeric_limits<size_t>::max())
    , _writer(write_messages()) {}

void client::send(sstring data, semaphore_units<> units) {
    if (_failure) {
        return;     // the connection is broken: drop the data, and its units
    }
    _pending += waiting_cost(data);
    _queue.push(message{std::move(data), std::move(units)});
}

future<> client::write_messages() {
    try {
        while (true) {
            // Wait for messages, and write all that are waiting, before
            // one flush.
            co_await _queue.not_empty();
            size_t written = 0;
            while (!_queue.empty()) {
                auto m = _queue.pop();
                written += waiting_cost(m.data);
                co_await _out.write(m.data);
            }
            co_await _out.flush();
            _pending -= written;
            if (_pending == 0 && _written.has_waiters()) {
                _written.broadcast();
            }
        }
    } catch (writer_stopped&) {
        // close() stopped the writer, after everything was written.
    } catch (...) {
        // Writing failed: the connection is broken. Drop what's waiting, and
        // make drain() fail too.
        _failure = std::current_exception();
        _queue.abort(_failure);
        _written.broken(_failure);
    }
}

future<> client::drain() {
    co_await _written.when([this] { return _pending == 0; });
}

future<> client::write(sstring data) {
    send(std::move(data));
    return drain();
}

future<> client::reply(sstring data) {
    if (!_channels.empty() || _pending) {
        co_return co_await write(std::move(data));
    }
    co_await _out.write(data);
    co_await _out.flush();
}

future<> client::close() {
    std::exception_ptr failure;
    try {
        co_await drain();
    } catch (...) {
        failure = std::current_exception();
    }
    _queue.abort(std::make_exception_ptr(writer_stopped{}));
    co_await std::move(_writer);
    if (failure) {
        co_await coroutine::return_exception_ptr(std::move(failure));
    }
}
// end snippet: send

void client::disconnect() {
    _queue.abort(std::make_exception_ptr(std::runtime_error("disconnected")));
    _socket.shutdown_output();
    _socket.shutdown_input();
}

bool is_pubsub_command(const request& req) {
    auto name = lowercase(req[0]);
    return name == "subscribe" || name == "unsubscribe" || name == "publish";
}

future<> pubsub::execute(client& c, const request& req) {
    auto name = lowercase(req[0]);
    if (name == "subscribe") {
        return subscribe(c, req);
    } else if (name == "unsubscribe") {
        return unsubscribe(c, req);
    } else if (c.channels().empty()) {
        // is_pubsub_command() lets only PUBLISH through here.
        return publish(c, req);
    } else if (name == "ping" && req.size() <= 2) {
        // A subscribed client's PING gets an array, like a message.
        return c.write(array_header_reply(2) + bulk_string_reply("pong")
                       + bulk_string_reply(req.size() == 2 ? req[1] : sstring()));
    }
    return c.write(error_reply(fmt::format(
        "Can't execute '{}': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / "
        "PING / QUIT / RESET are allowed in this context", name)));
}

// snippet: subscribe
// SUBSCRIBE channel [channel ...]
future<> pubsub::subscribe(client& c, const request& req) {
    if (req.size() < 2) {
        co_return co_await c.write(wrong_arguments("subscribe"));
    }
    for (size_t i = 1; i < req.size(); i++) {
        if (c.channels().insert(req[i]).second) {
            _channels[req[i]].insert(&c);
        }
        c.send(confirmation("subscribe", &req[i], c.channels().size()));
    }
    co_await c.drain();
}
// end snippet: subscribe

void pubsub::remove(client& c, const sstring& channel) {
    c.channels().erase(channel);
    auto it = _channels.find(channel);
    if (it != _channels.end()) {
        it->second.erase(&c);
        if (it->second.empty()) {
            _channels.erase(it);
        }
    }
}

// UNSUBSCRIBE [channel ...]
future<> pubsub::unsubscribe(client& c, const request& req) {
    if (req.size() == 1) {
        // Without arguments, unsubscribe from every channel.
        if (c.channels().empty()) {
            c.send(confirmation("unsubscribe", nullptr, 0));
        }
        std::vector<sstring> all(c.channels().begin(), c.channels().end());
        for (auto& channel : all) {
            remove(c, channel);
            c.send(confirmation("unsubscribe", &channel, c.channels().size()));
        }
    }
    for (size_t i = 1; i < req.size(); i++) {
        remove(c, req[i]);
        c.send(confirmation("unsubscribe", &req[i], c.channels().size()));
    }
    co_await c.drain();
}

void pubsub::forget(client& c) {
    std::vector<sstring> all(c.channels().begin(), c.channels().end());
    for (auto& channel : all) {
        remove(c, channel);
    }
}

// snippet: publish
// PUBLISH channel message
future<> pubsub::publish(client& c, const request& req) {
    if (req.size() != 3) {
        co_return co_await c.write(wrong_arguments("publish"));
    }
    // Subscribers may be on any shard: every shard sends the message to its
    // own subscribers, and the counts are added up.
    auto count = co_await container().map_reduce0(
        [&channel = req[1], &message = req[2]] (pubsub& ps) {
            return ps.publish_here(channel, message);
        }, size_t(0), std::plus<size_t>());
    co_await c.reply(integer_reply(count));
}

future<size_t> pubsub::publish_here(const sstring& channel,
                                    const sstring& data) {
    auto message = message_reply(channel, data);
    size_t cost = waiting_cost(message);
    // Wait until there is room for the message's copies to all the
    // subscribers: when too much is waiting to be written, publishers wait,
    // and so they publish at the pace at which subscribers read. (A huge
    // message waits for all the units, not for more than there are.)
    size_t subscribers = 0;
    if (auto it = _channels.find(channel); it != _channels.end()) {
        subscribers = it->second.size();
    }
    auto units = co_await get_units(
        _memory, std::min(cost * subscribers, _memory_limit));
    // While we waited, clients may have subscribed or unsubscribed, so look
    // at the subscribers again, and send to them without waiting.
    size_t count = 0;
    if (auto it = _channels.find(channel); it != _channels.end()) {
        std::vector<client*> current(it->second.begin(), it->second.end());
        for (client* s : current) {
            // Like Redis, disconnect a subscriber that doesn't read its
            // messages fast enough, so that one slow subscriber doesn't
            // hold up every publisher.
            if (s->pending() > _max_pending) {
                shoal_log.warn("disconnecting a subscriber with {} bytes of "
                               "unsent messages", s->pending());
                forget(*s);
                s->disconnect();
                continue;
            }
            s->send(message, units.split(std::min(cost, units.count())));
            count++;
        }
    }
    co_return count;
}
// end snippet: publish

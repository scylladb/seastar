// Pub/Sub: SUBSCRIBE, UNSUBSCRIBE and PUBLISH.
#include "pubsub.hh"
#include <seastar/core/coroutine.hh>
#include <seastar/coroutine/exception.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <fmt/format.h>
#include <algorithm>
#include <cctype>
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
// A message waiting to be written costs more memory than its bytes: the
// frame of write_after(), the shared_future and the allocations, and the
// allocator's rounding. We measured Shoal's memory with a subscriber that
// never reads: 613 bytes more than a message of 100, 919 more than one of
// 1,000. So this is too little, but good enough to keep Shoal alive.
constexpr size_t write_overhead = 512;

void client::send(sstring data) {
    size_t cost = data.size() + write_overhead;
    _pending += cost;
    _total_pending += cost;
    _last_write = write_after(_last_write.get_future(), std::move(data), cost);
}

future<> client::write_after(future<> previous, sstring data, size_t cost) {
    std::exception_ptr failure;
    try {
        co_await std::move(previous);
        co_await _out.write(data);
        co_await _out.flush();
    } catch (...) {
        failure = std::current_exception();
    }
    // Written or failed, the message no longer waits.
    _pending -= cost;
    _total_pending -= cost;
    if (failure) {
        co_await coroutine::return_exception_ptr(std::move(failure));
    }
}

future<> client::write(sstring data) {
    send(std::move(data));
    return drain();
}
// end snippet: send

void client::disconnect() {
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

// PUBLISH channel message
future<> pubsub::publish(client& c, const request& req) {
    if (req.size() != 3) {
        co_return co_await c.write(wrong_arguments("publish"));
    }
    const sstring& channel = req[1];
    // snippet: subscribers
    // Copy the list of subscribers: while we wait for one of them, others
    // may subscribe or unsubscribe, which changes the set.
    std::vector<client*> subscribers;
    if (auto it = _channels.find(channel); it != _channels.end()) {
        subscribers.assign(it->second.begin(), it->second.end());
    }
    // end snippet: subscribers
    auto message = message_reply(channel, req[2]);
    // PUBLISH replies with the number of subscribers it sent the message to.
    size_t count = subscribers.size();
    switch (_delivery) {
    case delivery::sequential:
        // snippet: sequential
        for (client* s : subscribers) {
            // While we waited for the previous subscriber, this one may
            // have unsubscribed, and even disconnected.
            auto it = _channels.find(channel);
            if (it == _channels.end() || !it->second.contains(s)) {
                continue;
            }
            try {
                co_await s->write(message);
            } catch (...) {
                // The subscriber's connection failed; it reports that itself.
            }
        }
        // end snippet: sequential
        break;
    case delivery::parallel:
        // snippet: parallel
        co_await coroutine::parallel_for_each(subscribers, [&] (client* s) {
            return s->write(message).handle_exception([] (std::exception_ptr) {
                // The subscriber's connection failed; it reports that itself.
            });
        });
        // end snippet: parallel
        break;
    case delivery::background:
        // snippet: background
        for (client* s : subscribers) {
            // Like Redis, disconnect a subscriber that doesn't read its
            // messages fast enough, instead of letting them pile up. When
            // all subscribers together hold too much, disconnect those
            // that are behind, before the shard runs out of memory.
            if (s->pending() > _max_pending ||
                    (_total_pending > _max_total_pending && s->pending() > 0)) {
                shoal_log.warn("disconnecting a subscriber with {} bytes of "
                               "unsent messages", s->pending());
                forget(*s);
                s->disconnect();
                count--;
                continue;
            }
            s->send(message);
        }
        // end snippet: background
        break;
    }
    co_await c.write(integer_reply(count));
}

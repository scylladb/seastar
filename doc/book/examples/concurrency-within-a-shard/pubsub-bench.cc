// pubsub-bench: measures how fast a Redis-compatible server delivers
// published messages. It subscribes many clients to one channel, publishes
// messages to it from one or more publishers, and counts what the
// subscribers receive.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/map_reduce.hh>
#include <seastar/core/seastar.hh>
#include <seastar/coroutine/all.hh>
#include <seastar/coroutine/parallel_for_each.hh>
#include <seastar/net/api.hh>
#include <boost/program_options.hpp>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <optional>
#include <print>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace seastar;
namespace bpo = boost::program_options;

constexpr std::string_view channel = "bench";

// A RESP request.
sstring command(std::initializer_list<std::string_view> words) {
    auto ret = seastar::format("*{}\r\n", words.size());
    for (auto w : words) {
        ret += seastar::format("${}\r\n{}\r\n", w.size(), w);
    }
    return ret;
}

// snippet: connect-to
// A connection to the server, with its two streams.
struct connection {
    connected_socket socket;
    input_stream<char> in;
    output_stream<char> out;
    explicit connection(connected_socket s)
        : socket(std::move(s)), in(socket.input()), out(socket.output()) {}
};

future<connection> connect_to(uint16_t port) {
    auto s = co_await connect(make_ipv4_address({"127.0.0.1", port}));
    co_return connection(std::move(s));
}
// end snippet: connect-to

// Reads exactly the bytes that we expect, or throws.
future<> expect(connection& c, std::string_view expected) {
    auto buf = co_await c.in.read_exactly(expected.size());
    if (std::string_view(buf.get(), buf.size()) != expected) {
        throw std::runtime_error("unexpected reply");
    }
}

// snippet: subscribe
// Connects a subscriber, and puts its connection in slot.
future<> subscribe(uint16_t port, std::optional<connection>& slot) {
    auto& c = slot.emplace(co_await connect_to(port));
    co_await c.out.write(command({"SUBSCRIBE", channel}));
    co_await c.out.flush();
    co_await expect(c, "*3\r\n$9\r\nsubscribe\r\n$5\r\nbench\r\n:1\r\n");
}
// end snippet: subscribe

// snippet: receive
// Receives the expected number of messages, all of the same size, and
// returns how many it received.
future<size_t> receive(connection& c, size_t size, size_t expected) {
    sstring header = seastar::format("*3\r\n$7\r\nmessage\r\n$5\r\nbench\r\n${}\r\n", size);
    for (size_t count = 0; count < expected; count++) {
        auto buf = co_await c.in.read_exactly(header.size() + size + 2);
        if (buf.size() < header.size() + size + 2) {
            throw std::runtime_error("connection closed");
        }
    }
    co_return expected;
}
// end snippet: receive

// Reads one integer reply, such as ":100\r\n", to its end.
future<> read_integer_reply(connection& c) {
    char last = 0;
    while (last != '\n') {
        auto b = co_await c.in.read_exactly(1);
        if (b.empty()) {
            throw std::runtime_error("connection closed");
        }
        last = b[0];
    }
}

// Publishes messages, a pipeline of them at a time: it sends that many
// PUBLISH requests, and then reads their replies.
future<> publish(uint16_t port, unsigned messages, size_t size, unsigned pipeline) {
    auto c = co_await connect_to(port);
    sstring message(size, 'x');
    unsigned sent = 0;
    while (sent < messages) {
        unsigned batch = std::min(pipeline, messages - sent);
        for (unsigned i = 0; i < batch; i++) {
            co_await c.out.write(command({"PUBLISH", channel, message}));
        }
        sent += batch;
        co_await c.out.flush();
        for (unsigned i = 0; i < batch; i++) {
            co_await read_integer_reply(c);
        }
    }
    co_await c.out.close();
}

struct options {
    uint16_t port;
    unsigned subscribers;
    unsigned messages;
    size_t size;
    unsigned max_connecting;
    unsigned pipeline;
    unsigned publishers;
};

future<> run(options opts) {
    // snippet: connect-all
    std::vector<std::optional<connection>> subscribers(opts.subscribers);
    // Connect the subscribers, but not all at once: at most max_connecting
    // connections are being set up at any time.
    co_await max_concurrent_for_each(subscribers, opts.max_connecting,
            [port = opts.port] (std::optional<connection>& slot) {
        return subscribe(port, slot);
    });
    // end snippet: connect-all
    auto start = std::chrono::steady_clock::now();
    // snippet: run-all
    size_t expected = size_t(opts.messages) * opts.publishers;
    auto [received] = co_await coroutine::all(
        // The subscribers: count the messages they receive.
        [&] {
            // Run receive() on every subscriber, and add up what they return.
            return map_reduce(subscribers,
                    [&] (std::optional<connection>& c) {
                        return receive(*c, opts.size, expected);
                    },
                    size_t(0), std::plus<size_t>());
        },
        // The publishers: each publishes its messages.
        [&] {
            return parallel_for_each(std::views::iota(0u, opts.publishers),
                    [&] (unsigned) {
                        return publish(opts.port, opts.messages, opts.size,
                                       opts.pipeline);
                    });
        });
    // end snippet: run-all
    std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
    std::println("{} subscribers received {} messages of {} bytes from {} "
                 "publishers in {:.2f} s: {:.0f} deliveries per second",
                 opts.subscribers, received, opts.size, opts.publishers,
                 took.count(), received / took.count());
    co_await coroutine::parallel_for_each(subscribers, [] (std::optional<connection>& c) {
        return c->out.close();
    });
}

int main(int argc, char** argv) {
    app_template app;
    app.add_options()
        ("port", bpo::value<uint16_t>()->default_value(6379), "the server's port")
        ("subscribers", bpo::value<unsigned>()->default_value(100), "subscribers to connect")
        ("messages", bpo::value<unsigned>()->default_value(10000), "messages to publish")
        ("size", bpo::value<size_t>()->default_value(100), "the size of each message")
        ("max-connecting", bpo::value<unsigned>()->default_value(10),
         "connections being set up at once")
        ("pipeline", bpo::value<unsigned>()->default_value(16),
         "PUBLISH requests sent before reading their replies")
        ("publishers", bpo::value<unsigned>()->default_value(1),
         "publishers, each publishing --messages messages");
    return app.run(argc, argv, [&app] {
        auto& config = app.configuration();
        return run(options{
            .port = config["port"].as<uint16_t>(),
            .subscribers = config["subscribers"].as<unsigned>(),
            .messages = config["messages"].as<unsigned>(),
            .size = config["size"].as<size_t>(),
            .max_connecting = config["max-connecting"].as<unsigned>(),
            .pipeline = config["pipeline"].as<unsigned>(),
            .publishers = config["publishers"].as<unsigned>(),
        });
    });
}

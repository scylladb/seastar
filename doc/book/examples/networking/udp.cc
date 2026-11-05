// A UDP server and its client, in one program: the server answers each
// datagram with the same text in capital letters.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <fmt/format.h>
#include <cctype>
#include <string>

using namespace seastar;

// The text of a datagram, which may come in several buffers.
std::string text(net::datagram& d) {
    std::string s;
    for (auto& buf : d.get_buffers()) {
        s.append(buf.get(), buf.size());
    }
    return s;
}

// snippet: server
// Answers n datagrams, each to the address it came from.
future<> serve(net::datagram_channel& channel, int n) {
    for (int i = 0; i < n; i++) {
        auto d = co_await channel.receive();
        auto reply = text(d);
        for (auto& c : reply) {
            c = std::toupper(static_cast<unsigned char>(c));
        }
        co_await channel.send(d.get_src(), reply.c_str());
    }
}
// end snippet: server

// snippet: client
// Sends each word to the server, and waits for its answer.
future<> ask(socket_address server) {
    auto channel = make_unbound_datagram_channel(AF_INET);
    for (auto word : {"hello", "datagram"}) {
        co_await channel.send(server, word);
        auto d = co_await channel.receive();
        fmt::print("sent {}, received {} from {}\n", word, text(d),
                   d.get_src());
    }
    channel.close();
}
// end snippet: client

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] -> future<> {
        // snippet: bind
        auto channel = make_bound_datagram_channel(
            make_ipv4_address({"127.0.0.1", 0}));
        // end snippet: bind
        auto server = serve(channel, 2);
        co_await ask(channel.local_address());
        co_await std::move(server);
        channel.close();
    });
}

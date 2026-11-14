// Chapter 16, exercise 5: udp.cc, with a server that ignores every other
// datagram, and a client that sends its datagram again every 100
// milliseconds until an answer comes.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/timer.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <fmt/format.h>
#include <cctype>
#include <string>

using namespace seastar;
using namespace std::chrono_literals;

// The text of a datagram, which may come in several buffers.
std::string text(net::datagram& d) {
    std::string s;
    for (auto& buf : d.get_buffers()) {
        s.append(buf.get(), buf.size());
    }
    return s;
}

// Answers n datagrams, each to the address it came from, but ignores
// every other datagram it receives, as if the network had lost it.
future<> serve(net::datagram_channel& channel, int n) {
    for (int received = 0, answered = 0; answered < n; received++) {
        auto d = co_await channel.receive();
        if (received % 2 == 0) {
            fmt::print("server: lost {}\n", text(d));
            continue;
        }
        answered++;
        auto reply = text(d);
        for (auto& c : reply) {
            c = std::toupper(static_cast<unsigned char>(c));
        }
        co_await channel.send(d.get_src(), reply.c_str());
    }
}

// Sends each word to the server, and again every 100 milliseconds, until
// its answer comes.
future<> ask(socket_address server) {
    auto channel = make_unbound_datagram_channel(AF_INET);
    for (auto word : {"hello", "datagram"}) {
        auto answer = channel.receive();
        // The timer's sends are in the background: the gate waits for
        // them before the channel is closed.
        gate sending;
        auto send = [&] {
            (void)with_gate(sending, [&] {
                return channel.send(server, word);
            }).handle_exception([] (auto) {});
        };
        timer<> again(send);
        send();
        again.arm_periodic(100ms);
        auto d = co_await std::move(answer);
        again.cancel();
        co_await sending.close();
        fmt::print("sent {}, received {} from {}\n", word, text(d),
                   d.get_src());
    }
    channel.close();
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] -> future<> {
        auto channel = make_bound_datagram_channel(
            make_ipv4_address({"127.0.0.1", 0}));
        auto server = serve(channel, 2);
        co_await ask(channel.local_address());
        co_await std::move(server);
        channel.close();
    });
}

// Seastar's RPC: a server and a client of a small protocol, in one program,
// and what a client sees when a call succeeds, fails, takes too long, or
// finds the connection closed. Run it with one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/byteorder.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/net/inet_address.hh>
#include <seastar/rpc/rpc.hh>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <exception>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

constexpr uint16_t port = 10090;

// snippet: serializer
// How the protocol's values are written into a message, and read back:
// integers in little-endian order, and a string as its length and its
// bytes. RPC finds these functions by their first parameter's type.
struct serializer {};

template <typename Output, std::integral T>
void write(serializer, Output& out, T v) {
    v = cpu_to_le(v);
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

template <typename Input, std::integral T>
T read(serializer, Input& in, rpc::type<T>) {
    T v;
    in.read(reinterpret_cast<char*>(&v), sizeof(v));
    return le_to_cpu(v);
}

template <typename Output>
void write(serializer s, Output& out, const sstring& v) {
    write(s, out, uint32_t(v.size()));
    out.write(v.data(), v.size());
}

template <typename Input>
sstring read(serializer s, Input& in, rpc::type<sstring>) {
    auto size = read(s, in, rpc::type<uint32_t>());
    auto v = uninitialized_string(size);
    in.read(v.data(), size);
    return v;
}
// end snippet: serializer

// snippet: verbs
// The protocol's verbs: the numbers that tell the server which handler a
// message is for. They never change, once a version is released.
enum class verb : uint32_t {
    add = 1,
    fail = 2,
    wait = 3,
    note = 4,
    stats = 5,              // not handled by this server
};

using protocol = rpc::protocol<serializer, verb>;

void register_handlers(protocol& proto) {
    // A third number was added later: a client that sends two still works.
    proto.register_handler(verb::add, [] (int64_t a, int64_t b,
                                          rpc::optional<int64_t> c) {
        return a + b + c.value_or(0);
    });
    proto.register_handler(verb::fail, [] (sstring key) -> int64_t {
        throw std::runtime_error(fmt::format("no key '{}'", key));
    });
    proto.register_handler(verb::wait, [] (uint32_t ms) {
        return sleep(std::chrono::milliseconds(ms));
    });
    // One way: the client gets no reply, and doesn't wait for one.
    proto.register_handler(verb::note, [] (const rpc::client_info& info,
                                           sstring text) {
        std::println("note from {}: {}", fmt::to_string(info.addr.addr()),
                     text);
        return rpc::no_wait;
    });
}
// end snippet: verbs

// snippet: calls
// Calls each verb, and prints what came back.
future<> call_verbs(protocol& proto, protocol::client& client) {
    auto add = proto.make_client<int64_t (int64_t, int64_t, int64_t)>(
        verb::add);
    auto old_add = proto.make_client<int64_t (int64_t, int64_t)>(verb::add);
    auto fail = proto.make_client<int64_t (sstring)>(verb::fail);
    auto wait = proto.make_client<void (uint32_t)>(verb::wait);
    auto note = proto.make_client<rpc::no_wait_type (sstring)>(verb::note);
    auto stats = proto.make_client<int64_t ()>(verb::stats);

    std::println("1 + 2 + 3 = {}", co_await add(client, 1, 2, 3));
    std::println("an older client's 1 + 2 = {}",
                 co_await old_add(client, 1, 2));
    co_await note(client, "hello");
    try {
        co_await fail(client, "x");
    } catch (rpc::remote_verb_error& e) {
        std::println("fail: remote_verb_error: {}", e.what());
    }
    try {
        // Wait at most 100 ms for the reply of a call that takes a second.
        co_await wait(client, 100ms, 1000);
    } catch (rpc::timeout_error& e) {
        std::println("wait: timeout_error: {}", e.what());
    }
    try {
        co_await stats(client);
    } catch (rpc::unknown_verb_error& e) {
        std::println("stats: unknown_verb_error: verb {}", e.type);
    }
}
// end snippet: calls

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: start
        protocol proto(serializer{});
        register_handlers(proto);
        auto address = socket_address(net::inet_address("127.0.0.1"), port);
        protocol::server server(proto, address);
        protocol::client client(proto, address);
        // end snippet: start
        std::exception_ptr error;
        try {
            co_await call_verbs(proto, client);
        } catch (...) {
            error = std::current_exception();
        }
        // snippet: closed
        // A server that stops closes its connections: the client's calls
        // fail from then on, and it must be replaced. A server can only be
        // stopped once.
        co_await server.stop();
        auto add = proto.make_client<int64_t (int64_t, int64_t)>(verb::add);
        try {
            co_await add(client, 1, 2);
        } catch (rpc::closed_error& e) {
            std::println("after the server stopped: closed_error: {}",
                         e.what());
        }
        co_await client.stop();
        // end snippet: closed
        if (error) {
            std::rethrow_exception(error);
        }
    });
}

// A server that counts the bytes each client sends, and tells the client
// the count when it closes its side of the connection.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <fmt/format.h>

using namespace seastar;

// snippet: count-bytes
// Reads until the client closes its side, and returns how many bytes it sent.
future<size_t> count_bytes(input_stream<char>& in) {
    size_t n = 0;
    while (true) {
        auto buf = co_await in.read();
        if (buf.empty()) {
            co_return n;
        }
        n += buf.size();
    }
}
// end snippet: count-bytes

// snippet: report
// Tells the client how many bytes it sent.
future<> report(connected_socket s) {
    auto in = s.input();
    auto out = s.output();
    size_t n = co_await count_bytes(in);
    co_await out.write(fmt::format("{} bytes\n", n));
    co_await out.close();
}
// end snippet: report

logger applog("count-bytes");

future<> serve(uint16_t port) {
    listen_options opts;
    opts.reuse_address = true;
    auto listener = listen(make_ipv4_address({port}), opts);
    while (true) {
        auto [socket, address] = co_await listener.accept();
        (void)report(std::move(socket)).handle_exception(
            [] (std::exception_ptr e) {
                applog.warn("connection failed: {}", formattable(e));
            });
    }
}

int main(int argc, char** argv) {
    app_template::config cfg;
    cfg.auto_handle_sigint_sigterm = false;
    app_template app(std::move(cfg));
    return app.run(argc, argv, [] {
        return serve(1234);
    });
}

// The connection handler from chapter 1, in a complete server: each request
// is a line of text, and the reply is the same line in upper case.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <cctype>
#include <optional>
#include <string>

using namespace seastar;

logger applog("line-server");

// Reads one line, without its newline, or returns nothing when the client
// has closed the connection. It reads one byte at a time, which is simple
// but slow; later chapters read requests efficiently.
future<std::optional<sstring>> read_request(input_stream<char>& in) {
    std::string line;
    while (true) {
        auto c = co_await in.read_exactly(1);
        if (c.empty()) {
            co_return std::nullopt;
        }
        if (c[0] == '\n') {
            co_return sstring(line);
        }
        line += c[0];
    }
}

future<sstring> execute(sstring request) {
    for (auto& c : request) {
        c = std::toupper(static_cast<unsigned char>(c));
    }
    co_return request + "\n";
}

// snippet: handle-connection
future<> handle_connection(connected_socket s) {
    auto in = s.input();
    auto out = s.output();
    while (auto request = co_await read_request(in)) {
        co_await out.write(co_await execute(*request));
        co_await out.flush();
    }
    co_await out.close();
}
// end snippet: handle-connection

future<> serve(uint16_t port) {
    listen_options opts;
    opts.reuse_address = true;
    auto listener = listen(make_ipv4_address({port}), opts);
    while (true) {
        auto [socket, address] = co_await listener.accept();
        (void)handle_connection(std::move(socket)).handle_exception(
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
        return smp::invoke_on_all([] {
            return serve(1235);
        });
    });
}

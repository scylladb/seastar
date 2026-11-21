// Chapter 2, exercise 3: an echo server that echoes whole lines only.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <string>
#include <string_view>

using namespace seastar;

// snippet: handle-connection
// Serves one client until it disconnects, echoing whole lines only.
future<> handle_connection(connected_socket s) {
    auto in = s.input();
    auto out = s.output();
    // What arrived after the last newline: the start of a line.
    std::string line;
    while (true) {
        auto buf = co_await in.read();
        if (buf.empty()) {
            break;          // the client closed its side
        }
        std::string_view data(buf.get(), buf.size());
        while (true) {
            auto newline = data.find('\n');
            if (newline == data.npos) {
                line += data;
                break;
            }
            line += data.substr(0, newline + 1);
            co_await out.write(line);
            co_await out.flush();
            line.clear();
            data.remove_prefix(newline + 1);
        }
    }
    co_await out.close();
}
// end snippet: handle-connection

// snippet: serve
// The program's logger. Its name, "echo", appears in every message it logs.
logger applog("echo");

// Accepts connections forever, serving each one in the background.
future<> serve(uint16_t port) {
    listen_options opts;
    opts.reuse_address = true;
    auto listener = listen(make_ipv4_address({port}), opts);
    while (true) {
        auto [socket, address] = co_await listener.accept();
        applog.info("shard {} serves {}", this_shard_id(), address);
        (void)handle_connection(std::move(socket)).handle_exception(
            [] (std::exception_ptr e) {
                applog.warn("connection failed: {}", formattable(e));
            });
    }
}
// end snippet: serve

// snippet: main
int main(int argc, char** argv) {
    // Let Ctrl-C (SIGINT) and kill (SIGTERM) end the program at once.
    app_template::config cfg;
    cfg.auto_handle_sigint_sigterm = false;
    app_template app(std::move(cfg));
    return app.run(argc, argv, [] {
        return serve(1234);
    });
}
// end snippet: main

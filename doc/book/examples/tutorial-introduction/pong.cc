// Answers every line with "+PONG", the reply to a Redis PING, on every shard.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <seastar/core/smp.hh>
#include <seastar/util/log.hh>

using namespace seastar;

logger applog("pong");

// snippet: handle-connection
// Serves one client until it disconnects.
future<> handle_connection(connected_socket s) {
    auto in = s.input();
    auto out = s.output();
    while (true) {
        auto buf = co_await in.read();
        if (buf.empty()) {
            break;          // the client closed its side
        }
        // snippet: pong
        for (char c : buf) {
            if (c == '\n') {
                co_await out.write("+PONG\r\n");
            }
        }
        // end snippet: pong
        co_await out.flush();
    }
    co_await out.close();
}
// end snippet: handle-connection

// snippet: serve
// Accepts connections forever, serving each one in the background.
future<> serve(uint16_t port) {
    listen_options opts;
    opts.reuse_address = true;
    auto listener = listen(make_ipv4_address({port}), opts);
    while (true) {
        auto [socket, address] = co_await listener.accept();
        (void)handle_connection(std::move(socket)).handle_exception(
            [] (std::exception_ptr e) {
                applog.warn("connection failed: {}", e);
            });
    }
}
// end snippet: serve

int main(int argc, char** argv) {
    // snippet: config
    // Let Ctrl-C (SIGINT) and kill (SIGTERM) end the program at once.
    app_template::config cfg;
    cfg.auto_handle_sigint_sigterm = false;
    app_template app(std::move(cfg));
    // end snippet: config
    return app.run(argc, argv, [] {
        // snippet: main
        return smp::invoke_on_all([] {
            return serve(6380);
        });
        // end snippet: main
    });
}

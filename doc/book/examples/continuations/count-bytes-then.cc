// Chapter 4's server, which counts the bytes each client sends and tells
// the client the count when it closes its side of the connection, written
// with continuations instead of coroutines.
#include <seastar/core/app-template.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <fmt/format.h>

using namespace seastar;

// snippet: count-bytes
// Reads until the client closes its side, and returns how many bytes it sent.
future<size_t> count_bytes(input_stream<char>& in) {
    return do_with(size_t(0), [&in] (size_t& n) {
        return repeat([&in, &n] {
            return in.read().then([&n] (temporary_buffer<char> buf) {
                if (buf.empty()) {
                    return stop_iteration::yes;
                }
                n += buf.size();
                return stop_iteration::no;
            });
        }).then([&n] {
            return n;
        });
    });
}
// end snippet: count-bytes

// snippet: report
// Tells the client how many bytes it sent.
future<> report(connected_socket s) {
    return do_with(s.input(), s.output(),
            [] (input_stream<char>& in, output_stream<char>& out) {
        return count_bytes(in).then([&out] (size_t n) {
            return out.write(fmt::format("{} bytes\n", n));
        }).then([&out] {
            return out.close();
        });
    });
}
// end snippet: report

logger applog("count-bytes");

// snippet: serve
future<> serve(uint16_t port) {
    listen_options opts;
    opts.reuse_address = true;
    return do_with(listen(make_ipv4_address({port}), opts),
            [] (server_socket& listener) {
        return keep_doing([&listener] {
            return listener.accept().then([] (accept_result accepted) {
                (void)report(std::move(accepted.connection)).handle_exception(
                    [] (std::exception_ptr e) {
                        applog.warn("connection failed: {}", formattable(e));
                    });
            });
        });
    });
}
// end snippet: serve

int main(int argc, char** argv) {
    app_template::config cfg;
    cfg.auto_handle_sigint_sigterm = false;
    app_template app(std::move(cfg));
    return app.run(argc, argv, [] {
        return serve(1234);
    });
}

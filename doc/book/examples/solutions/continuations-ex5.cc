// Chapter 23, exercise 5: count-bytes-then.cc, with a count_bytes() that
// uses repeat_until_value(), and needs no do_with().
#include <seastar/core/app-template.hh>
#include <seastar/core/do_with.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/net/api.hh>
#include <seastar/util/log.hh>
#include <fmt/format.h>
#include <optional>

using namespace seastar;

// snippet: count-bytes
// Reads until the client closes its side, and returns how many bytes it
// sent. The count can't be a capture of the loop's function, referred to
// by the continuation: repeat_until_value() moves its function into a
// state of its own when it first has to wait, and the reference would be
// left pointing at the old copy. So the count is in an object of its own,
// which the function and the continuation share.
future<size_t> count_bytes(input_stream<char>& in) {
    return repeat_until_value([&in, n = make_lw_shared<size_t>(0)] {
        return in.read().then([n] (temporary_buffer<char> buf) {
            if (buf.empty()) {
                return std::optional<size_t>(*n);
            }
            *n += buf.size();
            return std::optional<size_t>();
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
                auto reported = report(std::move(accepted.connection));
                (void)reported.handle_exception([] (std::exception_ptr e) {
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

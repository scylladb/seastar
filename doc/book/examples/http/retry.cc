// Seastar's HTTP client, with a retry strategy of its own: a server whose
// route answers 503 Service Unavailable to its first two calls, and a
// client that retries such answers, waiting longer each time. Run it with
// one shard: -c1.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/http/client.hh>
#include <seastar/http/exception.hh>
#include <seastar/http/function_handlers.hh>
#include <seastar/http/httpd.hh>
#include <seastar/http/request.hh>
#include <seastar/http/retry_strategy.hh>
#include <seastar/util/short_streams.hh>
#include <chrono>
#include <exception>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

constexpr uint16_t port = 10080;

// snippet: flaky
// A route that is busy the first two times it's called.
void set_routes(httpd::routes& r) {
    r.add(httpd::operation_type::GET, httpd::url("/flaky"),
          new httpd::function_handler([calls = 0] (httpd::const_req) mutable {
        if (++calls <= 2) {
            throw httpd::base_exception(
                "busy", http::reply::status_type::service_unavailable);
        }
        return sstring("ready");
    }, "txt"));
}
// end snippet: flaky

// snippet: strategy
// Retries a reply of 503 Service Unavailable, up to four times, waiting
// 10 ms before the first retry, and twice as long before each of the
// others. Any other error isn't retried.
class retry_when_busy : public http::retry_strategy {
public:
    future<bool> should_retry(std::exception_ptr error,
                              unsigned attempted) const override {
        bool busy = false;
        try {
            std::rethrow_exception(error);
        } catch (httpd::unexpected_status_error& e) {
            busy = e.status() ==
                   http::reply::status_type::service_unavailable;
        } catch (...) {
        }
        if (!busy || attempted >= 4) {
            co_return false;
        }
        std::println("busy; retrying after {} ms", 10 << attempted);
        co_await sleep(std::chrono::milliseconds(10 << attempted));
        co_return true;
    }
};
// end snippet: strategy

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        httpd::http_server_control server;
        co_await server.start("flaky");
        co_await server.set_routes(set_routes);
        co_await server.listen(socket_address(net::inet_address("127.0.0.1"),
                                              port));
        // snippet: client
        http::client client(socket_address(net::inet_address("127.0.0.1"),
                                           port));
        auto req = http::request::make("GET", "localhost", "/flaky");
        retry_when_busy strategy;
        // The handler gets the reply, once its status is 200 OK.
        co_await client.make_request(std::move(req),
                [] (const http::reply& reply,
                    input_stream<char>&& body) -> future<> {
            // Copied before the first co_await: the handler's parameters
            // are references to the client's objects.
            auto status = int(reply._status);
            auto in = std::move(body);
            auto text = co_await util::read_entire_stream_contiguous(in);
            std::println("{}: {}", status, text);
        }, strategy, http::reply::status_type::ok);
        co_await client.close();
        // end snippet: client
        co_await server.stop();
    });
}

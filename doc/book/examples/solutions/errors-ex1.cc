// Chapter 11, exercise 1: kinds.cc, with broken() replaced by its body,
// inline.
#include <seastar/core/abort_source.hh>
#include <seastar/core/app-template.hh>
#include <seastar/core/condition-variable.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/as_future.hh>
#include <seastar/util/log.hh>
#include <fmt/format.h>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <string_view>

using namespace seastar;
using namespace std::chrono_literals;

// Tells what kind of failure an exception is, by its base class.
std::string_view kind(std::exception_ptr e) {
    try {
        std::rethrow_exception(e);
    } catch (timed_out_error&) {
        return "a timeout";
    } catch (abort_requested_exception&) {
        return "an abort";
    } catch (...) {
        return "something else";
    }
}

// Waits for f, and if it failed, says what kind of failure it was.
future<> report(std::string_view what, future<> f) {
    auto result = co_await coroutine::as_future(std::move(f));
    if (result.failed()) {
        auto e = result.get_exception();
        fmt::print("{}: {}: {}\n", what, kind(e), formattable(e));
    }
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        // snippet: main
        semaphore sem(0);
        co_await report("semaphore wait",
                        get_units(sem, 1, 1ms).discard_result());
        condition_variable cv;
        co_await report("condition wait", cv.wait(1ms));
        abort_source as;
        as.request_abort();
        co_await report("sleep", sleep_abortable(1s, as));
        co_await report("semaphore wait",
                        get_units(sem, 1, as).discard_result());
        gate g;
        co_await g.close();
        co_await report("gate", futurize_invoke([&g] {
            return with_gate(g, [] {});
        }));
        co_await report("promise", promise<>().get_future());
        // end snippet: main
        // snippet: nested
        auto inner = std::make_exception_ptr(
            std::runtime_error("disk error"));
        auto outer = std::make_exception_ptr(
            std::runtime_error("network error"));
        fmt::print("{}\n", formattable(
            std::make_exception_ptr(nested_exception(inner, outer))));
        // end snippet: nested
    });
}

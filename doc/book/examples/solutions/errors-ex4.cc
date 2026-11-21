// Chapter 11, exercise 4: internal.cc, which asks on_internal_error() to
// abort the program, instead of throwing.
#include <seastar/core/app-template.hh>
#include <seastar/core/on_internal_error.hh>
#include <seastar/util/log.hh>
#include <fmt/format.h>
#include <cstdint>
#include <exception>
#include <print>

using namespace seastar;

static logger applog("app");

// Takes n from the balance. Every caller checks first that the balance is
// large enough, so a negative balance here is a bug.
void withdraw(int64_t& balance, int64_t n) {
    balance -= n;
    if (balance < 0) {
        on_internal_error(applog, fmt::format(
            "negative balance {} after withdrawing {}", balance, n));
    }
}

// Like assert(), but checked in every build mode, and reported as an
// internal error: logged with a backtrace, failing only the operation that
// found the bug.
#define CHECK(x)                                                      \
    do {                                                              \
        if (!(x)) [[unlikely]] {                                      \
            on_internal_error(applog, "check failed: " #x);           \
        }                                                             \
    } while (0)

void deposit(int64_t& balance, int64_t n) {
    CHECK(n > 0);
    balance += n;
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        // snippet: main
        int64_t balance = 100;
        set_abort_on_internal_error(true);
        try {
            withdraw(balance, 150);    // a caller that forgot to check
        } catch (std::runtime_error&) {
            std::println("the withdrawal failed, and the program goes on");
        }
        // end snippet: main
        try {
            deposit(balance, -5);
        } catch (std::runtime_error&) {
            std::println("the deposit failed");
        }
        return make_ready_future<>();
    });
}

// An internal error: a check of something that can only be false if the
// program has a bug.
#include <seastar/core/app-template.hh>
#include <seastar/core/on_internal_error.hh>
#include <seastar/util/log.hh>
#include <fmt/format.h>
#include <cstdint>
#include <exception>
#include <print>

using namespace seastar;

static logger applog("app");

// snippet: withdraw
// Takes n from the balance. Every caller checks first that the balance is
// large enough, so a negative balance here is a bug.
void withdraw(int64_t& balance, int64_t n) {
    balance -= n;
    if (balance < 0) {
        on_internal_error(applog, fmt::format(
            "negative balance {} after withdrawing {}", balance, n));
    }
}
// end snippet: withdraw

// snippet: check
// Like assert(), but checked in every build mode, and reported as an
// internal error: logged with a backtrace, failing only the operation that
// found the bug.
#define CHECK(x)                                                      \
    do {                                                              \
        if (!(x)) [[unlikely]] {                                      \
            on_internal_error(applog, "check failed: " #x);           \
        }                                                             \
    } while (0)
// end snippet: check

// snippet: deposit
void deposit(int64_t& balance, int64_t n) {
    CHECK(n > 0);
    balance += n;
}
// end snippet: deposit

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        // snippet: main
        int64_t balance = 100;
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

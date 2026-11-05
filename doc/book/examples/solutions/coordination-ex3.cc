// Chapter 10, exercise 3: withdraw.cc, with withdraw_safely() holding a
// std::mutex, instead of a semaphore's unit, across its co_await.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/all.hh>
#include <chrono>
#include <mutex>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

int balance = 100;

// Writes a record of the withdrawal to a log, which takes a moment.
future<> log_withdrawal(int amount) {
    co_await sleep(1ms);
}

// Withdraws an amount, if the balance allows it, and returns whether it did.
future<bool> withdraw(int amount) {
    if (balance < amount) {
        co_return false;
    }
    co_await log_withdrawal(amount);
    balance -= amount;
    co_return true;
}

// A lock of threads: only one withdrawal at a time may check and update
// the balance.
std::mutex account_lock;

future<bool> withdraw_safely(int amount) {
    std::lock_guard held(account_lock);
    if (balance < amount) {
        co_return false;
    }
    co_await log_withdrawal(amount);
    balance -= amount;
    co_return true;
}

future<> twice(future<bool> (*how)(int), const char* name) {
    balance = 100;
    auto [first, second] = co_await coroutine::all(
        [how] { return how(80); }, [how] { return how(80); });
    std::println("{}: {} of 2 withdrawals done, balance {}", name,
                 int(first) + int(second), balance);
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        co_await twice(withdraw, "withdraw");
        co_await twice(withdraw_safely, "withdraw_safely");
    });
}

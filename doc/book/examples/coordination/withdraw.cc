// Two withdrawals from the same account, at the same time. Each checks the
// balance, waits for something, and then takes the money: without
// coordination, both may pass the check.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/sleep.hh>
#include <seastar/coroutine/all.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

int balance = 100;

// Writes a record of the withdrawal to a log, which takes a moment.
future<> log_withdrawal(int amount) {
    co_await sleep(1ms);
}

// snippet: withdraw
// Withdraws an amount, if the balance allows it, and returns whether it did.
future<bool> withdraw(int amount) {
    if (balance < amount) {
        co_return false;
    }
    co_await log_withdrawal(amount);
    balance -= amount;
    co_return true;
}
// end snippet: withdraw

// snippet: withdraw-safely
// One unit: only one withdrawal at a time may check and update the balance.
semaphore account_lock(1);

future<bool> withdraw_safely(int amount) {
    auto held = co_await get_units(account_lock, 1);
    if (balance < amount) {
        co_return false;
    }
    co_await log_withdrawal(amount);
    balance -= amount;
    co_return true;
}
// end snippet: withdraw-safely

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

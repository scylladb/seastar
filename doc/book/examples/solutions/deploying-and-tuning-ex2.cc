// The solution of chapter 31's exercise 2: what the reactor says about
// itself, while a fiber works for a millisecond and sleeps for one, for
// two seconds.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;
using clk = std::chrono::steady_clock;

// snippet: stats
struct reactor_stats {
    std::chrono::nanoseconds uptime, busy, idle, awake, cpu, steal;
    uint64_t polls, tasks;

    static reactor_stats now() {
        auto& r = engine();
        return {r.uptime(), r.total_busy_time(), r.total_idle_time(),
                r.total_awake_time(), r.total_cpu_time(), r.total_steal_time(),
                r.polls(), r.get_sched_stats().tasks_processed};
    }
};
// end snippet: stats

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        auto before = reactor_stats::now();
        auto end = clk::now() + 2s;
        while (clk::now() < end) {
            auto stop = clk::now() + 1ms;
            while (clk::now() < stop) {
            }
            co_await sleep(1ms);
        }
        auto after = reactor_stats::now();
        using ms = std::chrono::duration<double, std::milli>;
        auto print = [] (const char* name, std::chrono::nanoseconds d) {
            std::println("{:>8}: {:7.1f} ms", name, ms(d).count());
        };
        print("uptime", after.uptime - before.uptime);
        print("busy", after.busy - before.busy);
        print("idle", after.idle - before.idle);
        print("awake", after.awake - before.awake);
        print("cpu", after.cpu - before.cpu);
        print("steal", after.steal - before.steal);
        std::println("{:>8}: {}", "polls", after.polls - before.polls);
        std::println("{:>8}: {}", "tasks", after.tasks - before.tasks);
    });
}

/**
 * DAIL in Alibaba Group
 *
 */

#include "micro_system.hh"
#include "my_thread_pool.hh"

namespace bpo = boost::program_options;

seastar::future<> micro_system::configure() {
    my_thread_pool::configure();
    return seastar::smp::invoke_on_all([] {
        allocate_micro_reactor();
        micro_engine().register_pollers();
    });
}

int micro_system::run(int ac, char **av, std::function<void()> &&func) {
    return _app.run_deprecated(ac, av, [fn = std::move(func)] () mutable {
        return configure().then(
            std::move(fn)
        ).then_wrapped([] (auto&& f) {
            try {
                f.get();
            } catch (...) {
                std::cerr << "Micro system failed with exception: "
                          << std::current_exception() << std::endl;
                micro_engine().exit();
            }
        });
    });
}


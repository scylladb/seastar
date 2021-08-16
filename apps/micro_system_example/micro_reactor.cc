/**
 * DAIL in Alibaba Group
 *
 */

#include "micro_reactor.hh"
#include "my_thread_pool.hh"

void micro_reactor::register_pollers() {
    _my_thread_pool_poller = seastar::reactor::poller(std::make_unique<my_thread_pool_pollfn>());
}

void micro_reactor::deregister_pollers() {
    _my_thread_pool_poller = {};
}

void micro_reactor::clear_micro_system() {
    micro_engine().deregister_pollers();
    (void)seastar::do_with(seastar::semaphore(0), [] (seastar::semaphore& sem) {
        (void)seastar::smp::invoke_on_others(seastar::this_shard_id(), [] {
            micro_engine().deregister_pollers();
        }).then([&sem] {
            sem.signal();
        });
        return sem.wait(seastar::smp::count - 1);
    });
    my_thread_pool::stop();
}

void micro_reactor::exit() {
    clear_micro_system();
    seastar::engine().exit(0);
}

struct mirco_engine_deleter {
    void operator()(micro_reactor* mr) {
        mr->~micro_reactor();
        free(mr);
    }
};

__thread micro_reactor* local_micro_engine{nullptr};
thread_local std::unique_ptr<class micro_reactor, mirco_engine_deleter> mirco_reactor_holder;

void allocate_micro_reactor() {
    assert(!mirco_reactor_holder);

    void *buf;
    int r = posix_memalign(&buf, seastar::cache_line_size, sizeof(micro_reactor));
    assert(r == 0);
    local_micro_engine = reinterpret_cast<micro_reactor*>(buf);
    new (buf) micro_reactor();
    mirco_reactor_holder.reset(local_micro_engine);
}

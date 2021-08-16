/**
 * DAIL in Alibaba Group
 *
 */

#pragma once

#include <seastar/core/reactor.hh>

class micro_system;

class micro_reactor {
    boost::optional<seastar::reactor::poller> _my_thread_pool_poller = {};
public:
    micro_reactor() = default;
    ~micro_reactor() = default;

    void exit();
public:
    void register_pollers();
    void deregister_pollers();
    void clear_micro_system();

    friend class micro_system;
};

extern __thread micro_reactor *local_micro_engine;

void allocate_micro_reactor();

inline micro_reactor& micro_engine() {
    return *local_micro_engine;
}
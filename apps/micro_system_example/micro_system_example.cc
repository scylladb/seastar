//
// Created by everlighting on 2020/8/19.
//

#include <seastar/core/print.hh>
#include <seastar/core/reactor.hh>
#include <seastar/core/sleep.hh>

#include "micro_system.hh"
#include "my_thread_pool.hh"

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    micro_system sys;
    sys.run(argc, argv, [] {
        return seastar::parallel_for_each(boost::irange<unsigned>(0u, seastar::smp::count), [] (unsigned id) {
            return seastar::smp::submit_to(id, [id] {
                return my_thread_pool::submit_work<int>([id] {
                    fmt::print("I'm work in my_thread! from micro_reactor {} \n", id);
                    return id * id;
                }).then_wrapped([id] (seastar::future<int> fut) {
                    try{
                        fmt::print("Micro_reactor {} got the result {} from my_thread.\n", id, fut.get0());
                    } catch (std::exception& ex) {
                        fmt::print("Exception: {}\n", ex.what());
                    }
                });
            });
        }).then([] {
            fmt::print("start to sleep.\n");
            return seastar::sleep(1s);
        }).then([] {
            fmt::print("exit.\n");
            micro_engine().exit();
        });
    });
}
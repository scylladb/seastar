// Sending a shared object to another shard: shard 0 keeps a dictionary in
// an lw_shared_ptr, which its fibers share, and asks shard 1 to count the
// long words in it. The lambda captures a foreign_ptr, not the
// lw_shared_ptr, so shard 1 can't copy the pointer by accident.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sharded.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/sstring.hh>
#include <algorithm>
#include <print>
#include <stdexcept>
#include <vector>

using namespace seastar;

struct dictionary {
    std::vector<sstring> words;
    ~dictionary() {
        std::println("the dictionary is destroyed on shard {}",
                     this_shard_id());
    }
};

// Takes the dictionary by reference: no pointer is copied.
size_t count_long(const dictionary& d) {
    return std::ranges::count_if(d.words, [] (const sstring& w) {
        return w.size() > 5;
    });
}

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        if (this_smp_shard_count() < 2) {
            throw std::runtime_error("run with at least two shards, -c2");
        }
        auto dict = make_lw_shared<const dictionary>(
                std::vector<sstring>{"shard", "reactor", "future", "fiber"});
        // snippet: send
        auto n = co_await smp::submit_to(1, [d = make_foreign(dict)] {
            // Shard 1 reads the dictionary through the pointer, and passes
            // it on by reference. Copying d wouldn't compile.
            return count_long(*d);
        });
        // end snippet: send
        std::println("shard 1 counted {} long words in shard 0's dictionary",
                     n);
    });
}

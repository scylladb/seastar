// An object, shared with lw_shared_ptr, that must be closed before it's
// destroyed, and closing it waits. lw_shared_ptr_deleter lets the last
// pointer start closing it in the background, instead of deleting it.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sleep.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: deleter
class segment;

namespace seastar {
template <>
struct lw_shared_ptr_deleter<segment> {
    static void dispose(segment* s);
};
}
// end snippet: deleter

// snippet: segment
class segment : public enable_lw_shared_from_this<segment> {
    int _id;
    bool _closing = false;
    gate& _closing_segments;
public:
    segment(int id, gate& g) : _id(id), _closing_segments(g) {}
    ~segment() { std::println("segment {}: deleted", _id); }

    // Called by dispose(), when the last pointer goes away.
    void unused() {
        if (_closing) {
            delete this;    // the second time: it's closed
            return;
        }
        _closing = true;    // the first time: close it, in the background
        std::println("segment {}: unused, closing", _id);
        auto self = shared_from_this();     // the count is 1 again
        (void)close().finally([self = std::move(self),
                               h = _closing_segments.hold()] {});
    }

private:
    future<> close() {
        co_await sleep(10ms);   // as if it closed files
        std::println("segment {}: closed", _id);
    }
};

void lw_shared_ptr_deleter<segment>::dispose(segment* s) {
    s->unused();
}
// end snippet: segment

#include <seastar/core/shared_ptr_incomplete.hh>

// snippet: main
int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        gate closing_segments;
        {
            auto s = make_lw_shared<segment>(1, closing_segments);
            auto copy = s;
            std::println("two pointers to segment 1");
        }
        std::println("no pointers left; shutting down");
        co_await closing_segments.close();  // waits for segments closing
        std::println("done");
    });
}
// end snippet: main

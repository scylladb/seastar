// Sharing ownership with lw_shared_ptr, and watching an object with weak_ptr:
// a session keeps itself alive while its background loop runs.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sleep.hh>
#include <seastar/core/sstring.hh>
#include <seastar/core/weak_ptr.hh>
#include <chrono>
#include <print>

using namespace seastar;
using namespace std::chrono_literals;

// snippet: session
class session : public enable_lw_shared_from_this<session>,
                public weakly_referencable<session> {
    sstring _name;
    bool _closed = false;
public:
    explicit session(sstring name) : _name(std::move(name)) {}
    ~session() { std::println("{}: destroyed", _name); }
    void close() { _closed = true; }

    // Runs in the background, until the session is closed.
    future<> keepalive() {
        auto self = shared_from_this();   // the session lives while this runs
        while (!_closed) {
            std::println("{}: still here", _name);
            co_await sleep(10ms);
        }
        std::println("{}: keepalive done", _name);
    }
};
// end snippet: session

// snippet: use
future<> use_session() {
    auto s = make_lw_shared<session>("s1");
    weak_ptr<session> watch = s->weak_from_this();
    (void)s->keepalive();               // runs in the background
    co_await sleep(25ms);
    s->close();
    s = nullptr;                        // the owner lets go
    std::println("owner let go; the session {}",
                 watch ? "still exists" : "is gone");
    co_await sleep(20ms);
    std::println("later, the session {}",
                 watch ? "still exists" : "is gone");
}
// end snippet: use

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        return use_session();
    });
}

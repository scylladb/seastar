// The first Seastar program: it prints a greeting and exits.
#include <seastar/core/app-template.hh>
#include <seastar/core/coroutine.hh>
#include <print>

using namespace seastar;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] () -> future<> {
        std::println("Hello, world");
        co_return;
    });
}

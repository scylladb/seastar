// Chapter 12, exercise 3: subscribing to an abort source after the abort
// was requested.
#include <seastar/core/abort_source.hh>
#include <seastar/core/app-template.hh>
#include <print>

using namespace seastar;

int main(int argc, char** argv) {
    app_template app;
    return app.run(argc, argv, [] {
        abort_source as;
        as.request_abort();
        bool called = false;
        auto subscription = as.subscribe([&called] () noexcept {
            called = true;
        });
        std::println("subscription engaged: {}, function called: {}",
                     bool(subscription), called);
        return make_ready_future<>();
    });
}

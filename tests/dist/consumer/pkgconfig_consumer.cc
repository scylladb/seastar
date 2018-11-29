#include <iostream>

#include <seastar/core/app-template.hh>
#include <seastar/core/future.hh>

namespace sr = seastar;

int main(int argc, char** argv) {
    sr::app_template app;

    return app.run(argc, argv, [] {
        std::cout << "\"Hello\" from the Seastar pkg-config consumer!\n";
        return sr::make_ready_future<>();
    });
}

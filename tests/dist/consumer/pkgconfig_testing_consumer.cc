#define SEASTAR_TESTING_MAIN

#include <iostream>

#include <seastar/core/future.hh>
#include <seastar/testing/test_case.hh>

namespace sr = seastar;

SEASTAR_TEST_CASE(greeting) {
    return sr::make_ready_future<>().then([] {
        BOOST_REQUIRE(true);
        std::cout << "\"Hello\" from the Seastar pkg-config testing consumer!\n";
    });
}

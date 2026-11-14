// The solution of chapter 28's exercise 3: a fuzz test of Shoal's
// key_table, which applies the same operations to a key_table and to a
// std::unordered_map, and checks that they always agree.
//
// With libFuzzer, which comes with Clang, compile it with Clang's options
// -fsanitize=fuzzer,address,undefined, with -DSHOAL_FUZZER, and with
// Seastar's include directories ("pkg-config --cflags seastar"), and run
// it for a minute, with inputs of up to 60,000 bytes from the start, with
// "./table-fuzz -max_total_time=60 -max_len=60000 -len_control=0 corpus".
// Without it, the program's own main() feeds the same function random
// inputs, which is a much weaker fuzzer, but checks that the test works.
#include "../../shoal/v16/table.hh"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef SHOAL_FUZZER
// sstring's two error paths, which are in the Seastar library. The fuzzer
// doesn't link it: Seastar's allocator, which the library brings, replaces
// malloc(), and can't be combined with the sanitizers that find the bugs.
namespace seastar::internal {
[[noreturn]] void throw_bad_alloc() { throw std::bad_alloc(); }
[[noreturn]] void throw_sstring_overflow() {
    throw std::overflow_error("sstring overflow");
}
}
#endif

// snippet: fuzz
// Every three bytes are an operation: the first byte says which, and the
// next two which of 8,192 keys: enough to make the table grow and split
// its small tables, and few enough that operations meet, inserting a key
// that's there, or erasing one that isn't. Every 1,024 operations, a scan
// of the whole table must find exactly the map's keys.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    key_table<int> table;
    std::unordered_map<std::string, int> map;
    for (size_t i = 0; i + 2 < size; i += 3) {
        auto n = (data[i + 1] << 8 | data[i + 2]) % 8192;
        auto key = "key" + std::to_string(n);
        switch (data[i] % 3) {
        case 0: {       // insert, or update
            auto [it, inserted] = table.try_emplace(key);
            if (inserted != !map.contains(key)) {
                std::abort();
            }
            it->second = data[i];
            map[key] = data[i];
            break;
        }
        case 1: {       // look up
            auto it = table.find(key);
            auto m = map.find(key);
            if ((it == table.end()) != (m == map.end()) ||
                    (m != map.end() && it->second != m->second)) {
                std::abort();
            }
            break;
        }
        case 2: {       // erase
            auto it = table.find(key);
            if (it != table.end()) {
                table.erase(it);
            }
            map.erase(key);
            break;
        }
        }
        if (table.size() != map.size()) {
            std::abort();
        }
        if (i % (3 * 1024) == 0) {
            size_t found = 0, position = 0;
            do {
                position = table.scan(position, [&] (auto& element) {
                    auto m = map.find(std::string(element.first));
                    if (m == map.end() || m->second != element.second) {
                        std::abort();
                    }
                    found++;
                });
            } while (position != 0);
            if (found != map.size()) {
                std::abort();
            }
        }
    }
    return 0;
}
// end snippet: fuzz

#ifndef SHOAL_FUZZER
int main() {
    std::mt19937 random(1);
    std::uniform_int_distribution<int> byte(0, 255), length(0, 60000);
    for (int run = 0; run < 200; run++) {
        std::vector<uint8_t> input(length(random));
        for (auto& b : input) {
            b = byte(random);
        }
        LLVMFuzzerTestOneInput(input.data(), input.size());
    }
    std::printf("200 random inputs: no differences\n");
}
#endif

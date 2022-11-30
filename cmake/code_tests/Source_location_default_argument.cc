#include<source_location>

int test_source_location(int line,
                         std::source_location loc = std::source_location::current()) {
    return line == loc.line() ? 0 : 1;
}

int main() {
    return test_source_location(__LINE__);
}

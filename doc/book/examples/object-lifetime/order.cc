// The order in which a function's arguments are evaluated is unspecified,
// so moving from an object and using it in the same call is a bug.
#include <print>
#include <string>

// snippet: order
void consume(std::string s, size_t n) {
    std::println("got {} bytes, told {}", s.size(), n);
}

int main() {
    std::string s(100, 'x');
    consume(std::move(s), s.size());   // s.size() may be 100, or 0
}
// end snippet: order

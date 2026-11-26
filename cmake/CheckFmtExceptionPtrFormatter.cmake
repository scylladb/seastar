include (CheckCXXSourceCompiles)
include (CMakePushCheckState)

cmake_push_check_state (RESET)

set (CMAKE_REQUIRED_LIBRARIES fmt::fmt)

# check if {fmt} has its own formatter for std::exception_ptr
check_cxx_source_compiles ("
#include <exception>
#include <string>
#include <fmt/format.h>
#include <fmt/std.h>

int main() {
    std::string s = fmt::format(\"{}\", std::exception_ptr());
    return s.empty();
}
"
  Fmt_HAS_EXCEPTION_PTR_FORMATTER)

cmake_pop_check_state ()

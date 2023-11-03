include (CheckCXXSourceCompiles)
include (CMakePushCheckState)

cmake_push_check_state (RESET)

# these options are included by -Wall, which is in turn included by
# Seastar_PRIVATE_CXX_FLAGS, which is not applied to CMAKE_CXX_FLAGS, so
# let's apply them explicitly.
set (CMAKE_REQUIRED_FLAGS "-Werror=stringop-overflow -Werror=array-bound")
set (CMAKE_REQUIRED_LIBRARIES fmt::fmt)

# see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=107852
check_cxx_source_compiles ("
#include <fmt/ranges.h>

int main() {
    float grades[] = {3.14};
    fmt::print(\"{}\", grades);
}"
  Cxx_Compiler_BZ107852_Free
  FAIL_REGEX "is out of the bounds")

cmake_pop_check_state ()


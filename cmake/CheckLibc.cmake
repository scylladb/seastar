# check for the bits in different standard C library implementations we
# care about

include (CheckCXXSourceCompiles)
file (READ ${CMAKE_CURRENT_LIST_DIR}/code_tests/stdout_test.cc _stdout_test_code)
check_cxx_source_compiles ("${_stdout_test_code}" Stdout_Can_Be_Used_As_Identifier)
if (Stdout_Can_Be_Used_As_Identifier)
  # "stdout" is defined as a macro by the C++ standard, so we cannot assume
  # that the macro is always expanded into an identifier which can be re-used
  # to name a enumerator in the declaration of an enumeration.
  target_compile_definitions (seastar-objects
    PUBLIC
      SEASTAR_LOGGER_TYPE_STDOUT)
endif ()

check_cxx_source_compiles ("
#include <string.h>

int main() {
    char buf;
    char* a = strerror_r(1, &buf, 0);
    static_cast<void>(a);
}"
  Strerror_R_Returns_Char_P)
if (Strerror_R_Returns_Char_P)
  # define SEASTAR_STRERROR_R_CHAR_P if strerror_r() is GNU-specific version,
  # which returns a "char*" not "int".
  target_compile_definitions (seastar-objects
    PRIVATE
      SEASTAR_STRERROR_R_CHAR_P)
endif ()

include (CheckFunctionExists)

check_function_exists (pthread_attr_setaffinity_np
  Pthread_Attr_Setaffinity_Np)
if (Pthread_Attr_Setaffinity_Np)
  target_compile_definitions (seastar-objects
    PRIVATE
    SEASTAR_PTHREAD_ATTR_SETAFFINITY_NP)
endif ()

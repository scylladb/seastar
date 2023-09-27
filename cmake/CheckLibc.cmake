# check for the bits in different standard C library implementations we
# care about

include (CheckCXXSourceCompiles)
file (READ ${CMAKE_CURRENT_LIST_DIR}/code_tests/stdout_test.cc _stdout_test_code)
check_cxx_source_compiles ("${_stdout_test_code}" Stdout_Can_Be_Used_As_Identifier)
if (Stdout_Can_Be_Used_As_Identifier)
  # "stdout" is defined as a macro by the C++ standard, so we cannot assume
  # that the macro is always expanded into an identifier which can be re-used
  # to name a enumerator in the declaration of an enumeration.
  target_compile_definitions (seastar
    PUBLIC
      SEASTAR_LOGGER_TYPE_STDOUT)
endif ()

add_library (seastar-perf-testing
  perf_tests.hh perf_tests.cc)

target_include_directories (seastar-perf-testing PUBLIC
  $<TARGET_PROPERTY:seastar,INCLUDE_DIRECTORIES>)

if (NOT ${SEASTAR_LINK_STATIC_BOOST})
  target_compile_definitions (seastar-perf-testing PUBLIC
    BOOST_TEST_DYN_LINK)
endif ()

# ADD_SEASTAR_PERF_TEST NAME $name SOURCES $source_1 $source_2 ... $source_n)
function (add_seastar_perf_test)
  set (options "")
  set (one_value_args NAME)
  set (multi_value_args SOURCES)
  cmake_parse_arguments (args "${options}" "${one_value_args}" "${multi_value_args}" "${ARGN}")

  add_executable (${args_NAME} "${args_SOURCES}")

  target_link_libraries (${args_NAME} PUBLIC
    -Wl,--whole-archive seastar seastar-perf-testing -Wl,--no-whole-archive
    # Strip symbols.
    -s)
endfunction ()

add_seastar_perf_test (NAME perf_future_util
  SOURCES perf_future_util.cc)

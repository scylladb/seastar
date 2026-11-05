# snippet: cmake
cmake_minimum_required(VERSION 3.18...3.30)
project(shoal LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "The build type" FORCE)
endif()
find_package(Seastar REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The admin API's code, generated from its definition by Seastar's
# seastar-json2code.py, which Seastar installs in its bin directory.
find_program(SEASTAR_JSON2CODE seastar-json2code.py
  HINTS "${Seastar_DIR}/../../../bin" REQUIRED)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/admin.json.hh
         ${CMAKE_CURRENT_BINARY_DIR}/admin.json.cc
  COMMAND ${Python3_EXECUTABLE} ${SEASTAR_JSON2CODE} --create-cc
          -f ${CMAKE_CURRENT_SOURCE_DIR}/api/admin.json
          -o ${CMAKE_CURRENT_BINARY_DIR}/admin.json.hh
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/api/admin.json)

add_executable(shoal main.cc resp.cc commands.cc pubsub.cc log.cc admin.cc
  feed.cc replication.cc
  ${CMAKE_CURRENT_BINARY_DIR}/admin.json.cc)
target_include_directories(shoal PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(shoal PRIVATE Seastar::seastar)
# end snippet: cmake

# snippet: unit-tests
# The unit tests (version 16): a program of their own, with the parts of
# Shoal that they test, and Seastar's testing library, which gives it its
# main(). "ctest" runs it, or it can be run directly.
add_executable(shoal_unit_tests unit_tests.cc resp.cc commands.cc log.cc
  feed.cc)
target_link_libraries(shoal_unit_tests PRIVATE Seastar::seastar_testing)
enable_testing()
add_test(NAME unit_tests COMMAND shoal_unit_tests -- -c2 -m1G)
# end snippet: unit-tests

# The micro-benchmarks, with Seastar's perf_tests library, which gives the
# program its main() too.
add_executable(shoal_perf_tests perf_tests.cc resp.cc commands.cc log.cc
  feed.cc)
target_link_libraries(shoal_perf_tests PRIVATE Seastar::seastar_perf_testing)

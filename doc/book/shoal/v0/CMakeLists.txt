# snippet: cmake
cmake_minimum_required(VERSION 3.13...3.30)
project(shoal LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "The build type" FORCE)
endif()
find_package(Seastar REQUIRED)

add_executable(shoal main.cc)
target_link_libraries(shoal PRIVATE Seastar::seastar)
# end snippet: cmake

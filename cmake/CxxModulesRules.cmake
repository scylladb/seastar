if (NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message (FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}")
endif ()

if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
  message (FATAL "C++20 module needs Clang++-16 or up")
endif ()

set (CMAKE_EXPERIMENTAL_CXX_MODULE_DYNDEP 1)
set (CMAKE_EXPERIMENTAL_CXX_SCANDEP_SOURCE "")
set (CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API  "2182bf5c-ef0d-489a-91da-49dbc3090d2a")

set (CMAKE_CXX_STANDARD_REQUIRED ON)
# C++ extension does work with C++ module support so far
set (CMAKE_CXX_EXTENSIONS OFF)

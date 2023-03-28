include(GNUInstallDirs)
include(ExternalProject)

set(config_flags "CPPFLAGS=-fPIC -g -O2")  # parameters desired for ./configure of Autotools

ExternalProject_Add(my_libbacktrace
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libbacktrace
    CONFIGURE_COMMAND <SOURCE_DIR>/configure ${config_flags}
    BUILD_COMMAND make -j
    BINARY_DIR libbacktrace
    INSTALL_COMMAND ""
    TEST_COMMAND ""
    BUILD_BYPRODUCTS <BINARY_DIR>/.libs/libbacktrace.a
)

ExternalProject_Get_property(my_libbacktrace BINARY_DIR)
add_library(libbacktrace INTERFACE IMPORTED GLOBAL)
add_dependencies(libbacktrace PRIVATE my_libbacktrace)
target_link_libraries(libbacktrace INTERFACE ${BINARY_DIR}/.libs/libbacktrace.a)

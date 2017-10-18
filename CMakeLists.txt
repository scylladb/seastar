cmake_minimum_required(VERSION 3.8)
set(CMAKE_CXX_STANDARD 14)
project(seastar)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -W -Wall -Wextra -O2 -fPIC")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR})

add_subdirectory(fmt)
set(CARES_STATIC ON CACHE BOOL "Build as a static library" FORCE)
add_subdirectory(c-ares)

include_directories(${PROJECT_SOURCE_DIR})
include_directories(${PROJECT_BINARY_DIR})
include_directories(${PROJECT_SOURCE_DIR}/fmt)

find_package(RAGEL 6.8 REQUIRED)

add_subdirectory(apps)

add_subdirectory(proto)
add_subdirectory(net)
add_subdirectory(util)
add_subdirectory(rpc)
add_subdirectory(json)
add_subdirectory(http)
add_subdirectory(core)



add_library(seastar $<TARGET_OBJECTS:net>
        $<TARGET_OBJECTS:util>
        $<TARGET_OBJECTS:rpc>
        $<TARGET_OBJECTS:json>
        $<TARGET_OBJECTS:http>
        $<TARGET_OBJECTS:core>
        $<TARGET_OBJECTS:metrics2_proto>)

target_link_libraries(seastar
        c-ares_static
        dl
        pthread
        aio
        boost_program_options
        boost_system
        boost_filesystem
        boost_unit_test_framework
        boost_thread
        cryptopp
        rt
        gnutls
        gnutlsxx
        xenstore
        hwloc
        numa
        pciaccess
        xml2
        z
        m
        fmt
        unwind
        ${PROTOBUF_LIBRARIES})

add_definitions(-D_REENTRANT
        -Wno-overloaded-virtual
        -DHAVE_XEN
        -DHAVE_HWLOC
        -DHAVE_NUMA)

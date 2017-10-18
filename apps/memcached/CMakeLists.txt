RAGEL_TARGET(ascii
        ascii.rl
        ${CMAKE_CURRENT_BINARY_DIR}/ascii.hh
        COMPILE_FLAGS
        '-G2')

add_executable(memcached
        ${RAGEL_ascii_OUTPUTS}
        memcache.cc
        memcached.hh)

target_link_libraries(memcached
        seastar)
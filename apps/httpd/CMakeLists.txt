add_executable(httpd
        main.cc)

target_link_libraries(httpd
        seastar)
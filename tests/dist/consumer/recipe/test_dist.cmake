cmake_host_system_information (
  RESULT build_concurrency_factor
  QUERY NUMBER_OF_LOGICAL_CORES)

set (make_command make -j ${build_concurrency_factor})

cooking_ingredient (Boost
  EXTERNAL_PROJECT_ARGS
    # The 1.67.0 release has a bug in Boost Lockfree around a missing header.
    URL https://dl.bintray.com/boostorg/release/1.64.0/source/boost_1_64_0.tar.gz
    URL_MD5 319c6ffbbeccc366f14bb68767a6db79
    PATCH_COMMAND
      ./bootstrap.sh
      --prefix=<INSTALL_DIR>
      --with-libraries=atomic,chrono,date_time,filesystem,program_options,system,test,thread
    CONFIGURE_COMMAND <DISABLE>
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND
      ${CMAKE_COMMAND} -E chdir <SOURCE_DIR>
      ./b2
      -j ${build_concurrency_factor}
      --layout=system
      --build-dir=<BINARY_DIR>
      install
      variant=debug
      link=shared
      threading=multi
      hardcode-dll-paths=true
      dll-path=<INSTALL_DIR>/lib)

cooking_ingredient (Seastar
  REQUIRES Boost
  COOKING_RECIPE dev
  COOKING_CMAKE_ARGS
    # Not `lib64`.
    -DCMAKE_INSTALL_LIBDIR=lib
    -DSeastar_APPS=OFF
    -DSeastar_DOCS=OFF
    -DSeastar_DEMOS=OFF
    -DSeastar_DPDK=ON
    -DSeastar_TESTING=OFF
  EXTERNAL_PROJECT_ARGS
    SOURCE_DIR $ENV{SEASTAR_SOURCE_DIR})

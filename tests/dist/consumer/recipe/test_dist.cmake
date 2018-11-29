cooking_ingredient (Seastar
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

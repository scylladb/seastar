find_package (PkgConfig REQUIRED)

pkg_search_module (Benchmark IMPORTED_TARGET GLOBAL benchmark)

if (Benchmark_FOUND)
  add_library (Benchmark::benchmark INTERFACE IMPORTED)
  target_link_libraries (Benchmark::benchmark INTERFACE PkgConfig::Benchmark)
  set (Benchmark_LIBRARY ${Benchmark_LIBRARIES})
  set (Benchmark_INCLUDE_DIR ${Benchmark_INCLUDE_DIRS})
endif ()

if (NOT Benchmark_LIBRARY)
  find_library (Benchmark_LIBRARY
    NAMES benchmark)
endif ()

if (NOT Benchmark_INCLUDE_DIR)
    find_path (Benchmark_INCLUDE_DIR
      NAMES benchmark/benchmark.h)
endif ()

mark_as_advanced (
  Benchmark_LIBRARY
  Benchmark_INCLUDE_DIR)

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (Benchmark
  REQUIRED_VARS
    Benchmark_LIBRARY
    Benchmark_INCLUDE_DIR)

if (Benchmark_FOUND)
  if (NOT (TARGET Benchmark::benchmark))
    add_library (Benchmark::benchmark UNKNOWN IMPORTED)

    set_target_properties (Benchmark::benchmark
      PROPERTIES
        IMPORTED_LOCATION ${Benchmark_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${Benchmark_INCLUDE_DIRS})
  endif ()
endif ()

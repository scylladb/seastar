add_library (seastar-module)
add_dependencies (seastar-module
    seastar_http_chunk_parsers
    seastar_http_request_parser
    seastar_http_response_parser)
target_sources (seastar-module
  PUBLIC
    FILE_SET CXX_MODULES
    TYPE CXX_MODULES
    FILES
      seastar.cc
  PRIVATE
    core/alien.cc
    core/app-template.cc
    core/condition-variable.cc
    core/exception_hacks.cc
    core/execution_stage.cc
    core/fair_queue.cc
    core/file.cc
    core/fsnotify.cc
    core/fstream.cc
    core/future-util.cc
    core/future.cc
    core/io_queue.cc
    core/linux-aio.cc
    core/memory.cc
    core/metrics.cc
    core/on_internal_error.cc
    core/posix.cc
    core/program_options.cc
    core/reactor.cc
    core/reactor_backend.cc
    core/resource.cc
    core/sharded.cc
    core/scollectd.cc
    core/semaphore.cc
    core/signal.cc
    core/smp.cc
    core/sstring.cc
    core/systemwide_memory_barrier.cc
    core/thread.cc
    core/thread_pool.cc
    core/uname.cc
    util/alloc_failure_injector.cc
    util/backtrace.cc
    util/conversions.cc
    util/file.cc
    util/log.cc
    util/process.cc
    util/program-options.cc
    util/read_first_line.cc
    util/short_streams.cc
    net/config.cc
    net/arp.cc
    net/dhcp.cc
    net/dpdk.cc
    net/ip.cc
    net/tcp.cc
    net/udp.cc
    net/stack.cc
    net/native-stack.cc
    net/posix-stack.cc
    net/net.cc
    net/proxy.cc
    net/ip_checksum.cc
    net/packet.cc
    net/inet_address.cc
    net/socket_address.cc
    net/tls.cc
    net/virtio.cc
    http/client.cc
    http/common.cc
    http/file_handler.cc
    http/httpd.cc
    http/json_path.cc
    http/matcher.cc
    http/mime_types.cc
    http/reply.cc
    http/request.cc
    http/routes.cc
    http/transformers.cc
    http/url.cc
    json/formatter.cc
    json/json_elements.cc
  )
target_include_directories (seastar-module
  PUBLIC
    $<INSTALL_INTERFACE:include>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${Seastar_GEN_BINARY_DIR}/include>
  PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_compile_definitions (seastar-module
  PUBLIC
    $<$<BOOL:${Seastar_SSTRING}>:SEASTAR_SSTRING>
    SEASTAR_API_LEVEL=${Seastar_API_LEVEL}
    SEASTAR_SCHEDULING_GROUPS_COUNT=${Seastar_SCHEDULING_GROUPS_COUNT}
  PRIVATE
    SEASTAR_MODULE
    ${Seastar_PRIVATE_COMPILE_DEFINITIONS})
target_compile_options (seastar-module
  PUBLIC
    -U_FORTIFY_SOURCE)
include (CheckCXXCompilerFlag)
check_cxx_compiler_flag ("-Winclude-angled-in-module-purview"
    Warning_IncludeAngledInModulePurview_FOUND)
if (Warning_IncludeAngledInModulePurview_FOUND)
  target_compile_options (seastar-module
    PRIVATE "-Wno-include-angled-in-module-purview")
endif ()

target_link_libraries (seastar-module
  PUBLIC
    Boost::boost
    Boost::program_options
    Boost::thread
    c-ares::cares
    fmt::fmt
    lz4::lz4
    SourceLocation::source_location
  PRIVATE
    ${CMAKE_DL_LIBS}
    GnuTLS::gnutls
    StdAtomic::atomic
    lksctp-tools::lksctp-tools
    rt::rt
    yaml-cpp::yaml-cpp
    "$<BUILD_INTERFACE:Valgrind::valgrind>"
    Threads::Threads)
if (Seastar_HWLOC)
  target_link_libraries (seastar-module
    PRIVATE hwloc::hwloc)
endif ()
if (Seastar_IO_URING)
  target_link_libraries (seastar-module
    PRIVATE URING::uring)
endif ()

install (
  TARGETS seastar-module
  LIBRARY
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  CXX_MODULES_BMI
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cxx/bmi"
    COMPONENT "bmi"
  FILE_SET CXX_MODULES
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cxx/modules"
    COMPONENT "modules")
export (
  TARGETS
    seastar-module
  NAMESPACE Seastar::
  APPEND FILE "${CMAKE_CURRENT_BINARY_DIR}/SeastarModules.cmake"
  CXX_MODULES_DIRECTORY "cxx-modules")

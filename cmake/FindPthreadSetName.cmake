include (CheckSymbolExists)
include (CMakePushCheckState)

cmake_push_check_state (RESET)
set (CMAKE_REQUIRED_FLAGS "-pthread")
set (CMAKE_REQUIRED_DEFINITIONS "-D_GNU_SOURCE")
check_symbol_exists (pthread_setname_np pthread.h HAVE_PTHREAD_SETNAME_NP)
cmake_pop_check_state ()

find_package_handle_standard_args (PthreadSetName
  FOUND_VAR PthreadSetName_FOUND
  REQUIRED_VARS
    HAVE_PTHREAD_SETNAME_NP)

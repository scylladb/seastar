set (ngtcp2_min_version 1.14.0)
if (ngtcp2_FIND_VERSION AND ngtcp2_FIND_VERSION VERSION_GREATER ngtcp2_min_version)
  set (ngtcp2_required_version ${ngtcp2_FIND_VERSION})
else ()
  set (ngtcp2_required_version ${ngtcp2_min_version})
endif ()

if (NOT DEFINED Seastar_NGTCP2_PROVIDER)
  set (Seastar_NGTCP2_PROVIDER AUTO)
endif ()

if (NOT Seastar_NGTCP2_PROVIDER MATCHES "^(AUTO|SYSTEM|BUNDLED)$")
  message (FATAL_ERROR
    "Invalid Seastar_NGTCP2_PROVIDER='${Seastar_NGTCP2_PROVIDER}'. "
    "Expected AUTO, SYSTEM, or BUNDLED.")
endif ()

set (ngtcp2_system_found FALSE)
if (NOT Seastar_NGTCP2_PROVIDER STREQUAL "BUNDLED")
  find_package (PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules (PC_ngtcp2 QUIET libngtcp2)
    pkg_check_modules (PC_ngtcp2_crypto_gnutls QUIET libngtcp2_crypto_gnutls)
  endif ()

  find_library (ngtcp2_LIBRARY
    NAMES ngtcp2
    HINTS
      ${PC_ngtcp2_LIBDIR}
      ${PC_ngtcp2_LIBRARY_DIRS})

  find_library (ngtcp2_crypto_gnutls_LIBRARY
    NAMES ngtcp2_crypto_gnutls
    HINTS
      ${PC_ngtcp2_crypto_gnutls_LIBDIR}
      ${PC_ngtcp2_crypto_gnutls_LIBRARY_DIRS})

  find_path (ngtcp2_INCLUDE_DIR
    NAMES ngtcp2/ngtcp2.h
    HINTS
      ${PC_ngtcp2_INCLUDEDIR}
      ${PC_ngtcp2_INCLUDE_DIRS})

  find_path (ngtcp2_crypto_gnutls_INCLUDE_DIR
    NAMES ngtcp2/ngtcp2_crypto_gnutls.h
    HINTS
      ${PC_ngtcp2_crypto_gnutls_INCLUDEDIR}
      ${PC_ngtcp2_crypto_gnutls_INCLUDE_DIRS})

  mark_as_advanced (
    ngtcp2_LIBRARY
    ngtcp2_crypto_gnutls_LIBRARY
    ngtcp2_INCLUDE_DIR
    ngtcp2_crypto_gnutls_INCLUDE_DIR)

  set (ngtcp2_VERSION ${PC_ngtcp2_VERSION})

  if (PC_ngtcp2_FOUND
      AND PC_ngtcp2_crypto_gnutls_FOUND
      AND ngtcp2_LIBRARY
      AND ngtcp2_crypto_gnutls_LIBRARY
      AND ngtcp2_INCLUDE_DIR
      AND ngtcp2_crypto_gnutls_INCLUDE_DIR
      AND NOT PC_ngtcp2_VERSION VERSION_LESS ngtcp2_required_version
      AND NOT PC_ngtcp2_crypto_gnutls_VERSION VERSION_LESS ngtcp2_required_version)
    set (ngtcp2_system_found TRUE)
  endif ()
endif ()

set (ngtcp2_use_bundled FALSE)
if (ngtcp2_system_found)
  set (ngtcp2_LIBRARIES
    ${ngtcp2_crypto_gnutls_LIBRARY}
    ${ngtcp2_LIBRARY})
  set (ngtcp2_INCLUDE_DIRS
    ${ngtcp2_INCLUDE_DIR}
    ${ngtcp2_crypto_gnutls_INCLUDE_DIR})
  list (REMOVE_DUPLICATES ngtcp2_INCLUDE_DIRS)

  if (NOT TARGET ngtcp2::ngtcp2_core)
    add_library (ngtcp2::ngtcp2_core UNKNOWN IMPORTED)

    set_target_properties (ngtcp2::ngtcp2_core
      PROPERTIES
        IMPORTED_LOCATION ${ngtcp2_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${ngtcp2_INCLUDE_DIR})
  endif ()

  if (NOT TARGET ngtcp2::ngtcp2_crypto_gnutls)
    add_library (ngtcp2::ngtcp2_crypto_gnutls UNKNOWN IMPORTED)

    set_target_properties (ngtcp2::ngtcp2_crypto_gnutls
      PROPERTIES
        IMPORTED_LOCATION ${ngtcp2_crypto_gnutls_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${ngtcp2_crypto_gnutls_INCLUDE_DIR})
  endif ()

  if (NOT TARGET ngtcp2::ngtcp2)
    add_library (ngtcp2::ngtcp2 INTERFACE IMPORTED)

    set_target_properties (ngtcp2::ngtcp2
      PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ngtcp2_INCLUDE_DIRS}")

    target_link_libraries (ngtcp2::ngtcp2
      INTERFACE
        ngtcp2::ngtcp2_crypto_gnutls
        ngtcp2::ngtcp2_core)
  endif ()

  set (ngtcp2_FOUND TRUE)
elseif (NOT Seastar_NGTCP2_PROVIDER STREQUAL "SYSTEM"
    AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ngtcp2/CMakeLists.txt")
  set (ngtcp2_use_bundled TRUE)
else ()
  set (ngtcp2_FOUND FALSE)
endif ()

if (NOT ngtcp2_use_bundled)
  include (FindPackageHandleStandardArgs)

  find_package_handle_standard_args (ngtcp2
    REQUIRED_VARS
      ngtcp2_FOUND
    VERSION_VAR
      ngtcp2_VERSION)
  return ()
endif ()

set (NGTCP2_ENABLE_LIB_ONLY
  ON
  CACHE
  BOOL
  "Build libngtcp2 only")

set (NGTCP2_ENABLE_STATIC_LIB
  ON
  CACHE
  BOOL
  "Build static lib")

set (NGTCP2_ENABLE_SHARED_LIB
  OFF
  CACHE
  BOOL
  "Disable shared lib")

set (NGTCP2_ENABLE_GNUTLS
  ON
  CACHE
  BOOL
  "Enable GnuTLS")

set (NGTCP2_ENABLE_OPENSSL
  OFF
  CACHE
  BOOL
  "Disable OpenSSL")

set (NGTCP2_DISABLE_TESTS
  ON
  CACHE
  BOOL
  "Disable tests")

enable_language (C)

set (Cooking_USE_CMAKE_PROJECT_COMMAND ON)
add_subdirectory (ngtcp2)
unset (Cooking_USE_CMAKE_PROJECT_COMMAND)

set (NGTCP2_SRC "${CMAKE_CURRENT_SOURCE_DIR}/ngtcp2")
set (NGTCP2_BIN "${CMAKE_CURRENT_BINARY_DIR}/ngtcp2")

set (ngtcp2_INCLUDE_DIRS
  "${NGTCP2_SRC}/lib/includes"
  "${NGTCP2_SRC}/crypto/includes"
  "${NGTCP2_SRC}/lib"
  "${NGTCP2_BIN}/lib/includes"
)

if (TARGET ngtcp2_static AND TARGET ngtcp2_crypto_gnutls_static)
  add_library (ngtcp2::ngtcp2 INTERFACE IMPORTED)

  set_target_properties(ngtcp2_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
  )

  set_target_properties(ngtcp2_crypto_gnutls_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
  )

  set_target_properties (ngtcp2::ngtcp2 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${ngtcp2_INCLUDE_DIRS}"
  )

  target_link_libraries(ngtcp2::ngtcp2
    INTERFACE
      ngtcp2_static
      ngtcp2_crypto_gnutls_static
  )

  get_directory_property (ngtcp2_VERSION
    DIRECTORY ngtcp2
    DEFINITION PROJECT_VERSION)

  set (ngtcp2_FOUND TRUE)
  if (NOT ngtcp2_FIND_QUIETLY)
    message (STATUS "Using bundled ngtcp2 ${ngtcp2_VERSION} with the GnuTLS backend")
  endif ()
else()
  message (FATAL_ERROR "[ngtcp2] CRITICAL: targets of ngtcp2 library haven't been found!")
endif()

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (ngtcp2
  REQUIRED_VARS
    ngtcp2_FOUND
  VERSION_VAR
    ngtcp2_VERSION
  FAIL_MESSAGE
    "Could NOT find ngtcp2 >= ${ngtcp2_required_version} with the GnuTLS backend. Install the system development packages, initialize the ngtcp2 submodule, or select Seastar_NGTCP2_PROVIDER=BUNDLED.")

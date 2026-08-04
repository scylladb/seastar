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

  set (ngtcp2_FOUND TRUE)
else()
  message (FATAL_ERROR "[ngtcp2] CRITICAL: targets of ngtcp2 library haven't been found!")
endif()

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (ngtcp2
  REQUIRED_VARS
    ngtcp2_FOUND)

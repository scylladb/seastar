#
# This file is open source software, licensed to you under the terms
# of the Apache License, Version 2.0 (the "License").  See the NOTICE file
# distributed with this work for additional information regarding copyright
# ownership.  You may not use this file except in compliance with the License.
#
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

#
# Copyright (C) 2026 Kefu Chai (tchaikov@gmail.com)
#

# Find ngtcp2 and its crypto backends.
#
# The following imported targets are defined:
#   ngtcp2::ngtcp2                   - the core QUIC library
#   ngtcp2::ngtcp2_crypto_gnutls     - GnuTLS crypto backend (if found)
#   ngtcp2::ngtcp2_crypto_quictls    - OpenSSL/quictls crypto backend (if found)
#
# The following components are supported:
#   crypto_gnutls    - require the GnuTLS crypto backend
#   crypto_quictls   - require the OpenSSL/quictls crypto backend

find_package (PkgConfig REQUIRED)

pkg_check_modules (PC_ngtcp2 QUIET libngtcp2)

find_library (ngtcp2_LIBRARY
  NAMES ngtcp2
  HINTS
    ${PC_ngtcp2_LIBDIR}
    ${PC_ngtcp2_LIBRARY_DIRS})

find_path (ngtcp2_INCLUDE_DIR
  NAMES ngtcp2/ngtcp2.h
  HINTS
    ${PC_ngtcp2_INCLUDEDIR}
    ${PC_ngtcp2_INCLUDE_DIRS})

mark_as_advanced (
  ngtcp2_LIBRARY
  ngtcp2_INCLUDE_DIR)

# GnuTLS crypto backend
pkg_check_modules (PC_ngtcp2_crypto_gnutls QUIET libngtcp2_crypto_gnutls)

find_library (ngtcp2_crypto_gnutls_LIBRARY
  NAMES ngtcp2_crypto_gnutls
  HINTS
    ${PC_ngtcp2_crypto_gnutls_LIBDIR}
    ${PC_ngtcp2_crypto_gnutls_LIBRARY_DIRS})

mark_as_advanced (ngtcp2_crypto_gnutls_LIBRARY)

# OpenSSL/quictls crypto backend
pkg_check_modules (PC_ngtcp2_crypto_quictls QUIET libngtcp2_crypto_quictls)

find_library (ngtcp2_crypto_quictls_LIBRARY
  NAMES ngtcp2_crypto_quictls
  HINTS
    ${PC_ngtcp2_crypto_quictls_LIBDIR}
    ${PC_ngtcp2_crypto_quictls_LIBRARY_DIRS})

mark_as_advanced (ngtcp2_crypto_quictls_LIBRARY)

# Determine which components were found
set (_ngtcp2_required_vars ngtcp2_LIBRARY ngtcp2_INCLUDE_DIR)

if (Ngtcp2_FIND_REQUIRED_crypto_gnutls)
  list (APPEND _ngtcp2_required_vars ngtcp2_crypto_gnutls_LIBRARY)
endif ()

if (Ngtcp2_FIND_REQUIRED_crypto_quictls)
  list (APPEND _ngtcp2_required_vars ngtcp2_crypto_quictls_LIBRARY)
endif ()

include (FindPackageHandleStandardArgs)

find_package_handle_standard_args (Ngtcp2
  REQUIRED_VARS ${_ngtcp2_required_vars}
  VERSION_VAR PC_ngtcp2_VERSION)

if (Ngtcp2_FOUND)
  if (NOT TARGET ngtcp2::ngtcp2)
    add_library (ngtcp2::ngtcp2 UNKNOWN IMPORTED)

    set_target_properties (ngtcp2::ngtcp2
      PROPERTIES
        IMPORTED_LOCATION ${ngtcp2_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${ngtcp2_INCLUDE_DIR})
  endif ()

  if (ngtcp2_crypto_gnutls_LIBRARY AND NOT TARGET ngtcp2::ngtcp2_crypto_gnutls)
    add_library (ngtcp2::ngtcp2_crypto_gnutls UNKNOWN IMPORTED)

    set_target_properties (ngtcp2::ngtcp2_crypto_gnutls
      PROPERTIES
        IMPORTED_LOCATION ${ngtcp2_crypto_gnutls_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${ngtcp2_INCLUDE_DIR}
        INTERFACE_LINK_LIBRARIES ngtcp2::ngtcp2)
  endif ()

  if (ngtcp2_crypto_quictls_LIBRARY AND NOT TARGET ngtcp2::ngtcp2_crypto_quictls)
    add_library (ngtcp2::ngtcp2_crypto_quictls UNKNOWN IMPORTED)

    set_target_properties (ngtcp2::ngtcp2_crypto_quictls
      PROPERTIES
        IMPORTED_LOCATION ${ngtcp2_crypto_quictls_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${ngtcp2_INCLUDE_DIR}
        INTERFACE_LINK_LIBRARIES ngtcp2::ngtcp2)
  endif ()
endif ()

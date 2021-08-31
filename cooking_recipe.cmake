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
# Copyright (C) 2018 Scylladb, Ltd.
#

#
# Useful definitions for `cmake -E env`.
#

set (amended_PATH PATH=${Cooking_INGREDIENTS_DIR}/bin:$ENV{PATH})
set (PKG_CONFIG_PATH PKG_CONFIG_PATH=${Cooking_INGREDIENTS_DIR}/lib/pkgconfig)

#
# Some Autotools ingredients need this information because they don't use pkgconfig.
#

set (autotools_ingredients_flags
  CFLAGS=-I${Cooking_INGREDIENTS_DIR}/include
  CXXFLAGS=-I${Cooking_INGREDIENTS_DIR}/include
  LDFLAGS=-L${Cooking_INGREDIENTS_DIR}/lib)

#
# Some Autotools projects amend the info file instead of making a package-specific one.
# This doesn't play nicely with GNU Stow.
#
# Just append the name of the ingredient, like
#
#     ${info_dir}/gmp
#

set (info_dir --infodir=<INSTALL_DIR>/share/info)

#
# Build-concurrency.
#

cmake_host_system_information (
  RESULT build_concurrency_factor
  QUERY NUMBER_OF_LOGICAL_CORES)

set (make_command make -j ${build_concurrency_factor})

#
# All the ingredients.
#

##
## Dependencies of dependencies of dependencies.
##

cooking_ingredient (gmp
  EXTERNAL_PROJECT_ARGS
    URL https://gmplib.org/download/gmp/gmp-6.1.2.tar.bz2
    URL_MD5 8ddbb26dc3bd4e2302984debba1406a5
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR> ${info_dir}/gmp
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

##
## Dependencies of dependencies.
##

cooking_ingredient (colm
  EXTERNAL_PROJECT_ARGS
    URL http://www.colm.net/files/colm/colm-0.13.0.6.tar.gz
    URL_MD5 16aaf566cbcfe9a06154e094638ac709
    # This is upsetting.
    BUILD_IN_SOURCE YES
    CONFIGURE_COMMAND ./configure --prefix=<INSTALL_DIR>
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

cooking_ingredient (libpciaccess
  EXTERNAL_PROJECT_ARGS
    URL https://www.x.org/releases/individual/lib/libpciaccess-0.13.4.tar.gz
    URL_MD5 cc1fad87da60682af1d5fa43a5da45a4
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

cooking_ingredient (nettle
  REQUIRES gmp
  EXTERNAL_PROJECT_ARGS
    URL https://ftp.gnu.org/gnu/nettle/nettle-3.4.tar.gz
    URL_MD5 dc0f13028264992f58e67b4e8915f53d
    CONFIGURE_COMMAND
      <SOURCE_DIR>/configure
      --prefix=<INSTALL_DIR>
      --srcdir=<SOURCE_DIR>
      --libdir=<INSTALL_DIR>/lib
      ${info_dir}/nettle
      ${autotools_ingredients_flags}
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

# Also a direct dependency of Seastar.
cooking_ingredient (numactl
  EXTERNAL_PROJECT_ARGS
    URL https://github.com/numactl/numactl/releases/download/v2.0.12/numactl-2.0.12.tar.gz
    URL_MD5 2ba9777d78bfd7d408a387e53bc33ebc
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

cooking_ingredient (zlib
  EXTERNAL_PROJECT_ARGS
    URL https://zlib.net/zlib-1.2.11.tar.gz
    URL_MD5 1c9f62f0778697a09d36121ead88e08e
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR>
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

##
## Private and private/public dependencies.
##

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

cooking_ingredient (GnuTLS
  REQUIRES
    gmp
    nettle
  EXTERNAL_PROJECT_ARGS
    URL https://www.gnupg.org/ftp/gcrypt/gnutls/v3.5/gnutls-3.5.18.tar.xz
    URL_MD5 c2d93d305ecbc55939bc2a8ed4a76a3d
    CONFIGURE_COMMAND
     ${CMAKE_COMMAND} -E env ${PKG_CONFIG_PATH}
      <SOURCE_DIR>/configure
      --prefix=<INSTALL_DIR>
      --srcdir=<SOURCE_DIR>
      --with-included-unistring
      --with-included-libtasn1
      --without-p11-kit
      # https://lists.gnupg.org/pipermail/gnutls-help/2016-February/004085.html
      --disable-non-suiteb-curves
      --disable-doc
      ${autotools_ingredients_flags}
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

cooking_ingredient (hwloc
  REQUIRES
    numactl
    libpciaccess
  EXTERNAL_PROJECT_ARGS
    URL https://download.open-mpi.org/release/hwloc/v2.2/hwloc-2.2.0.tar.gz
    URL_MD5 762c93cdca3249eed4627c4a160192bd
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

cooking_ingredient (ragel
  REQUIRES colm
  EXTERNAL_PROJECT_ARGS
    URL http://www.colm.net/files/ragel/ragel-6.10.tar.gz
    URL_MD5 748cae8b50cffe9efcaa5acebc6abf0d
    PATCH_COMMAND
      sed -i "s/ CHAR_M/ SCHAR_M/g" ragel/common.cpp
    # This is upsetting.
    BUILD_IN_SOURCE YES
    CONFIGURE_COMMAND
      ${CMAKE_COMMAND} -E env ${amended_PATH}
      ./configure
      --prefix=<INSTALL_DIR>
      # This is even more upsetting.
      ${autotools_ingredients_flags}
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

cooking_ingredient (lksctp-tools
  EXTERNAL_PROJECT_ARGS
    URL https://sourceforge.net/projects/lksctp/files/lksctp-tools/lksctp-tools-1.0.16.tar.gz
    URL_MD5 708bb0b5a6806ad6e8d13c55b067518e
    PATCH_COMMAND ./bootstrap
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

cooking_ingredient (yaml-cpp
  REQUIRES Boost
  CMAKE_ARGS
    -DYAML_CPP_BUILD_TESTS=OFF
    -DBUILD_SHARED_LIBS=ON
  EXTERNAL_PROJECT_ARGS
    URL https://github.com/jbeder/yaml-cpp/archive/yaml-cpp-0.5.3.tar.gz
    URL_MD5 2bba14e6a7f12c7272f87d044e4a7211)

##
## Public dependencies.
##

cooking_ingredient (c-ares
  EXTERNAL_PROJECT_ARGS
    URL https://c-ares.haxx.se/download/c-ares-1.13.0.tar.gz
    URL_MD5 d2e010b43537794d8bedfb562ae6bba2
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=<INSTALL_DIR> --srcdir=<SOURCE_DIR>
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} install)

cooking_ingredient (cryptopp
  CMAKE_ARGS
    -DCMAKE_INSTALL_LIBDIR=<INSTALL_DIR>/lib
    -DBUILD_TESTING=OFF
  EXTERNAL_PROJECT_ARGS
    URL https://github.com/weidai11/cryptopp/archive/CRYPTOPP_5_6_5.tar.gz
    URL_MD5 88224d9c0322f63aa1fb5b8ae78170f0)

set (dpdk_args
  --default-library=static
  -Dc_args="-Wno-error"
  -Denable_docs=false
  -Dtests=false
  -Dexamples=
  -Datomic_mbuf_ref_counts=false
  -Dmax_memseg_lists=8192
  -Ddisable_drivers="net/softnic,net/bonding"
  -Ddisable_libs="kni,jobstats,lpm,acl,power,ip_frag,distributor,reorder,port,table,pipeline,flow_classify,bpf,efd,member"
  -Dcpu_instruction_set=${Seastar_DPDK_MACHINE})

if (CMAKE_BUILD_TYPE STREQUAL Debug)
  list (APPEND dpdk_args -Dbuildtype=debug)
endif ()

find_program (Meson_EXECUTABLE
  meson)
if (NOT Meson_EXECUTABLE)
  message (FATAL_ERROR "Cooking: Meson is required!")
endif ()

find_program (Ninja_EXECUTABLE
  ninja)
if (NOT Ninja_EXECUTABLE)
  message (FATAL_ERROR "Cooking: Ninja is required!")
endif ()

cooking_ingredient (dpdk
  EXTERNAL_PROJECT_ARGS
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/dpdk
    CONFIGURE_COMMAND
      env CC=${CMAKE_C_COMPILER} ${Meson_EXECUTABLE} ${dpdk_args} --prefix=<INSTALL_DIR> <BINARY_DIR> <SOURCE_DIR>
    BUILD_COMMAND
      ${Ninja_EXECUTABLE} -C <BINARY_DIR>
    INSTALL_COMMAND
      ${Ninja_EXECUTABLE} -C <BINARY_DIR> install)

cooking_ingredient (fmt
  EXTERNAL_PROJECT_ARGS
    URL https://github.com/fmtlib/fmt/archive/5.2.1.tar.gz
    URL_MD5 eaf6e3c1b2f4695b9a612cedf17b509d
  CMAKE_ARGS
    -DFMT_DOC=OFF
    -DFMT_TEST=OFF)

cooking_ingredient (lz4
  EXTERNAL_PROJECT_ARGS
    URL https://github.com/lz4/lz4/archive/v1.8.0.tar.gz
    URL_MD5 6247bf0e955899969d1600ff34baed6b
    # This is upsetting.
    BUILD_IN_SOURCE ON
    CONFIGURE_COMMAND <DISABLE>
    BUILD_COMMAND <DISABLE>
    INSTALL_COMMAND ${make_command} PREFIX=<INSTALL_DIR> install)

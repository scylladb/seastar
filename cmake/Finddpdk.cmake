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

find_path (dpdk_INCLUDE_DIR
  NAMES rte_atomic.h
  PATH_SUFFIXES dpdk)

find_library (dpdk_PMD_VMXNET3_UIO_LIBRARY rte_pmd_vmxnet3_uio)
find_library (dpdk_PMD_I40E_LIBRARY rte_pmd_i40e)
find_library (dpdk_PMD_IXGBE_LIBRARY rte_pmd_ixgbe)
find_library (dpdk_PMD_E1000_LIBRARY rte_pmd_e1000)
find_library (dpdk_PMD_BNXT_LIBRARY rte_pmd_bnxt)
find_library (dpdk_PMD_RING_LIBRARY rte_pmd_ring)
find_library (dpdk_PMD_CXGBE_LIBRARY rte_pmd_cxgbe)
find_library (dpdk_PMD_ENA_LIBRARY rte_pmd_ena)
find_library (dpdk_PMD_ENIC_LIBRARY rte_pmd_enic)
find_library (dpdk_PMD_FM10K_LIBRARY rte_pmd_fm10k)
find_library (dpdk_PMD_NFP_LIBRARY rte_pmd_nfp)
find_library (dpdk_PMD_QEDE_LIBRARY rte_pmd_qede)
find_library (dpdk_RING_LIBRARY rte_ring)
find_library (dpdk_KVARGS_LIBRARY rte_kvargs)
find_library (dpdk_MEMPOOL_LIBRARY rte_mempool)
find_library (dpdk_MEMPOOL_RING_LIBRARY rte_mempool_ring)
find_library (dpdk_PMD_SFC_EFX_LIBRARY rte_pmd_sfc_efx)
find_library (dpdk_HASH_LIBRARY rte_hash)
find_library (dpdk_CMDLINE_LIBRARY rte_cmdline)
find_library (dpdk_MBUF_LIBRARY rte_mbuf)
find_library (dpdk_CFGFILE_LIBRARY rte_cfgfile)
find_library (dpdk_EAL_LIBRARY rte_eal)
find_library (dpdk_ETHDEV_LIBRARY rte_ethdev)
find_library (dpdk_NET_LIBRARY rte_net)
find_library (dpdk_TIMER_LIBRARY rte_timer)
find_library (dpdk_PCI_LIBRARY rte_pci)
find_library (dpdk_BUS_PCI_LIBRARY rte_bus_pci)
find_library (dpdk_BUS_VDEV_LIBRARY rte_bus_vdev)

include (FindPackageHandleStandardArgs)

set (dpdk_REQUIRED
  dpdk_INCLUDE_DIR
  dpdk_PMD_VMXNET3_UIO_LIBRARY
  dpdk_PMD_I40E_LIBRARY
  dpdk_PMD_IXGBE_LIBRARY
  dpdk_PMD_E1000_LIBRARY
  dpdk_PMD_BNXT_LIBRARY
  dpdk_PMD_RING_LIBRARY
  dpdk_PMD_CXGBE_LIBRARY
  dpdk_PMD_ENA_LIBRARY
  dpdk_PMD_ENIC_LIBRARY
  dpdk_PMD_NFP_LIBRARY
  dpdk_PMD_QEDE_LIBRARY
  dpdk_RING_LIBRARY
  dpdk_KVARGS_LIBRARY
  dpdk_MEMPOOL_LIBRARY
  dpdk_MEMPOOL_RING_LIBRARY
  dpdk_HASH_LIBRARY
  dpdk_CMDLINE_LIBRARY
  dpdk_MBUF_LIBRARY
  dpdk_CFGFILE_LIBRARY
  dpdk_EAL_LIBRARY
  dpdk_ETHDEV_LIBRARY
  dpdk_NET_LIBRARY
  dpdk_TIMER_LIBRARY
  dpdk_PCI_LIBRARY
  dpdk_BUS_PCI_LIBRARY
  dpdk_BUS_VDEV_LIBRARY)

# fm10k, sfc_efx driver can only build on x86
if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
  set (dpdk_REQUIRED
    ${dpdk_REQUIRED}
    dpdk_PMD_FM10K_LIBRARY
    dpdk_PMD_SFC_EFX_LIBRARY)
endif()

find_package_handle_standard_args (dpdk
  REQUIRED_VARS
    ${dpdk_REQUIRED}
)

if (dpdk_FOUND AND NOT (TARGET dpdk::dpdk))
  set (dpdk_LIBRARIES
    ${dpdk_CFGFILE_LIBRARY}
    ${dpdk_CMDLINE_LIBRARY}
    ${dpdk_ETHDEV_LIBRARY}
    ${dpdk_HASH_LIBRARY}
    ${dpdk_MBUF_LIBRARY}
    ${dpdk_EAL_LIBRARY}
    ${dpdk_KVARGS_LIBRARY}
    ${dpdk_MEMPOOL_LIBRARY}
    ${dpdk_MEMPOOL_RING_LIBRARY}
    ${dpdk_PMD_BNXT_LIBRARY}
    ${dpdk_PMD_E1000_LIBRARY}
    ${dpdk_PMD_ENA_LIBRARY}
    ${dpdk_PMD_ENIC_LIBRARY}
    ${dpdk_PMD_QEDE_LIBRARY}
    ${dpdk_PMD_I40E_LIBRARY}
    ${dpdk_PMD_IXGBE_LIBRARY}
    ${dpdk_PMD_NFP_LIBRARY}
    ${dpdk_PMD_RING_LIBRARY}
    ${dpdk_PMD_VMXNET3_UIO_LIBRARY})

  if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set (dpdk_LIBRARIES
      ${dpdk_LIBRARIES}
      ${dpdk_PMD_FM10K_LIBRARY}
      ${dpdk_PMD_SFC_EFX_LIBRARY})
  endif()

  set (dpdk_LIBRARIES
    ${dpdk_LIBRARIES}
    ${dpdk_RING_LIBRARY}
    ${dpdk_NET_LIBRARY}
    ${dpdk_TIMER_LIBRARY}
    ${dpdk_PCI_LIBRARY}
    ${dpdk_BUS_PCI_LIBRARY}
    ${dpdk_BUS_VDEV_LIBRARY})

  # ensure that all dpdk libraries are linked in with --whole-archive
  set (dpdk_LIBRARIES -Wl,--whole-archive ${dpdk_LIBRARIES} -Wl,--no-whole-archive)
  # additional variable with commas replaced to use in ".pc.in" genex expansion
  string(REPLACE "," "$<COMMA>" genex_dpdk_LIBRARIES "${dpdk_LIBRARIES}")

  #
  # pmd_vmxnet3_uio
  #

  add_library (dpdk::pmd_vmxnet3_uio UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_vmxnet3_uio
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_VMXNET3_UIO_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_i40e
  #

  add_library (dpdk::pmd_i40e UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_i40e
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_I40E_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_ixgbe
  #

  add_library (dpdk::pmd_ixgbe UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_ixgbe
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_IXGBE_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_e1000
  #

  add_library (dpdk::pmd_e1000 UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_e1000
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_E1000_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_bnxt
  #

  add_library (dpdk::pmd_bnxt UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_bnxt
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_BNXT_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_ring
  #

  add_library (dpdk::pmd_ring UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_ring
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_RING_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_cxgbe
  #

  add_library (dpdk::pmd_cxgbe UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_cxgbe
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_CXGBE_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_ena
  #

  add_library (dpdk::pmd_ena UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_ena
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_ENA_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_enic
  #

  add_library (dpdk::pmd_enic UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_enic
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_ENIC_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_fm10k
  #

  add_library (dpdk::pmd_fm10k UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_fm10k
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_FM10K_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_nfp
  #

  add_library (dpdk::pmd_nfp UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_nfp
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_NFP_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_qede
  #

  add_library (dpdk::pmd_qede UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_qede
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_QEDE_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pmd_sfc_efx
  #

  add_library (dpdk::pmd_sfc_efx UNKNOWN IMPORTED)

  set_target_properties (dpdk::pmd_sfc_efx
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PMD_SFC_EFX_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # hash
  #

  add_library (dpdk::hash UNKNOWN IMPORTED)

  set_target_properties (dpdk::hash
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_HASH_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # kvargs
  #

  add_library (dpdk::kvargs UNKNOWN IMPORTED)

  set_target_properties (dpdk::kvargs
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_KVARGS_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # mbuf
  #

  add_library (dpdk::mbuf UNKNOWN IMPORTED)

  set_target_properties (dpdk::mbuf
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_MBUF_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR}
      INTERFACE_LINK_LIBRARIES dpdk::eal)

  #
  # eal (since dpdk 18.08, eal depends on kvargs)
  #

  add_library (dpdk::eal UNKNOWN IMPORTED)

  set_target_properties (dpdk::eal
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_EAL_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR}
      INTERFACE_LINK_LIBRARIES dpdk::kvargs)

  #
  # ethdev
  #

  add_library (dpdk::ethdev UNKNOWN IMPORTED)

  set_target_properties (dpdk::ethdev
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_ETHDEV_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR}
      INTERACE_LINK_LIBRARIES dpdk::eal)

  #
  # mempool
  #

  add_library (dpdk::mempool UNKNOWN IMPORTED)

  set_target_properties (dpdk::mempool
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_MEMPOOL_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # mempool_ring
  #

  add_library (dpdk::mempool_ring UNKNOWN IMPORTED)

  set_target_properties (dpdk::mempool_ring
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_MEMPOOL_RING_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # ring
  #

  add_library (dpdk::ring UNKNOWN IMPORTED)

  set_target_properties (dpdk::ring
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_RING_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # cmdline
  #

  add_library (dpdk::cmdline UNKNOWN IMPORTED)

  set_target_properties (dpdk::cmdline
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_CMDLINE_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # cfgfile
  #

  add_library (dpdk::cfgfile UNKNOWN IMPORTED)

  set_target_properties (dpdk::cfgfile
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_CFGFILE_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # net
  #

  add_library (dpdk::net UNKNOWN IMPORTED)

  set_target_properties (dpdk::net
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_NET_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # timer
  #

  add_library (dpdk::timer UNKNOWN IMPORTED)

  set_target_properties (dpdk::timer
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_TIMER_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # pci
  #

  add_library (dpdk::pci UNKNOWN IMPORTED)

  set_target_properties (dpdk::pci
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_PCI_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # bus_pci
  #

  add_library (dpdk::bus_pci UNKNOWN IMPORTED)

  set_target_properties (dpdk::bus_pci
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_BUS_PCI_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # bus_vdev
  #

  add_library (dpdk::bus_vdev UNKNOWN IMPORTED)

  set_target_properties (dpdk::bus_vdev
    PROPERTIES
      IMPORTED_LOCATION ${dpdk_BUS_VDEV_LIBRARY}
      INTERFACE_INCLUDE_DIRECTORIES ${dpdk_INCLUDE_DIR})

  #
  # Summary.
  #

  add_library (dpdk::dpdk INTERFACE IMPORTED)

  set (_dpdk_libraries
    dpdk::cfgfile
    dpdk::cmdline
    dpdk::eal
    dpdk::ethdev
    dpdk::hash
    dpdk::kvargs
    dpdk::mbuf
    dpdk::mempool
    dpdk::mempool_ring
    dpdk::pmd_bnxt
    dpdk::pmd_cxgbe
    dpdk::pmd_e1000
    dpdk::pmd_ena
    dpdk::pmd_enic
    dpdk::pmd_qede
    dpdk::pmd_i40e
    dpdk::pmd_ixgbe
    dpdk::pmd_nfp
    dpdk::pmd_ring
    dpdk::pmd_vmxnet3_uio
    dpdk::ring
    dpdk::net
    dpdk::timer
    dpdk::pci
    dpdk::bus_pci
    dpdk::bus_vdev)

  if (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set (_dpdk_libraries
      ${_dpdk_libraries}
      dpdk::pmd_fm10k
      dpdk::pmd_sfc_efx)
  endif()

  set_target_properties (dpdk::dpdk
    PROPERTIES
      INTERFACE_LINK_LIBRARIES "${_dpdk_libraries}")
endif ()

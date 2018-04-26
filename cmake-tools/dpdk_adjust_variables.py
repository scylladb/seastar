#!/usr/bin/python3

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

"""
Doing this from CMake is a pain due to its poor type system.
"""

import argparse
import os

# Adjust to taste.
DEFAULTS = {
    'CONFIG_RTE_LIBRTE_PMD_BOND': 'n',
    'CONFIG_RTE_MBUF_SCATTER_GATHER': 'n',
    'CONFIG_RTE_LIBRTE_IP_FRAG': 'n',
    'CONFIG_RTE_APP_TEST': 'n',
    'CONFIG_RTE_TEST_PMD': 'n',
    'CONFIG_RTE_MBUF_REFCNT_ATOMIC': 'n',
    'CONFIG_RTE_MAX_MEMSEG': '8192',
    'CONFIG_RTE_EAL_IGB_UIO': 'n',
    'CONFIG_RTE_LIBRTE_KNI': 'n',
    'CONFIG_RTE_KNI_KMOD': 'n',
    'CONFIG_RTE_LIBRTE_JOBSTATS': 'n',
    'CONFIG_RTE_LIBRTE_LPM': 'n',
    'CONFIG_RTE_LIBRTE_ACL': 'n',
    'CONFIG_RTE_LIBRTE_POWER': 'n',
    'CONFIG_RTE_LIBRTE_IP_FRAG': 'n',
    'CONFIG_RTE_LIBRTE_METER': 'n',
    'CONFIG_RTE_LIBRTE_SCHED': 'n',
    'CONFIG_RTE_LIBRTE_DISTRIBUTOR': 'n',
    'CONFIG_RTE_LIBRTE_PMD_CRYPTO_SCHEDULER': 'n',
    'CONFIG_RTE_LIBRTE_REORDER': 'n',
    'CONFIG_RTE_LIBRTE_PORT': 'n',
    'CONFIG_RTE_LIBRTE_TABLE': 'n',
    'CONFIG_RTE_LIBRTE_PIPELINE': 'n',
}

parser = argparse.ArgumentParser(description='Update the configuration variables for DPDK.')
parser.add_argument('file', metavar='FILE', help="Path of the DPDK configuration file.")
parser.add_argument('machine', metavar='NAME', help="The architecture identifier for DPDK, like `ivb`.")

args = parser.parse_args()

lines = open(args.file, encoding='UTF-8').readlines()
new_lines = []

for line in lines:
    for var, val in DEFAULTS.items():
        if line.startswith(var + '='):
            line = '{}={}\n'.format(var, val)

    new_lines.append(line)

new_lines += 'CONFIG_RTE_MACHINE={}\n'.format(args.machine)
open(args.file, 'w', encoding='UTF-8').writelines(new_lines)

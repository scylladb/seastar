#!/usr/bin/env -S uv run --script
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
# Copyright (C) 2026 Redpanda Data.
#
# /// script
# requires-python = ">=3.10"
# dependencies = [
#   "covertable",
#   "ruamel.yaml",
# ]
# ///
"""
Generate the regular_test include: matrix for tests.yaml.

Uses pairwise coverage over compiler x standard x mode x arch for the
regular builds, plus an explicit list of special-purpose jobs (dpdk,
cxx-modules, fuzz). Pairwise guarantees that every pair of parameter
values is exercised by at least one job, with far fewer combinations
than the full cartesian product. Pairs which cannot appear at all (see
CONSTRAINTS) are the exception, and _validate_pairwise() enforces the
guarantee over every other pair. The file is round-tripped through
ruamel.yaml, replacing only the strategy matrix include list;
everything else in tests.yaml is left untouched.

Usage:
    .github/workflows/gen-matrix.py
    git diff .github/workflows/tests.yaml
"""

import itertools
import pathlib
from typing import Any

from covertable import make
from ruamel.yaml import YAML
from ruamel.yaml.comments import CommentedMap, CommentedSeq

HERE = pathlib.Path(__file__).parent
TESTS_YAML = HERE / "tests.yaml"

# Compilers are named with the version suffix that matches the Ubuntu
# package binary (clang-21, g++-16, ...). install-build-env.sh installs
# the corresponding apt package and derives CC from CPP. We cover the
# last two major releases of each toolchain per compatibility.md.
CLANG_VERSIONS: list[int] = [21, 22]
GCC_VERSIONS:   list[int] = [15, 16]
CLANG_COMPILERS: list[str] = [f"clang++-{v}" for v in CLANG_VERSIONS]
GCC_COMPILERS:   list[str] = [f"g++-{v}"     for v in GCC_VERSIONS]
COMPILERS: list[str] = CLANG_COMPILERS + GCC_COMPILERS
STANDARDS: list[int] = [23, 26]
MODES: list[str] = ["debug", "release", "sanitize"]
ARCHS: list[str] = ["x86", "arm"]

# The parameters the regular jobs are drawn from, by matrix key.
PARAMETERS: dict[str, list[Any]] = {
    "compiler": COMPILERS,
    "standard": STANDARDS,
    "mode": MODES,
    "arch": ARCHS,
}

# Modes gcc cannot build: gcc 15+ miscompiles structured bindings inside loops
# in coroutines (https://gcc.gnu.org/bugzilla/show_bug.cgi?id=124584), breaking
# g++ debug (LSan: pollable_fd_state leak via cross-shard accept) and g++
# sanitize (UBSan abort on poisoned rcv_buf in rpc loops). Tracked in
# scylladb/seastar#3431.
GCC_BROKEN_MODES: list[str] = ["debug", "sanitize"]

# No gcc combined with those modes, given to the generator as a constraint so it
# treats the combination as unreachable while working out which rows it needs,
# rather than us dropping rows it has already committed to.
CONSTRAINTS: list[dict[str, Any]] = [
    {
        "operator": "or",
        "conditions": [
            {
                "operator": "not",
                "condition": {"operator": "in", "left": "compiler", "values": GCC_COMPILERS},
            },
            {
                "operator": "not",
                "condition": {"operator": "in", "left": "mode", "values": GCC_BROKEN_MODES},
            },
        ],
    }
]


def _is_excluded(key_a: str, value_a: Any, key_b: str, value_b: Any) -> bool:
    """Is this pair of parameter values one CONSTRAINTS rules out?"""
    if {key_a, key_b} != {"compiler", "mode"}:
        return False
    compiler, mode = (value_a, value_b) if key_a == "compiler" else (value_b, value_a)
    return compiler in GCC_COMPILERS and mode in GCC_BROKEN_MODES


def _validate_pairwise(rows: list[dict[str, Any]]) -> None:
    """Fail unless every pair of parameter values which may appear does.

    Only the generated rows are checked. The special-purpose jobs cover
    combinations of their own, but leaning on them would be wrong: the
    cxx-modules job runs no tests at all, so a pair it is the only carrier
    of is not exercised by anything.
    """
    absent = [
        f"{key_a}={value_a} with {key_b}={value_b}"
        for key_a, key_b in itertools.combinations(PARAMETERS, 2)
        for value_a, value_b in itertools.product(PARAMETERS[key_a], PARAMETERS[key_b])
        if not _is_excluded(key_a, value_a, key_b, value_b)
        and not any(row[key_a] == value_a and row[key_b] == value_b for row in rows)
    ]
    if absent:
        raise SystemExit(
            "pairwise coverage is incomplete, "
            f"{len(absent)} pair(s) appear in no job:\n  " + "\n  ".join(absent)
        )

# Key path to the strategy matrix map inside tests.yaml; we replace its
# "include" list. The "# AUTOGENERATED" comment is preserved automatically
# on round-trip.
MATRIX_PATH = ("jobs", "regular_test", "strategy", "matrix")

# Special jobs merged into the same matrix so tests.yaml stays a single job.
# Items only set enable-ccache when overriding the default (test.yaml treats
# absent/empty as enabled).
SPECIAL_ITEMS: list[dict[str, Any]] = [
    # The dev job doubles as our heap profiling coverage: nothing else in
    # the matrix defines SEASTAR_HEAPPROF, so the sampled-memory-profile
    # code and the tests guarded by it are otherwise never compiled.
    {
        "compiler": "clang++-22",
        "standard": 23,
        "arch": "x86",
        "mode": "dev",
        "options": "--heap-profiling",
        "info": "heapprof, ",
    },
    {
        "compiler": "clang++-22",
        "standard": 23,
        "arch": "x86",
        "mode": "release",
        "enables": "--enable-dpdk",
        "options": "--cook dpdk --dpdk-machine corei7-avx",
        "info": "dpdk, ",
    },
    {
        "compiler": "clang++-22",
        "standard": 23,
        "arch": "x86",
        "mode": "debug",
        "enables": "--enable-cxx-modules",
        "enable-ccache": False,
        "info": "modules, ",
    },
    {
        "compiler": "clang++-22",
        "standard": 23,
        "arch": "x86",
        "mode": "fuzz",
        "test-args": "-- -R 'Seastar.fuzz.'",
    },
]

def _build_include(items: list[dict[str, Any]]) -> CommentedSeq:
    """Build the matrix include list as a block sequence of flow mappings.

    Each item becomes a flow-style mapping (one job per line) so the block
    stays compact and diff-friendly; ruamel.yaml handles scalar quoting.
    """
    seq = CommentedSeq()
    for item in items:
        mapping = CommentedMap(item)
        mapping.fa.set_flow_style()
        seq.append(mapping)
    return seq


def generate() -> list[dict[str, Any]]:
    # The rows come back keyed by parameter name in an order of the generator's
    # choosing, so rebuild each one in PARAMETERS order to keep the generated
    # YAML stable.
    regular: list[dict[str, Any]] = [
        {key: row[key] for key in PARAMETERS}
        for row in make(PARAMETERS, strength=2, constraints=CONSTRAINTS)
    ]
    _validate_pairwise(regular)
    return regular + SPECIAL_ITEMS


def main() -> None:
    items = generate()

    yaml = YAML()
    yaml.preserve_quotes = True
    yaml.width = 1 << 20  # never wrap a flow mapping across lines
    yaml.indent(mapping=2, sequence=4, offset=2)

    data = yaml.load(TESTS_YAML)
    matrix = data
    for key in MATRIX_PATH:
        matrix = matrix[key]

    # We only swap out the include list. The "# AUTOGENERATED, do not edit"
    # comment is attached to the include key, not the list, so it is
    # preserved automatically on round-trip.
    matrix["include"] = _build_include(items)

    yaml.dump(data, TESTS_YAML)
    print(f"Updated {TESTS_YAML}: {len(items)} matrix items")


if __name__ == "__main__":
    main()

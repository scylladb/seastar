#!/usr/bin/env bash
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
# Install the seastar build toolchain and run ./configure.py inside the
# CI container. Called from .github/workflows/test.yaml after checkout.
#
# Expects these environment variables:
#   COMPILER       clang++-N | g++-N  (e.g. clang++-22, g++-16)
#   ENABLES        string of --enable-* flags passed to configure.py
#   STANDARD       C++ standard (e.g. 20, 23)
#   MODE           build mode (dev, debug, release, fuzz)
#   OPTIONS        extra configure.py args (may contain spaces)
#   ENABLE_CCACHE  true | false (controls --compiler-cache=ccache)
#
# Locally:
#   COMPILER=clang++-22 ENABLES='' STANDARD=23 MODE=dev OPTIONS='' \
#     ENABLE_CCACHE=false ./install-build-env.sh
#
# ::group:: / ::endgroup:: are GitHub Actions log-folding markers so
# each phase shows up collapsed in the CI log, mirroring what separate
# steps used to look like. Harmless plain text when run locally.
set -euo pipefail

# Suppress debconf frontend warnings when apt installs packages with
# postinst prompts (the container has no tty / no readline / no dialog).
export DEBIAN_FRONTEND=noninteractive

group() {
    echo "::endgroup::"
    echo "::group::$*"
}
trap 'echo "::endgroup::"' EXIT
echo "::group::install-dependencies.sh"

./install-dependencies.sh

packages=()
case "$COMPILER" in
    clang++-*)
        version="${COMPILER#clang++-}"
        # libstdc++-16-dev: libboost-all-dev pulls gcc-16-base but not
        # libstdc++-16-dev; clang prefers the highest gcc dir, so
        # without this `ld: cannot find -lstdc++` aborts the link.
        packages+=("clang-${version}" libstdc++-16-dev)
        CC="clang-${version}"
        CPP="$COMPILER"
        ;;
    g++-*)
        version="${COMPILER#g++-}"
        packages+=("gcc-${version}" "g++-${version}")
        CC="gcc-${version}"
        CPP="$COMPILER"
        ;;
    *) echo "install-build-env.sh: unknown COMPILER='$COMPILER'" >&2; exit 1 ;;
esac

if [[ "$ENABLES" == *cxx-modules* ]]; then
    if [[ "$COMPILER" != clang++-* ]]; then
        echo "install-build-env.sh: cxx-modules requires clang++, got '$COMPILER'" >&2
        exit 1
    fi
    packages+=("clang-tools-${version}")
fi

group "apt-get install ${packages[*]}"
apt-get install -y "${packages[@]}"

ccache_opt=()
if [[ "$ENABLE_CCACHE" != "false" ]]; then
    ccache_opt=(--compiler-cache=ccache)
fi

# --cook fmt for clang >= 20 or C++26: the system libfmt-dev on ubuntu:26.04 is
# 10.1.1, and clang-20+ enforces consteval strictly enough to reject
# fmt/chrono.h's FMT_STRING("{:.{}f}") path. C++26 requires a fmt fix
# for std::optional formatting (https://github.com/fmtlib/fmt/issues/4760)
# The check is version-guarded rather
# than blanket-applied so future pre-20 clang versions in the matrix
# don't pay the build cost.
cook_args=()
clang_ver="${COMPILER#clang++-}"
if [[ "$COMPILER" == clang++-* && "$clang_ver" -ge 20 || "$STANDARD" -ge 26 ]] ; then
    cook_args=(--cook fmt)
fi

group "configure.py"
# OPTIONS / ENABLES intentionally unquoted: each carries multiple
# whitespace-separated args (e.g. "--cook dpdk --dpdk-machine corei7-avx").
# shellcheck disable=SC2086
./configure.py                  \
    --c++-standard "$STANDARD"  \
    --compiler "$CPP"           \
    --c-compiler "$CC"          \
    --mode "$MODE"              \
    "${cook_args[@]}"           \
    "${ccache_opt[@]}"          \
    $OPTIONS                    \
    $ENABLES

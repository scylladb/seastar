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
# Run ./configure.py inside the CI container. Called from
# .github/workflows/test.yaml after checkout.
#
# The container is the frozen seastar-toolchain image (see test.yaml), which
# already carries every compiler in the matrix and every dependency
# install-dependencies.sh lists. Nothing is installed here: an apt-get install
# would pull whatever the archive has today, which is exactly the drift the
# frozen image exists to prevent.
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
#     ENABLE_CCACHE=false ./configure-build.sh
set -euo pipefail

case "$COMPILER" in
    clang++-*)
        CC="clang-${COMPILER#clang++-}"
        CPP="$COMPILER"
        ;;
    g++-*)
        CC="gcc-${COMPILER#g++-}"
        CPP="$COMPILER"
        ;;
    *) echo "configure-build.sh: unknown COMPILER='$COMPILER'" >&2; exit 1 ;;
esac

if [[ "$ENABLES" == *cxx-modules* && "$COMPILER" != clang++-* ]]; then
    echo "configure-build.sh: cxx-modules requires clang++, got '$COMPILER'" >&2
    exit 1
fi

# A compiler added to the matrix before the image is rebuilt with it should
# fail here, by name, rather than somewhere inside cmake.
for tool in "$CC" "$CPP"; do
    if ! command -v "$tool" >/dev/null; then
        echo "configure-build.sh: $tool is not in the toolchain image; rebuild it from docker/dev/Dockerfile" >&2
        exit 1
    fi
done

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
#
# --enable-import-fmt needs one too, for a different reason: it consumes fmt as
# a C++20 module, which no libfmt-dev ships -- the module has to be built
# alongside the library, which cooking_recipe.cmake arranges (with FMT_MODULE)
# when that option is on.
cook_args=()
clang_ver="${COMPILER#clang++-}"
if [[ "$COMPILER" == clang++-* && "$clang_ver" -ge 20 || "$STANDARD" -ge 26 ]] \
   || [[ "$ENABLES" == *import-fmt* ]]; then
    cook_args=(--cook fmt)
fi

# Both TLS backends are requested explicitly rather than left to cmake's
# auto-detection, so that a TLS development package going missing from the
# image fails the configure instead of quietly dropping that backend, and the
# tests which cover it, from the run.
#
# OPTIONS / ENABLES intentionally unquoted: each carries multiple
# whitespace-separated args (e.g. "--cook dpdk --dpdk-machine corei7-avx").
# shellcheck disable=SC2086
./configure.py                  \
    --c++-standard "$STANDARD"  \
    --compiler "$CPP"           \
    --c-compiler "$CC"          \
    --mode "$MODE"              \
    --enable-gnutls             \
    --enable-openssl            \
    "${cook_args[@]}"           \
    "${ccache_opt[@]}"          \
    $OPTIONS                    \
    $ENABLES

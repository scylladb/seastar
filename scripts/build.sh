#!/bin/bash
# This scripts expects seastar-dev container to be built e.g. like this
# $ docker build -t seastar-dev  -f ./docker/dev/Dockerfile .

BUILDER_IMAGE=${BUILDER_IMAGE:=seastar-dev}

if [ $# -eq 0 -o "$1" == "--help" -o "$1" == "-h" ]; then
    echo "Usage: $(basename $0) <mode> [<compiler>] [<version>] [<c++ standard>]"
    exit 0
fi

if [ -z "$CONTAINERIZED" ]; then
    OPTIONS=""
    if [ -t 1 ]; then
        OPTIONS="-it"
        echo "Wrapping self into $BUILDER_IMAGE container"
    fi
    exec docker run $OPTIONS --rm -v$(pwd):/home/src -e 'CONTAINERIZED=yes' -w /home/src $BUILDER_IMAGE /home/src/scripts/$(basename $0) "$@"
fi

set -e
set -x
update-alternatives --auto gcc
update-alternatives --auto clang

MODE=$1
COMPILER=$2
VERSION=$3
STANDARD=$4

CONFIGURE="--mode=$MODE"
if [ ! -z "$COMPILER" ]; then
    if [ "$COMPILER" == "gcc" ]; then
        CPP_COMPILER="g++"
    elif [ "$COMPILER" == "clang" ]; then
        CPP_COMPILER="clang++"
    else
        echo "Unknown compiler (use 'gcc' or 'clang')"
        exit 1
    fi
    CONFIGURE="$CONFIGURE --compiler=$CPP_COMPILER"

    if [ ! -z "$VERSION" ]; then
        update-alternatives --set $COMPILER /usr/bin/${COMPILER}-${VERSION}
        update-alternatives --set $CPP_COMPILER /usr/bin/${CPP_COMPILER}-${VERSION}

        if [ ! -z "$STANDARD" ]; then
            CONFIGURE="$CONFIGURE --c++-standard=$STANDARD"
        fi
    fi
fi

./configure.py $CONFIGURE
ninja -C build/$MODE

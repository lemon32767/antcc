#!/bin/bash

cd "$(dirname "$0")"
. ./t.sh
ver=1.13.5
get "https://github.com/hirrolot/metalang99/archive/refs/tags/v$ver.tar.gz" \
    metalang99-$ver

cd metalang99-$ver
echo '  -- examples --'
mkdir -p examples/build
cd examples/build
cmake --fresh .. -DCMAKE_C_COMPILER="$antcc" 
cmake --build .

cd ../..
echo '  -- tests --'
set -e
mkdir -p tests/build
cd tests/build
cmake --fresh ..  -DCMAKE_C_COMPILER="$antcc" 
cmake --build .

if [ "$OSTYPE" = "linux-gnu" ]; then
    echo " Testing ./gen ..."
    ./gen

    echo " Testing ./stmt ..."
    ./stmt
fi

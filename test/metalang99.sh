#!/bin/bash

set -e

cd $(dirname "$0")/external/metalang99/
cc=$(realpath ../../../antcc)
echo '  -- examples --'
mkdir -p examples/build
cd examples/build
cmake --fresh .. -DCMAKE_C_COMPILER="$cc" 
cmake --build .

cd ../..
echo '  -- tests --'
set -e
mkdir -p tests/build
cd tests/build
cmake --fresh ..  -DCMAKE_C_COMPILER="$cc" 
cmake --build .

if [ "$OSTYPE" = "linux-gnu" ]; then
    echo " Testing ./gen ..."
    ./gen

    echo " Testing ./stmt ..."
    ./stmt
fi

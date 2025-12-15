#!/bin/env sh

cd $(dirname "$0")
./run.sh

echo "--- Lua 5.4.0 ---"
./lua.sh

echo "--- c-testsuite ---"
./c-testsuite.sh

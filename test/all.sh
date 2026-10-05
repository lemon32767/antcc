#!/bin/sh

cd "$(dirname "$0")"
./c/run.sh
./ir/run.sh

echo "=== Lua 5.4.0 ==="
./external/lua.sh

echo "=== c-testsuite ==="
./c-testsuite.sh

echo "=== metalang99 (preprocessor) ==="
./external/metalang99.sh

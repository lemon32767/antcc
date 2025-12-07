#!/bin/env sh

cd $(dirname "$0")
./run.sh

echo "--- 'Write a C Compiler' test suite ---"
cd nlsandler-write_a_c_compiler
./test_compiler.sh ../../antcc

cd ..

echo "--- Lua 5.4.0 ---"
./lua.sh

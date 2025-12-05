#!/bin/env sh

cd $(dirname "$0")
./run.sh

echo "--- 'Write a C Compiler' test suite ---"
cd nlsandler-write_a_c_compiler
exec ./test_compiler.sh ../../antcc

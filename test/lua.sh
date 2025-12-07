#!/bin/sh

cd $(dirname "$0")/external
make -C lua-5.4.0/ clean all
cd lua-5.4.0-tests/
../lua-5.4.0/src/lua -e"_U=true" all.lua

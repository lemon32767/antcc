#!/bin/sh

cd "$(dirname "$0")"

. ./t.sh
ver=5.4.0
get "https://lua.org/ftp/lua-$ver.tar.gz" lua-$ver
get "https://lua.org/tests/lua-$ver-tests.tar.gz" lua-$ver-tests
cd lua-$ver
patch sed -i 's/CC=.*$//' src/Makefile

export CC=$antcc
export V=1
make clean
make
cd ../lua-tests/
../lua/src/lua -e"_U=true" all.lua

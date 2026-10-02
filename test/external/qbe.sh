#!/bin/sh

cd "$(dirname "$0")"

. ./t.sh
ver=1.3
get "https://c9x.me/compile/release/qbe-$ver.tar.xz" qbe-$ver
cd qbe-$ver

patch sed -i 's/CC *=.*$//' Makefile
make clean
CC=$antcc make
./tools/test.sh all

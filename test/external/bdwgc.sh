#!/bin/sh

cd "$(dirname "$0")"

. ./t.sh
ver=8.2.12
get "https://github.com/bdwgc/bdwgc/releases/download/v$ver/gc-$ver.tar.gz" gc-$ver
cd gc-$ver

export CC=$antcc
./configure
make clean
make check

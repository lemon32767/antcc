#!/bin/sh

cd "$(dirname "$0")"
. ./t.sh
ver=3530200
get "https://sqlite.org/2026/sqlite-src-$ver.zip" sqlite-src-$ver
cd sqlite-src-$ver
##export CC_FOR_BUILD=$CC ##miscompilation in sqlite's fuzzer and some of its build tools?
CCACHE=none CC="$(realpath ../../../antcc)" ./configure
make clean ##clean-tool-zip clean-sanity-check
V=1 make

if [ x"$1" = xtest ] && command -v tclsh >/dev/null 2>&1; then
    ## needs tcl; is also very intensive
    ## N.B. tcl needs __attribute__((packed)) epoll. with __GNUC__ it also needs -> weak attr <-
    V=1 make devtest
fi

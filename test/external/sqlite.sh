#!/bin/sh

cd $(dirname "$0")/sqlite
if ! test -f ./configure; then
    echo 'sqlite submodule not pulled in!'
    exit 1
fi
export CCACHE=none CC="$(realpath ../../../antcc)"
##export CC_FOR_BUILD=$CC ##miscompilation in sqlite's fuzzer?
./configure
make clean ##clean-tool-zip clean-sanity-check
V=1 make

if [ x"$1" = xtest ] && command -v tclsh >/dev/null 2>&1; then
    ## needs tcl; is also very intensive
    ## N.B. tcl needs __attribute__((packed)) epoll. with __GNUC__ it also needs -> weak attr <-
    V=1 make devtest
fi

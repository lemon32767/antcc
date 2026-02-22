#!/bin/sh

cd $(dirname "$0")/external/sqlite
if ! test -f ./configure; then
    echo 'sqlite submodule not pulled in!'
    exit 1
fi
set -e
CCACHE=none CC="$(realpath ../../../antcc)" ./configure
make clean
V=1 make
V=1 make devtest

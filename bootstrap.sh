#!/bin/env sh

set -euo pipefail
cc=gcc
cflags="-std=c11"
src=$(grep -o '\([_A-Za-z0-9/]\)\+\.c' < Makefile)
X() {
    echo "> $@" | sed -s 's/\([^ ]\+\.c \?\)\{10\}$/.../'
    $@
}
echo "== Stage 0 (compiling with $cc) =="
X $cc $cflags -o antcc0 $src
echo
echo "== Stage 1 (compiling with stage 0 output) =="
X ./antcc0 $cflags -o antcc1 $src
X md5sum antcc1
echo
echo "== Stage 2 (compiling with stage 1 output) =="
X ./antcc1 $cflags -o antcc2 $src
X md5sum antcc2

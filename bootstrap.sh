#!/bin/sh

set -eo pipefail

cc="$CC"
test -n "$cc" || cc=cc
opt="$O"
test -n "$opt" || opt=

cflags=$CFLAGS
test -n "$cflags" || : ${cflags:="-std=c11"}
if test -n "$V"; then
    opt="$opt -v"
fi
src=$(find src/ -name '*.c')
X() {
    echo "> $@" | (test -n "$V" && cat || sed 's/\([^ ]\+\.c \?\)\{10\}$/.../')
    $@
}
md5=md5sum
if ! $(command -v md5sum > /dev/null); then
    md5=md5
fi

echo "== Stage 0 (compiling with $cc) =="
X $cc $cflags -o antcc0 $src
echo
echo "== Stage 1 (compiling with stage 0 output) =="
X ./antcc0 $opt $cflags -o antcc1 $src
X $md5 antcc1
echo
echo "== Stage 2 (compiling with stage 1 output) =="
X ./antcc1 $opt $cflags -o antcc2 $src
X $md5 antcc2

(X cmp antcc1 antcc2) && echo ok. || (echo 'bootstrap FAIL!'; exit 1)

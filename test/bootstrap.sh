#!/bin/sh

set -e
cd $(dirname "$0")/..

cc="$CC"
test -n "$cc" || cc=cc
opt="$O"
test -n "$opt" || opt=

cflags=$CFLAGS
test -n "$cflags" || : ${cflags:="-std=c11"}
if test -n "$V"; then
    opt="$opt -v"
fi
X() {
    echo "> $@" | (test -n "$V" && cat || sed 's/\([^ ]\+\.c \?\)\{10\}$/.../')
    "$@"
}
chk="wc -c"
if which md5sum > /dev/null; then
    chk=md5sum
elif which md5 > /dev/null; then
    chk=md5
fi

make src/a_embedfilesdir.c || exit 1

src=$(find src/ -name '*.c')

mkdir -p build/
echo "== Stage 0 (compiling with $cc) =="
X $cc -w -o build/antcc0 $src
echo
echo "== Stage 1 (compiling with stage 0 output) =="
X build/antcc0 $opt $cflags -o build/antcc1 $src
X $chk build/antcc1
echo
echo "== Stage 2 (compiling with stage 1 output) =="
X build/antcc1 $opt $cflags -o build/antcc2 $src
X $chk build/antcc2

if X cmp build/antcc1 build/antcc2; then
    echo ok.
else
    echo 'bootstrap FAIL!'
    exit 1
fi

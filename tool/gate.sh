#!/bin/sh

export ANTCC_IRCHECK=1
cd "$(dirname "$0")"/..

mktgt=

for arg ; do
    case "$arg" in
        --san) mktgt=dbg ;;
        *) die 'usage: tool/gate.sh [--san]' ;;
    esac
done


set -xe

make clean clean-config
./configure
J=6
make $mktgt -j$J

for o in '' -O0 -O1 -O2; do
    CFLAGS=$o ./test/bootstrap.sh
done

./test/c/run.sh
./test/ir/run.sh
./test/external/lua.sh
./test/external/qbe.sh
./test/external/sqlite.sh test

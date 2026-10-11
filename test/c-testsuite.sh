#!/bin/env sh

wd="$(pwd)"
cd "$(dirname "$0")/external/c-testsuite"

arch=$(uname -m)
export CC=../../../antcc
export CFLAGS="-lm"

skip () {
    case "$(basename "$1")" in
    00204.c) ;; # nyi: va_arg for 2reg struct
    00206.c) ;; # pp: push/pop macro
    00210.c) ;; # c: attr in function type parsing issues
    00216.c) ;; # ext: struct cast, init flexible arr, range [a ... b] = ..
    *) return 1
    esac
}
set -- tests/single-exec/*.c
{
    echo "TAP version 13"
    echo "1..$#"
    for t in "$@"
    do
        if ! grep -qxE "portable|arch-$arch" "$t.tags" || skip "$t"
        then
            echo "ok $t # SKIP"
            continue
        fi

        result="ok"
        if ! timeout 5m ./runners/single-exec/posix "$t" > "tmpout" 2>&1
        then
            result="not ok"
        fi

        echo "$result" "$t"
        sed -e 's/^/#/' "tmpout"
    done
} | ./scripts/tapsummary > log.txt
head -n 5 log.txt
echo "... >> $(realpath --relative-to "$wd" log.txt)"

if head -n 5 log.txt | grep -q '^fail [1-9]'; then
    cat log.txt
    echo '(fail)'
    exit 1
fi
echo '(ok)'

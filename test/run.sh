#!/bin/sh

cd $(dirname "$0")
ANTCC="../antcc $CFLAGS"
ntest=0
npass=0
rc=0

x() {
    echo + "$@">>log.txt
    "$@" 2>> log.txt
}
run() {
    ntest=$(( ntest + 1 ))
    f="$(basename "$1")"
    expected=build/"$(echo "$f" | sed 's/\.c$/.expected/')"
    echo ----  "$f" ---- >> log.txt
    mkdir -p build/
    args=$(awk '/\/\* ARGS:.*$/ {ORS=" ";for (i=3;i<NF;++i)print $i;ORS="\n";print""}' "$f")
    cflags=$(awk '/\/\* CFLAGS:.*$/ {ORS=" ";for (i=3;i<NF;++i)print $i;ORS="\n";print""}' "$f")
    if awk '/\/\* EXPECT:$/ {x=k=any=1} x && /\*\// {x=0} x {if (!k)print $0;k=0} END{if(x||!any)exit 1;}' "$f" > "$expected"; then
        exe=build/"$(echo "$f" | sed 's/\.c$//')"
        if ! ( x $ANTCC $cflags "$f" -o "$exe" ); then
            echo !TEST ERROR "$f"
            echo !FAILED TO COMPILE
            echo '-------'
            rc=1
        else
            actual=build/"$(echo "$f" | sed 's/\.c$/.actual/')"
            x $QEMU "$exe" $args > "$actual"
            if ! cmp "$actual"  "$expected" > /dev/null; then
                echo --- !TEST ERROR "$f"
                diff --unified=0 "$expected" "$actual"
                echo '-------'
                rc=1
            else
                npass=$(( npass + 1 ))
            fi
        fi
    else
        echo --- !ignore "$f"
    fi
}

: < /dev/null > log.txt
tests=$(find . | grep -E '\./[0-9]+-.*\.c' | sort)
for test in $tests; do
    run $test
done

for arg ; do
	case "$arg" in
    --print) cat log.txt
    esac
done
echo TESTS PASSED: $npass/$ntest
printf 'wc log.txt;'
wc log.txt
exit $rc

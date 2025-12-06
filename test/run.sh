#!/bin/env sh

cd $(dirname "$0")
ANTCC=../antcc
ntest=0
npass=0

x() {
    echo X $@>>log.txt
    $@ 2>> log.txt
}
run() {
    ntest=$(expr $ntest + 1)
    f="$(basename "$1")"
    expected=build/"$(echo "$f" | sed -s 's/\.c$/.expected/')"
    echo ----  $f ---- >> log.txt
    mkdir -p build/
    args=$(awk '/\/\* ARGS:.*$/ {ORS=" ";for (i=3;i<NF;++i)print $i;ORS="\n";print""}' "$f")
    cflags=$(awk '/\/\* CFLAGS:.*$/ {ORS=" ";for (i=3;i<NF;++i)print $i;ORS="\n";print""}' "$f")
    awk '/\/\* EXPECT:$/ {x=k=any=1} x && /\*\// {x=0} x {if (!k)print $0;k=0} END{if(x||!any)exit 1;}' "$f" > "$expected"
    if [ $? == 0 ]; then
        obj=build/"$(echo "$f" | sed -s 's/\.c$/.o/')"
        exe=build/"$(echo "$f" | sed -s 's/\.c$//')"
        if ! ( x $ANTCC $cflags "$f" -c -o "$obj" && x $ANTCC $cflags "$obj" -o "$exe" ); then
            echo !TEST ERROR $f
            echo !FAILED TO COMPILE
            echo '-------'
        else
            actual=build/"$(echo "$f" | sed -s 's/\.c$/.actual/')"
            x "$exe" $args > "$actual"
            if [ "$(md5sum < "$actual")" != "$(md5sum < "$expected")" ]; then
                echo --- !TEST ERROR $f
                diff --unified=0 --color=auto "$expected" "$actual"
                echo '-------'
            else
                npass=$(expr $npass + 1)
            fi
        fi
    else
        echo --- !ignore $f
    fi
}

< /dev/null > log.txt
tests=$(find . -regex '\./[0-9]+-.*\.c' | sort)
for test in $tests; do
    run $test
done

echo TESTS PASSED: $npass/$ntest
printf 'wc log.txt;'
wc log.txt

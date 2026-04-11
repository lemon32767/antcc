#!/bin/sh
csmith_path=/usr/include/csmith-2.3.0/
cd $(dirname "$0")

WORKDIR=csmith
mkdir -p "$WORKDIR"

REFCC=${REFCC:-gcc}
N=${1:-100}
npass=0
nfail=0
ntimeout=0
ncomperr=0
pid=$$

TIMEOUT=${TIMEOUT:-"2"}
CFLAGS=${CFLAGS:-""}

echo "($pid) Running $N tests"
echo "Using REFCC=$REFCC"
echo "antcc CFLAGS=$CFLAGS"
echo "timeout=$TIMEOUT"s

run_test() {
    num=$1
    tmp1="$WORKDIR/$pid-$num-antcc"
    tmp2="$WORKDIR/$pid-$num-ref"
    out1="$WORKDIR/$pid-$num-antcc.out"
    out2="$WORKDIR/$pid-$num-ref.out"
    src="$WORKDIR/$pid-$num.c"
    
    csmith --no-packed-struct > "$src" 2>/dev/null || return 1
    
    if ! prlimit --as=1073741824 ../antcc $CFLAGS -I"$csmith_path" -w "$src" -o "$tmp1" -lm; then
        ncomperr=$((ncomperr+1))
        echo "FAIL (compile): test $num"
        mv "$src" "$WORKDIR/$pid-fail_compile_$num.c"
        rm -f "$tmp1" "$tmp2" "$out1" "$out2"
        return 1
    fi
    
    if ! $REFCC -I"$csmith_path" -w "$src" -o "$tmp2" -lm ; then
        echo "FAIL (ref compile)"
        rm -f "$tmp1" "$tmp2" "$out1" "$out2" "$src"
        exit 1
    fi
    
    timeout $TIMEOUT "$tmp1" > "$out1" 2>&1
    st1=$?
    
    timeout $TIMEOUT "$tmp2" > "$out2" 2>&1
    st2=$?
    
    if [ $st1 -eq 124 ] || [ $st2 -eq 124 ]; then
        # Timeout - likely infinite loop
        ntimeout=$((ntimeout+1))
    elif [ $st1 -ne $st2 ]; then
        # Different exit codes
        echo ""
        echo "FAIL (exit code): test $num"
        echo "  antcc: $st1, gcc: $st2"
        mv "$src" "$WORKDIR/$pid-fail_exit_$num.c"
        nfail=$((nfail+1))
    elif ! diff -q -w "$out1" "$out2" >/dev/null 2>&1; then
        # Different output
        echo ""
        echo "FAIL (output): test $num"
        diff "$out1" "$out2"
        mv "$src" "$WORKDIR/$pid-fail_output_$num.c"
        nfail=$((nfail+1))
    else
        npass=$((npass+1))
        printf "."
    fi
    
    rm -f "$tmp1" "$tmp2" "$out1" "$out2" "$src"
    return 0
}

set -e

i=0
while [ $i -lt $N ]; do
    run_test $i || true
    i=$((i+1))
done

echo ""
echo "Results: $npass passed, $nfail failed, $ntimeout timeouts, $ncomperr compile errors"
find "$WORKDIR"/ | grep "$pid" && echo

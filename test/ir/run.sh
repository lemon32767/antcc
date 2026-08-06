#!/bin/sh

cd "$(dirname "$0")"
nfail=0
ntest=0
rc=0
for f in *.test; do
    if tclsh "$f"; then
        printf .
    else
        nfail=$(( nfail + 1 ))
        rc=1
    fi
    ntest=$(( ntest + 1 ))
done
echo
if [ $rc = 0 ]; then
    echo "OK ($ntest tests)"
else
    echo "FAIL ($nfail out of $ntest tests failed)"
fi
exit $rc

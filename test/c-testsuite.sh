#!/bin/env sh

wd="$(pwd)"
cd $(dirname "$0")/external/c-testsuite
test -d .tmsu/ || (./scripts/make-search-index && ./single-exec antcc-x86_64 2>/dev/null >/dev/null)
./single-exec antcc-x86_64 | ./scripts/tapsummary > log.txt
head -n 5 log.txt
echo "... >> $(realpath --relative-to "$wd" log.txt)"

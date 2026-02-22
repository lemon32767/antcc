#!/bin/env sh

cd $(dirname "$0")/external/c-testsuite
test -d .tmsu/ || (./scripts/make-search-index && ./single-exec antcc-x86_64 2>/dev/null >/dev/null)
./single-exec antcc-x86_64 | ./scripts/tapsummary | tee log.txt | head -n 5
echo '... >> log.txt'

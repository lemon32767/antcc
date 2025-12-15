#!/bin/env sh

cd $(dirname "$0")/external/c-testsuite
./single-exec antcc-x86_64 | ./scripts/tapsummary | tee log.txt | head -n 5
echo '... >> log.txt'

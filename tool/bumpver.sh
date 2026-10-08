#!/bin/sh

cd "$(dirname "$0")"/..

if [ -n "$(git status --porcelain --untracked-files=no 2)" ]; then
    echo dirty work tree!
    exit 1
fi

version=$1
if [ -z "$version" ]; then
    version=$(cat VERSION)
    major=$(echo "$version" | cut -d. -f1)
    minor=$(echo "$version" | cut -d. -f2)
    patch=$(expr $(echo "$version" | cut -d. -f3) + 1)
    version="$major"."$minor"."$patch"
fi
if ! echo "$version" | grep -q '^[0-9]\+\.[0-9]\+\.[0-9]\+$'; then
    echo bad version "$version"!
    exit 1
fi
tag=v$version
if git describe --tags $tag 2>/dev/null; then
    echo already exists!
    exit 1
fi

set -xe
echo $version > VERSION
git add VERSION
git commit -m $version
git tag "$tag"
echo ok

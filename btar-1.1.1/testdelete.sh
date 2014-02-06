#!/bin/sh -e

if [ -z "$IKNOW" ]; then
    echo Set the environment var IKNOW to run this. This script my delete your files.
    exit 1
fi

set -x

rm -Rf test

mkdir test
pushd test
touch a
touch b
touch c
mkdir d
pushd d
touch e
touch f
popd
mkdir g
pushd g
touch h
touch i
popd
popd

./btar -c -F gzip -f test.btar test
rm test/b
rm test/d/e
rm -R test/g

./btar -c -F gzip -d test.btar -f test2.btar test

./btar -H -x -v -f test.btar -f test2.btar

echo '*** All files set to be deleted:'
tar xf test2.btar -O deleted.tar.gzip | gunzip | tar t

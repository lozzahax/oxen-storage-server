#!/bin/bash

bash -i

set -e
set -x

#curl -L https://dl.bintray.com/boostorg/release/1.74.0/source/boost_1_74_0.tar.bz2 | tar xj
#curl -L https://deb.imaginary.stream/boost_1_72_0.tar.bz2 | tar xj

#curl -L https://builds.lokinet.dev/deps/boost_1_75_0.tar.bz2 | tar xj
curl -L https://boostorg.jfrog.io/artifactory/main/release/1.76.0/source/boost_1_76_0.tar.bz2 | tar xj

cd boost_1_76_0

export CC=gcc-8 CXX=g++-8

./bootstrap.sh

./b2 -a --prefix=${PWD}/../boost link=static variant=release install \
        --with-program_options \
        --lto



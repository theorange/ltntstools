#!/bin/bash

set -e
# autoconf2.7x
PREFIX=${PREFIX:-$PWD/deps/target-root/usr}

if [ ! -d deps/target-root ]; then
  git submodule update --init --recursive
  pushd deps
  ./build.sh
  popd
fi

withntt=$(test -d deps/libntt && echo yes || echo no)
withdtapi=$(test -d deps/LinuxSDK && echo yes || echo no)

export CFLAGS="-I${PREFIX}/include -I${PWD}/deps/ffmpeg"
export LDFLAGS="-L${PREFIX}/lib -L${PREFIX}/lib64"
test -f configure || ./autogen.sh --build
./configure --prefix=${PREFIX} --enable-shared=yes --enable-ntt=$withntt --enable-dtapi=$withdtapi
make -j$JOBS V=1
make install
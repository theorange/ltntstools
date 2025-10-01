#!/bin/bash

set -e

PREFIX=${PREFIX:-$PWD/deps/target-root/usr}

withntt=$(test -d deps/libntt && echo yes || echo no)
withdtapi=$(test -d sdk-dektec/LinuxSDK && echo yes || echo no)

export CFLAGS="-I${PREFIX}/include -I${PWD}/deps/ffmpeg"
export LDFLAGS="-L${PREFIX}/lib -L${PREFIX}/lib64"
test -f configure || ./autogen.sh --build
./configure --prefix=${PREFIX} --enable-shared=no --enable-ntt=$withntt --enable-dtapi=$withdtapi
make -j$JOBS V=1
make install
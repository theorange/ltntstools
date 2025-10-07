#!/bin/bash -ex

set -e

PREFIX=${PREFIX:-${PWD}/target-root/usr}

export CFLAGS="-I${PREFIX}/include $NIELSEN_INC"
export CPPFLAGS="-I${PREFIX}/include $NIELSEN_INC"
export LDFLAGS="-L${PREFIX}/lib -L${PREFIX}/lib64 $NIELSEN_LIB"
export PKG_CONFIG_PATH="${PREFIX}/lib64/pkgconfig"

# Unpack the DekTec SDK
if [ ! -d LinuxSDK ]; then
  version=v2025.09.0
  test -f LinuxSDK_${version}.tar.gz || curl -LO https://www.dektec.com/products/SDK/DTAPI/Downloads/LinuxSDK_${version}.tar.gz
	tar zxf LinuxSDK_${version}.tar.gz	
fi

# Unpack the nielsen SDK, if it's available.
NIELSEN_SDK=/storage/dev/NIELSEN/DecoderSdkMonitor_v1.4_Linux.tgz
if [ -f $NIELSEN_SDK ]; then
	if [ ! -d sdk-nielsen ]; then
		mkdir -p sdk-nielsen
		cd sdk-nielsen
		tar zxvf $NIELSEN_SDK
		cd ..
	fi
	NIELSEN_INC="-I$PWD/sdk-nielsen/package/include"
	NIELSEN_LIB="-L$PWD/sdk-nielsen/package/lib"
  export CFLAGS="${CFLAGS} $NIELSEN_INC"
  export CPPFLAGS="${CPPFLAGS} $NIELSEN_INC"
  export LDFLAGS="${LDFLAGS} $NIELSEN_LIB"
fi


pushd json-c	
	./autogen.sh
	./configure --prefix=${PREFIX} --enable-shared=yes
	make -j$JOBS
	make install
popd

#
# pushd zvbi
# 	./autogen.sh
# 	./configure --prefix=${PREFIX} --enable-shared=yes
#	make -j$JOBS
#	make install
# popd

pushd srt
	./configure --enable-static=no --enable-shared=yes --prefix=${PREFIX}
	make -j8
	make install
popd

pushd MediaInfoLib/Project/GNU/Library
	./autogen.sh
	./configure --enable-shared=yes --enable-static=yes --prefix=${PREFIX}
	make -j$JOBS
	make install
popd

pushd bitstream
	make PREFIX=${PREFIX}
	make PREFIX=${PREFIX} install
popd

pushd libdvbpsi
  sed -i -e "/for v in 15 14 13 12 11 10 9 8 7 6 5; do/s|15|16 15|" bootstrap
	./bootstrap
	./configure --prefix=${PREFIX} --enable-shared=yes
	make -j$JOBS
	make install
popd

pushd libklvanc
	./autogen.sh --build
	./configure --prefix=${PREFIX} --enable-shared=yes
	make -j$JOBS
	make install
popd

pushd libklscte35
	./autogen.sh --build
	./configure --prefix=${PREFIX} --enable-shared=yes
	make -j$JOBS
	make install
popd

git clone --branch main git@git.ltnglobal.com:video/libntt.git || :
if [ -d libntt ]; then
  pushd libntt
    ./autogen.sh --build
    ./configure --prefix=${PREFIX} --enable-shared=yes
    make -j$JOBS
    make install
  popd
fi

pushd ffmpeg
#	export LDFLAGS="$LDFLAGS -lcrypto -lm -lsrt"
	./configure --prefix=${PREFIX} --disable-iconv --enable-shared \
		--disable-audiotoolbox --disable-videotoolbox --disable-avfoundation \
		--enable-libsrt
	make -j$JOBS
	make install
popd

pushd libltntstools
	./autogen.sh --build
	./configure --prefix=${PREFIX} --enable-shared=yes
	make -j$JOBS
	make install
popd
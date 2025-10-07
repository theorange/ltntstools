#!/bin/bash

set -ev

APP=ltntstools
SPECFILE=$APP.spec

rm -rf ~/rpmbuild

which rpmdev-setuptree >/dev/null 2>&1
if [ $? -ne 0 ]; then
	echo "Aborting, please install rpm dev tools with:"
	echo "     sudo yum -y install rpmdevtools rpmlint"
	exit 1
fi
rpmdev-setuptree

GIT_VERSION=`git describe --abbrev=8 | sed 's!-.*!!g'`

cat $SPECFILE  | sed "s/^Version.*$/Version:\t${GIT_VERSION}/g" > ~/rpmbuild/SPECS/$SPECFILE

DESTDIR=~/rpmbuild/BUILDROOT/$APP-$GIT_VERSION-1.x86_64

prefix=/usr
bindir=${prefix}/bin
libdir=${prefix}/lib64

mkdir -p $DESTDIR/${prefix}/{bin,lib64}
cp ../src/tstools_util $DESTDIR${bindir}
# strip $DESTDIR/usr/bin/tstools_util # rpm should do this for you

mkdir -p $DESTDIR${prefix}/share/man/man8 
cp ../man/*.8 $DESTDIR${prefix}/share/man/man8

mkdir -p $DESTDIR${libdir}/ltntstools/
cp ../deps/target-root/usr/lib/libdvbpsi.so.10    $DESTDIR${libdir}/ltntstools/libdvbpsi.so.10
cp ../deps/target-root/usr/lib/libklscte35.so.0   $DESTDIR${libdir}/ltntstools/libklscte35.so.0
cp ../deps/target-root/usr/lib/libltntstools.so.0 $DESTDIR${libdir}/ltntstools/libltntstools.so.0
cp ../deps/target-root/usr/lib64/libsrt.so.1.5    $DESTDIR${libdir}/ltntstools/libsrt.so.1.5
cp ../deps/target-root/usr/lib/libjson-c.so.4     $DESTDIR${libdir}/ltntstools/libjson-c.so.4
# cp ../deps/target-root/usr/lib/libzvbi.so.0       $DESTDIR${libdir}/ltntstools/libzvbi.so.0
cp ../deps/target-root/usr/lib/libklvanc.so.0     $DESTDIR${libdir}/ltntstools/libklvanc.so.0
cp ../deps/target-root/usr/lib/libavformat.so.58  $DESTDIR${libdir}/ltntstools/libavformat.so.58
cp ../deps/target-root/usr/lib/libavutil.so.56    $DESTDIR${libdir}/ltntstools/libavutil.so.56
cp ../deps/target-root/usr/lib/libavcodec.so.58   $DESTDIR${libdir}/ltntstools/libavcodec.so.58
cp ../deps/target-root/usr/lib/libswresample.so.3 $DESTDIR${libdir}/ltntstools/libswresample.so.3
cp ../deps/target-root/usr/lib/libswscale.so.5    $DESTDIR${libdir}/ltntstools/libswscale.so.5
if [ -f ../deps/target-root/usr/lib/libntt.so.0 ]; then
	cp ../deps/target-root/usr/lib/libntt.so.0        $DESTDIR${libdir}/ltntstools/libntt.so.0
fi

pushd $DESTDIR${bindir}
	for BIN in `./tstools_util | grep ^tstools`
	do
		ln -sf tstools_util $BIN
	done
popd

rpmbuild -bb ~/rpmbuild/SPECS/$SPECFILE

mv ~/rpmbuild/RPMS/x86_64/$APP-$GIT_VERSION-1.x86_64.rpm .

# Test the RPM install on a clean centos system.
# We have a dep on libpcap, ensure yum finds the dep and installs it automatically for us.
# yum --nogpgcheck localinstall ltntstools-v1.0.1-1.x86_64.rpm

# Extract the change log rpm -qp --changelog ~/rpmbuild/RPMS/x86_64/$APP-$GIT_VERSION-1.x86_64.rpm

#cp $APP-$GIT_VERSION-1.x86_64.rpm docker

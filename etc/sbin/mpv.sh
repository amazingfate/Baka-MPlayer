#!/bin/bash

# mpv: https://github.com/mpv-player/mpv

arch=x86_64
if [[ $1 == 'x86_64' ]]; then
	arch=x86_64
elif [[ $1 == 'i686' ]]; then
	arch=i686
else
	echo "Please specify either x86_64 or i686 architecture.";
	exit;
fi

# add built mxe directory to the path
root=$(pwd)
export PATH=$root/mxe.$arch/usr/bin/:$PATH

# get mpv/waf
git clone https://github.com/mpv-player/mpv.git mpv.$arch
cd mpv.$arch
./bootstrap.py
DEST_OS=win32 TARGET=$arch-w64-mingw32.static ./waf configure --enable-libmpv-shared --enable-static-build --disable-client-api-examples
./waf build

# after building successfully we need to copy the files into the correct locations in mxe
instroot=$root/mxe.$arch/usr/$arch-w64-mingw32.static
# copy the static library over
cp build/*.a $instroot/lib
mkdir -p $instroot/include/mpv
# copy the include file over
cp libmpv/client.h $instroot/include/mpv/
# modify and write the mpv pkgconfig file
cat build/libmpv/mpv.pc | 
	sed "s,^prefix=.*$,prefix=${instroot},g" |
	sed "s,^exec_prefix=.*$,exec_prefix=\${prefix},g" |
	sed "s,^libdir=.*$,libdir=\${prefix}/lib,g" |
	sed "s,^includedir=.*$,includedir=\${prefix}/include,g" > $instroot/lib/pkgbuild/mpv.pc

cd ..

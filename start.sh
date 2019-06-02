#!/bin/sh
git submodule init
git submodule update
if [ ! -d "build/libevent" ]; then
	mkdir -p build/libevent
	cd build/libevent
	cmake ../../libevent \
		-DCMAKE_BUILD_TYPE=Release \
		-DEVENT__LIBRARY_TYPE=STATIC \
		-DEVENT__DISABLE_DEBUG_MODE=ON \
		-DEVENT__ENABLE_VERBOSE_DEBUG=OFF \
		-DEVENT__DISABLE_THREAD_SUPPORT=OFF \
		-DEVENT__DISABLE_OPENSSL=ON \
		-DEVENT__DISABLE_BENCHMARK=ON \
		-DEVENT__DISABLE_TESTS=ON \
		-DEVENT__DISABLE_REGRESS=ON \
		-DEVENT__DISABLE_SAMPLES=ON \
		-DEVENT__DISABLE_CLOCK_GETTIME=OFF \
		-DEVENT__DISABLE_GCC_WARNINGS=ON \
		-DEVENT__ENABLE_GCC_HARDENING=ON \
		-DEVENT__ENABLE_GCC_WARNINGS=OFF \
		-DCMAKE_SKIP_RPATH=YES \
		-DCMAKE_C_FLAGS="-Ofast -mtune=native -march=native -flto" \
		-DCMAKE_AR="$(whereis -b gcc-ar | cut -d\  -f2)" \
		-DCMAKE_RANLIB="$(whereis -b gcc-ranlib | cut -d\  -f2)" \
		-DCMAKE_INSTALL_PREFIX="../local"
	cd ../..
fi

cmake --build build/libevent
cmake --build build/libevent --target install
export PKG_CONFIG_PATH=$PWD/build/local/lib/pkgconfig/

if [ ! -d "build/pixelflood" ]; then
	mkdir -p build/pixelflood
	cd build/pixelflood
	cmake ../.. -DCMAKE_BUILD_TYPE=Release
	cd ../..
fi

cmake --build build/pixelflood
export MESA_NO_ERROR=1
#export SDL_VIDEODRIVER=kmsdrm
#/usr/bin/valgrind --tool=drd \
#strace -c \
#operf --callgraph --append \
#/usr/bin/valgrind --tool=cachegrind \
#perf stat -a \
#perf record --call-graph=dwarf \
build/pixelflood/pixelflood

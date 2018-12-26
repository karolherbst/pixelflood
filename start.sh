#!/bin/sh
if [ ! -d "build" ]; then
	mkdir build
	cd build
	cmake .. -DCMAKE_BUILD_TYPE=Release
	cd ..
fi

cmake --build build
export MESA_NO_ERROR=1
#export SDL_VIDEODRIVER=kmsdrm
#/usr/bin/valgrind --tool=drd \
#strace -c \
#operf --callgraph --append \
#/usr/bin/valgrind --tool=cachegrind \
#perf stat -a \
#perf record --call-graph=dwarf \
build/pixelflood

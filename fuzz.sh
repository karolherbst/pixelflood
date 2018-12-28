#!/bin/sh
export AS=afl-as
export CC=afl-clang-fast

AFL_INST_RATIO=100
AFL_HARDEN=1

if [ ! -d "build_fuzzing" ]; then
	mkdir build_fuzzing
	cd build_fuzzing
	cmake .. -DCMAKE_BUILD_TYPE=Release
	cd ..
fi

cmake --build build_fuzzing
export MESA_NO_ERROR=1

rm -rf fuzzing_output

afl-fuzz -i fuzzing_files/ -o fuzzing_output/ -M master0 build_fuzzing/pixelflood fuzz @@

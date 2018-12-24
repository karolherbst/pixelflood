#!/bin/sh
ninja -C build
#export SDL_VIDEODRIVER=kmsdrm
#/usr/bin/valgrind --tool=drd \
#strace -c \
#operf --callgraph --append \
#/usr/bin/valgrind --tool=cachegrind \
#perf stat -a \
perf record --call-graph=dwarf \
build/pixelflood

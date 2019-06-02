cmake_minimum_required(VERSION 3.0)

project(pixelflood C)

set(CMAKE_C_STANDARD_REQUIRED 11)

set(CMAKE_C_FLAGS "-fPIE -flto -mtune=native -march=native -fwhole-program -g -ggdb3 -Wall -fstack-protector-strong -fprefetch-loop-arrays -D_FORTIFY_SOURCE=2")
set(CMAKE_C_FLAGS_DEBUG "-O0 -fno-inline")
set(CMAKE_C_FLAGS_RELEASE "-Ofast -finline -funsafe-loop-optimizations -fomit-frame-pointer")

set(CMAKE_C_LINK_FLAGS "-pie -flto -Wl,-z,defs -Wl,-z,now -Wl,--as-needed")

set(CMAKE_THREAD_PREFER_PTHREAD YES)

include(FindPkgConfig)

find_package(Threads REQUIRED)
pkg_check_modules(SDL2 sdl2)
pkg_check_modules(SDL2_TTF SDL2_ttf)
pkg_check_modules(LIBEVENT_CORE libevent_core)
pkg_check_modules(LIBEVENT_PTHREADS libevent_pthreads)
pkg_check_modules(FONTCONFIG fontconfig)

add_executable(pixelflood
  main.c
)

target_include_directories(pixelflood
  PRIVATE
    ${FONTCONFIG_INCLUDE_DIRS}
    ${LIBEVENT_CORE_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
)

target_link_libraries(pixelflood
  ${FONTCONFIG_LIBRARIES}
  ${LIBEVENT_CORE_LINK_LIBRARIES}
  ${LIBEVENT_PTHREADS_LINK_LIBRARIES}
  ${SDL2_LIBRARIES}
  ${SDL2_TTF_LIBRARIES}
  Threads::Threads
)

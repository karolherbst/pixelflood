cmake_minimum_required(VERSION 3.0)

set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_STANDARD 11)

set(CMAKE_THREAD_PREFER_PTHREAD YES)

include(FindPkgConfig)

find_package(Threads REQUIRED)
find_package(SDL2 REQUIRED)
pkg_check_modules(SDL2_TTF SDL2_ttf)

add_executable(pixelflood
  main.c
)

target_include_directories(pixelflood
  PRIVATE
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
)

target_link_libraries(pixelflood
  ${SDL2_LIBRARIES}
  ${SDL2_TTF_LIBRARIES}
  Threads::Threads
)

cmake_minimum_required(VERSION 3.20)
project(frame_replay CXX)

# The host side of azahar's sgxd frame captures: its views of a capture and of the replay
# player's results (docs/debugging.md). Built on its own:
#   cmake -S tools/frame_replay -B build-frame-replay -G Ninja && ninja -C build-frame-replay
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 REQUIRED COMPONENTS Gui)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

add_executable(frame-replay
  src/cli.cpp
  src/capture.cpp src/capture.h
  src/pica.cpp src/pica.h
  src/replay.cpp src/replay.h
  src/session.cpp src/session.h)
target_compile_options(frame-replay PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(frame-replay PRIVATE Qt6::Gui PkgConfig::ZSTD)

# pica_cg: a host tool, built on its own and never part of azahar's build.
#   cmake -S tools/pica_cg -B build-pica-cg && cmake --build build-pica-cg
cmake_minimum_required(VERSION 3.20)
project(pica_cg CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AZAHAR ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(SRC ${AZAHAR}/src)
set(EXT ${AZAHAR}/externals)

add_executable(pica_cg
    main.cpp
    cg_fs_shader_gen.cpp
    cg_vs_shader_gen.cpp
    ${SRC}/common/cityhash.cpp
    ${SRC}/video_core/pica/shader_setup.cpp
    ${SRC}/video_core/shader/generator/pica_fs_config.cpp
    ${SRC}/video_core/shader/generator/shader_gen.cpp
)
# The shims come first: they shadow azahar's logging backend and settings.
target_include_directories(pica_cg PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${SRC}
    ${EXT}/fmt/include
    ${EXT}/xxHash
    ${EXT}/boost
    ${EXT}/nihstro/include
)
target_compile_definitions(pica_cg PRIVATE FMT_HEADER_ONLY XXH_INLINE_ALL)

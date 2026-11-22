# Building

Linux with a VitaSDK, clang 21 (`clang-21`, `clang++-21`, `llvm-ar-21`), lld and Ninja. Not
clang 22 or newer: it miscompiles NEON code for 32-bit ARM.

```sh
git submodule update --init --recursive
export VITASDK=/usr/local/vitasdk
TC="-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=CMakeModules/toolchain-vita-clang.cmake"

# Release: what players run, with the checked-in PGO profile
cmake -S . -B cmake-build-release $TC
ninja -C cmake-build-release src/citra_vita/citra_vita.vpk-vpk

# Release with the development tools and line tables, without PGO: speeds and CPU profiles
cmake -S . -B cmake-build-release-debug $TC -DENABLE_VITA_DEBUG_TOOLS=ON \
    -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG -gline-tables-only" \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -gline-tables-only"
ninja -C cmake-build-release-debug src/citra_vita/citra_vita.vpk-vpk

# Diagnostics: debug asserts and sgxd's switches, no LTO; also sgxd_test and the replay player
cmake -S . -B cmake-build-release-diagnostics $TC -DENABLE_LTO=OFF
ninja -C cmake-build-release-diagnostics src/citra_vita/citra_vita.vpk-vpk
ninja -C cmake-build-release-diagnostics sgxd_test.vpk-vpk
ninja -C cmake-build-release-diagnostics citra_vita_player.vpk-vpk
```

The vpk is `src/citra_vita/citra_vita.vpk` in the build tree, beside the unstripped ELF
`citra_vita` that crash reports and profiles are read against. Add debug information through
`CMAKE_{C,CXX}_FLAGS_RELEASE` only: `CMAKE_{C,CXX}_FLAGS` holds the toolchain's own flags, and
replacing them builds an eboot that hangs at start.

## A new PGO profile

```sh
# LLVM_SRC: an llvm-project tree of the clang's release
tools/pgo/build-profile-rt.sh LLVM_SRC libclang_rt.profile-vita.a
cmake -S . -B cmake-build-pgo-gen $TC -DAZAHAR_PGO=GENERATE \
    -DAZAHAR_PGO_RUNTIME=$PWD/libclang_rt.profile-vita.a
ninja -C cmake-build-pgo-gen src/citra_vita/citra_vita.vpk-vpk
# play the training scenes; `pgo PATH` on the command channel writes the counts
llvm-profdata-21 merge -o azahar.profdata *.profraw
tar -cJf src/citra_vita/pgo/azahar.profdata.tar.xz azahar.profdata
```

## Host tools

```sh
# frame_replay (Qt 6, libzstd)
cmake -S tools/frame_replay -B build-frame-replay -G Ninja && ninja -C build-frame-replay
# the Z3DS web compressor's zstd.wasm; serve tools/z3ds-web/site over HTTP(S)
CC=clang-21 tools/z3ds-web/build.sh
```

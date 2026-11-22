# azaharnative, the kernel plugin (docs/native-execution.md), built in the emulator's build with its
# clang and lld and carried by the vpk (src/citra_vita). It exposes one target to the rest of the
# build, azaharnative_stub: the weak import stubs the app links, the ABI headers, and the plugin's
# path (AZAHAR_NATIVE_MODULE) for the vpk.

# A kernel plugin shares none of the emulator's compile setup: this directory starts from nothing
# (no inherited options, definitions, include directories or LTO) and sets every flag itself.
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
set_property(DIRECTORY PROPERTY COMPILE_DEFINITIONS "")
set_property(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "")
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
# vita_create_self finds the ELF beside it, not in the project's bin/.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
foreach(lang C ASM)
    set(CMAKE_${lang}_FLAGS "")
    foreach(config RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
        set(CMAKE_${lang}_FLAGS_${config} "")
    endforeach()
endforeach()
set(CMAKE_EXE_LINKER_FLAGS "")
set(CMAKE_C_STANDARD_LIBRARIES "")
foreach(config RELEASE DEBUG RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_EXE_LINKER_FLAGS_${config} "")
endforeach()

add_executable(azaharnative src/main.c src/entry.S src/aeabi_mem.c)
set_source_files_properties(src/aeabi_mem.c PROPERTIES COMPILE_OPTIONS -fno-builtin)
target_compile_definitions(azaharnative PRIVATE __vita__=1 __VITA__=1)
# The kernel saves no VFP/NEON state for a plugin. Its syscalls could use the registers a call may
# clobber, but its interrupt handler, kernel threads and exception vector run with someone else's
# live, so the compiler must never choose VFP/NEON by itself: no vectorising, no NEON block copies
# (-mno-implicit-float). check_vfp.py below holds every link to that. No unwind tables: nothing
# unwinds a plugin, and they would want a personality routine. -fno-optimize-sibling-calls is kept
# from the module's first build, which recorded no reason.
target_compile_options(azaharnative PRIVATE
    -mcpu=cortex-a9 -mthumb -mfpu=neon -fno-pic -fno-pie
    -O2 -Wall
    -fno-vectorize -fno-slp-vectorize -mno-implicit-float
    -fno-optimize-sibling-calls
    -fno-unwind-tables -fno-asynchronous-unwind-tables)
# The app's linker script (vitasdk's armelf.xc, fitted to lld and vita-elf-create) without its
# start files; Sony's kernel libraries are the only runtime. A kernel module is always relocated
# when it loads, and lld's long-branch thunks otherwise load absolute addresses that -q leaves no
# relocation for (src/sgxd_test/CMakeLists.txt): any thunk is position-independent.
target_link_options(azaharnative PRIVATE
    -fuse-ld=lld${VITA_CLANG_SUFFIX} -nostdlib
    -Wl,-q -Wl,-z,nocopyreloc -Wl,-z,norelro -Wl,-z,max-page-size=4096 -Wl,--no-eh-frame-hdr
    -Wl,--no-rosegment -Wl,--pic-veneer
    -T ${CMAKE_SOURCE_DIR}/src/citra_vita/ldscript.ld)
target_link_directories(azaharnative PRIVATE ${VITA_SYSROOT}/lib)
target_link_libraries(azaharnative PRIVATE
    taihenForKernel_stub
    taihenModuleUtils_stub
    SceCpuForDriver_stub
    SceIofilemgrForDriver_stub
    SceProcessmgrForDriver_stub
    SceProcEventForDriver_stub
    SceSysclibForDriver_stub
    SceSysmemForDriver_stub
    SceSysrootForDriver_stub
    SceThreadmgrForDriver_stub
    SceDebugForDriver_stub
    SceKernelSuspendForDriver_stub
    SceIntrmgrForDriver_stub
    SceCtrlForDriver_stub
    SceRegMgrForDriver_stub
    SceSysconForDriver_stub
    SceUdcdForDriver_stub
)
# Deliberately NOT linked: SceProcessmgrForKernel_stub and SceSysmemForKernel_stub. The
# *ForKernel libraries are re-versioned every firmware - SceProcessmgrForKernel is library
# 0x7A69DE86 on 3.60 and 0xEB1F8EF7 on 3.63+ - so a link-time import of the wrong one makes the
# whole module fail to load with "Library not found". Anything needed from them is resolved at
# runtime instead.

# The VFP/NEON rule, checked on every link: only the functions that work on VFP state on purpose
# (the guest's, saved and restored around a slice, or emulated) may hold VFP/NEON instructions.
add_custom_command(TARGET azaharnative POST_BUILD
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/check_vfp.py
            llvm-objdump${VITA_CLANG_SUFFIX} $<TARGET_FILE:azaharnative>
    VERBATIM)

include("${VITASDK}/share/vita.cmake" REQUIRED)
vita_create_self(azaharnative.skprx azaharnative CONFIG exports.yml UNSAFE)

# The app's import stubs, from the module and its exports: rebuilt when either changes (vitasdk's
# vita_create_stubs builds them on every build and copies them each time, which would relink the
# app every time).
set(STUB_DIR ${CMAKE_CURRENT_BINARY_DIR}/stubs)
set(STUB_WEAK ${STUB_DIR}/libazaharnative_stub_weak.a)
add_custom_command(OUTPUT ${STUB_WEAK}
    COMMAND ${VITA_ELF_EXPORT} kernel $<TARGET_FILE:azaharnative>
            ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml ${CMAKE_CURRENT_BINARY_DIR}/stubs.yml
    COMMAND ${VITA_LIBS_GEN} ${CMAKE_CURRENT_BINARY_DIR}/stubs.yml ${STUB_DIR}
    COMMAND make -s -C ${STUB_DIR}
    DEPENDS azaharnative ${CMAKE_CURRENT_SOURCE_DIR}/exports.yml
    COMMENT "Building azaharnative's import stubs"
    VERBATIM)
add_custom_target(azaharnative_stubs DEPENDS ${STUB_WEAK})

# What the rest of the build uses. The weak stub, because the app starts before the module is
# loaded: it loads the module itself (LoadAzaharNative in citra_vita_main.cpp), and a strong
# import of a library that is not there yet stops the loader from starting the app at all.
add_library(azaharnative_stub INTERFACE)
target_link_libraries(azaharnative_stub INTERFACE ${STUB_WEAK})
target_include_directories(azaharnative_stub INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_dependencies(azaharnative_stub azaharnative_stubs azaharnative.skprx-self)
set_target_properties(azaharnative_stub PROPERTIES
    AZAHAR_NATIVE_MODULE ${CMAKE_CURRENT_BINARY_DIR}/azaharnative.skprx)

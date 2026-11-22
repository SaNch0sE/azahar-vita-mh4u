# vita_create_self / vita_create_vpk live here, not in the toolchain file.
include("${VITASDK}/share/vita.cmake" REQUIRED)

# The presentation shaders, embedded as byte arrays. The .gxp are vendored - compiled offline
# by the SDK's psp2cgc (shaders/compile.sh) from the .cg next to them - so the console needs no
# runtime Cg compiler to present, and a machine without docker still builds the vpk.
set(GXM_SHADER_HEADER "${CMAKE_CURRENT_BINARY_DIR}/gxm_shaders.gen.h")
file(GLOB GXM_SHADER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.gxp")
add_custom_command(
    OUTPUT "${GXM_SHADER_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/shaders
        -DOUT=${GXM_SHADER_HEADER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/shaders/EmbedShaders.cmake
    DEPENDS ${GXM_SHADER_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/EmbedShaders.cmake"
    COMMENT "Embedding GXM shader programs")

set(CITRA_VITA_SOURCES
    ../vita_compat/src/vita_libc_shims.c
    citra_vita_main.cpp
    crash_report.cpp
    crash_report.h
    emu_window_vita.cpp
    emu_window_vita.h
    gxm_present.cpp
    gxm_present.h
    imgui_impl_sgxd.cpp
    imgui_impl_sgxd.h
    imgui_impl_vita.cpp
    imgui_impl_vita.h
    input_factory_vita.cpp
    input_factory_vita.h
    sgxd_present.cpp
    sgxd_present.h
    stats_hud.cpp
    stats_hud.h
    vita_ui.cpp
    vita_ui.h
    "${GXM_SHADER_HEADER}"
)

# The shims must survive LTO. clang only emits the call to __aeabi_h2f during code generation,
# which happens after LTO has decided what to keep - so with the shim in bitcode, LLVM sees a
# definition nothing calls, drops it, and the link then fails on a symbol the compiler itself
# introduced. Compiling this file to a real object keeps the definitions where the final link
# can always see them.
set_source_files_properties(../vita_compat/src/vita_libc_shims.c PROPERTIES
    COMPILE_OPTIONS "-fno-lto")
# And for the abort report's wraps of raise and __assert_func (crash_report.cpp).
set_source_files_properties(crash_report.cpp PROPERTIES COMPILE_OPTIONS "-fno-lto")

# The emulator's executable, and the replay player's below: the same frontend, told its title ID.
# The kernel opens the module by its installed path rather than through app0:, so the app is
# told its title ID to build that path (LoadAzaharNative in citra_vita_main.cpp).
function(citra_vita_executable target titleid)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
                                                 "${CMAKE_SOURCE_DIR}/src/sgxd")
    create_target_directory_groups(${target})

    target_link_libraries(${target} PRIVATE citra_common citra_core input_common video_core)
    # The ID the crash report names the build by: a commit does not tell a dirty build's ELF.
    target_link_options(${target} PRIVATE "LINKER:--build-id=sha1")

    # imgui's core objects come from the VITASDK's imgui_vita2d archive (its 1.79 imgui build);
    # the vita2d backend object inside it is never referenced, so vita2d itself is not linked.
    target_link_libraries(${target} PRIVATE
        imgui_vita2d
        taihen_stub
        SceDisplay_stub
        SceGxm_stub
        SceCommonDialog_stub
        SceCtrl_stub
        SceTouch_stub
        SceAudio_stub
        ScePower_stub
        SceShellSvc_stub
        SceAppMgr_stub
        SceAppUtil_stub
        SceKernelDmacMgr_stub
        SceKernelModulemgr_stub
        SceVshBridge_stub
        m
    )

    target_include_directories(${target} PRIVATE ${VITASDK}/arm-vita-eabi/include)

    # vita_create_self looks for the executable next to the target rather than in the project's
    # shared output directory, so this target keeps its own.
    set_target_properties(${target} PROPERTIES
        LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ldscript.ld
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(${target} PRIVATE AZAHAR_VITA_TITLEID="${titleid}")
endfunction()

add_executable(citra_vita ${CITRA_VITA_SOURCES})

# ATTRIBUTE2=12 is the extended-memory mode: the app's LPDDR budget grows by 109 MB, which
# is what lets the newlib heap below hold the guest's 128 MB FCRAM and still leave the
# emulator real working room. Without it the ceiling was 210 MB with ~7 MB spare in-game,
# and one large FS read's std::bad_alloc froze the guest (see hle_ipc.h's async sections).
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

set(VITA_TITLEID "AZAHARPSV")
set(VITA_APP_NAME "Azahar")
set(VITA_VERSION "00.10")

# The vpk carries the kernel module and the app loads it at startup (LoadAzaharNative in
# citra_vita_main.cpp).
get_target_property(AZAHAR_NATIVE_MODULE azaharnative_stub AZAHAR_NATIVE_MODULE)
citra_vita_executable(citra_vita ${VITA_TITLEID})

# The SceShell authid grants the extra permissions the native backend needs (taking a core, the
# kernel-module bridge), and -na disables ASLR so the guest range lands where the module expects.
# -a goes through VITA_MAKE_FSELF_FLAGS (the macro has no keyword for it); NOASLR is -na.
set(VITA_MAKE_FSELF_FLAGS "-a 0x2800000000000001")
vita_create_self(citra_vita.self citra_vita UNSAFE NOASLR)

# A second eboot for profiling. vita-make-fself keeps only the loadable segments, so the one
# the console runs carries no section headers and no symbols and a host profiler can only show
# addresses. tools/make_debug_self.py copies that finished SELF and appends the sections from
# the velf beside it, so the same build exists twice: the small one to ship, and a large one
# whose addresses have names. Built on request - "ninja -C build-vita citra_vita_tracer".
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tracer.self
  COMMAND ${CMAKE_COMMAND} -E env python3
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_debug_self.py
          ${CMAKE_CURRENT_BINARY_DIR}/citra_vita.self
          ${CMAKE_CURRENT_BINARY_DIR}/citra_vita.velf
          ${CMAKE_CURRENT_BINARY_DIR}/tracer.self
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/citra_vita.self
          ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_debug_self.py
  COMMENT "Building tracer.self (symbols kept)")
add_custom_target(citra_vita_tracer DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/tracer.self)
vita_create_vpk(citra_vita.vpk ${VITA_TITLEID} citra_vita.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE ${AZAHAR_NATIVE_MODULE} azaharnative.skprx
        ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
        ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
        ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
        ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
# The module is a byproduct of the sub-build in src/core; this orders the vpk after it for
# generators that do not follow a file dependency across directories.
if (TARGET vita_native_module)
    add_dependencies(citra_vita.vpk-vpk vita_native_module)
endif()

# The replay player (diagnostics builds, on request: "ninja citra_vita_player.vpk-vpk"): a
# vprof capture's frame on sgxd with no title, as Razor replays a capture without the game
# (citra_vita_main.cpp's RunReplay). The capture and its requests are added to the vpk after
# the build, as app0:replay.vprof and app0:replay.txt (tools/frame_replay/FORMAT.md).
if (ENABLE_VITA_DIAGNOSTICS)
    set(PLAYER_TITLEID "AZRPLAY01")
    add_executable(citra_vita_player EXCLUDE_FROM_ALL ${CITRA_VITA_SOURCES})
    citra_vita_executable(citra_vita_player ${PLAYER_TITLEID})
    target_compile_definitions(citra_vita_player PRIVATE AZAHAR_REPLAY_PLAYER)
    vita_create_self(citra_vita_player.self citra_vita_player UNSAFE NOASLR)
    vita_create_vpk(citra_vita_player.vpk ${PLAYER_TITLEID} citra_vita_player.self
        VERSION ${VITA_VERSION}
        NAME "Azahar Replay"
        FILE ${AZAHAR_NATIVE_MODULE} azaharnative.skprx
            ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
            ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
            ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
            ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    )
    if (TARGET vita_native_module)
        add_dependencies(citra_vita_player.vpk-vpk vita_native_module)
    endif()
    # vita.cmake makes its targets part of "all"; the player is built on request only.
    foreach(player_target citra_vita_player-velf citra_vita_player.self-self
                          citra_vita_player.vpk-vpk)
        if (TARGET ${player_target})
            set_target_properties(${player_target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()
endif()

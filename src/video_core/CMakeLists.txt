add_library(video_core STATIC EXCLUDE_FROM_ALL
        debug_utils/debug_utils.cpp
        debug_utils/debug_utils.h
        gpu.cpp
        gpu.h
        gpu_debugger.h
        gpu_impl.h
        pica_types.h
        precompiled_headers.h
        rasterizer_accelerated.cpp
        rasterizer_accelerated.h
        rasterizer_interface.h
        pica/dirty_regs.h
        pica/geometry_pipeline.cpp
        pica/geometry_pipeline.h
        pica/pica_core.cpp
        pica/pica_core.h
        pica/output_vertex.cpp
        pica/output_vertex.h
        pica/primitive_assembly.cpp
        pica/primitive_assembly.h
        pica/regs_external.h
        pica/regs_framebuffer.h
        pica/regs_internal.cpp
        pica/regs_internal.h
        pica/regs_lcd.h
        pica/regs_lighting.h
        pica/regs_pipeline.h
        pica/regs_rasterizer.h
        pica/regs_shader.h
        pica/regs_texturing.h
        pica/shader_setup.cpp
        pica/shader_setup.h
        pica/shader_unit.cpp
        pica/shader_unit.h
        pica/packed_attribute.h
        pica/vertex_loader.cpp
        pica/vertex_loader.h
        pica/words_differ.h
        right_eye_disabler.cpp
        right_eye_disabler.h
        shader/debug_data.h
        shader/generator/glsl_fs_shader_gen.cpp
        shader/generator/glsl_fs_shader_gen.h
        shader/generator/glsl_shader_decompiler.cpp
        shader/generator/glsl_shader_decompiler.h
        shader/generator/glsl_shader_gen.cpp
        shader/generator/glsl_shader_gen.h
        shader/generator/pica_fs_config.cpp
        shader/generator/pica_fs_config.h
        shader/generator/profile.h
        shader/generator/shader_gen.cpp
        shader/generator/shader_gen.h
        shader/generator/shader_uniforms.cpp
        shader/generator/shader_uniforms.h
        shader/shader.cpp
        shader/shader.h
        shader/shader_interpreter.cpp
        shader/shader_interpreter.h
        shader/shader_jit.cpp
        shader/shader_jit.h
        shader/a32_emitter.h
        shader/shader_jit_a32_compiler.cpp
        shader/shader_jit_a32_compiler.h
        shader/shader_jit_a64_compiler.cpp
        shader/shader_jit_a64_compiler.h
        shader/shader_jit_x64_compiler.cpp
        shader/shader_jit_x64_compiler.h
        texture/etc1.cpp
        texture/etc1.h
        texture/texture_decode.cpp
        texture/texture_decode.h
        utils.h
)

target_sources(video_core PRIVATE
        renderer_sgxd/sgxd_dump.h
        renderer_sgxd/sgxd_flags.cpp
        renderer_sgxd/sgxd_flags.h
        renderer_sgxd/sgxd_rasterizer.cpp
        renderer_sgxd/sgxd_rasterizer.h
        renderer_sgxd/sgxd_shader_disk_cache.cpp
        renderer_sgxd/sgxd_shader_disk_cache.h
        renderer_sgxd/sgxd_shader_tier.cpp
        renderer_sgxd/sgxd_shader_tier.h
        renderer_sgxd/guest_gpu_memory.cpp
        renderer_sgxd/guest_gpu_memory.h
        renderer_sgxd/key_index.h
        renderer_sgxd/page_holds.cpp
        renderer_sgxd/page_holds.h
        renderer_sgxd/usse/fs_usse_gen.cpp
        renderer_sgxd/usse/fs_usse_gen.h
        renderer_sgxd/usse/usse_encoder.cpp
        renderer_sgxd/usse/usse_encoder.h
        renderer_sgxd/usse/usse_ir.cpp
        renderer_sgxd/usse/usse_ir.h
        renderer_sgxd/usse/usse_program.cpp
        renderer_sgxd/usse/usse_program.h
        renderer_sgxd/usse/vs_usse_gen.cpp
        renderer_sgxd/usse/vs_usse_gen.h
        renderer_sgxd/render_thread.cpp
        renderer_sgxd/render_thread.h
        renderer_sgxd/renderer_sgxd.cpp
        renderer_sgxd/renderer_sgxd.h
        renderer_sgxd/sgxd_backend.cpp
        renderer_sgxd/sgxd_backend.h
        renderer_sgxd/state_regs.h
        renderer_sgxd/untile_geometry.h
        # sgxd's plain C pieces the rasterizer builds its passes with.
        ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_program.c
        ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_recipe.c
)
# The dumps, vprof (the profiler) and its replay: diagnostics builds only.
if (ENABLE_VITA_DIAGNOSTICS)
    target_sources(video_core PRIVATE
            renderer_gxm/gxm_flags.cpp
            renderer_gxm/gxm_flags.h
            renderer_gxm/gxm_rasterizer.cpp
            renderer_gxm/gxm_rasterizer.h
            renderer_gxm/gxm_shader_tier.cpp
            renderer_gxm/gxm_shader_tier.h
            renderer_gxm/guest_gpu_memory.cpp
            renderer_gxm/guest_gpu_memory.h
            renderer_gxm/key_index.h
            renderer_gxm/page_holds.cpp
            renderer_gxm/page_holds.h
            renderer_gxm/usse/fs_usse_gen.cpp
            renderer_gxm/usse/fs_usse_gen.h
            renderer_gxm/usse/usse_encoder.cpp
            renderer_gxm/usse/usse_encoder.h
            renderer_gxm/usse/usse_ir.cpp
            renderer_gxm/usse/usse_ir.h
            renderer_gxm/usse/usse_program.cpp
            renderer_gxm/usse/usse_program.h
            renderer_gxm/usse/vs_usse_gen.cpp
            renderer_gxm/usse/vs_usse_gen.h
            renderer_gxm/render_thread.cpp
            renderer_gxm/render_thread.h
            renderer_gxm/renderer_gxm.cpp
            renderer_gxm/renderer_gxm.h
            renderer_gxm/sgxd_backend.cpp
            renderer_gxm/sgxd_backend.h
            renderer_gxm/state_regs.h
            renderer_gxm/untile_geometry.h
            # sgxd's plain C pieces the rasterizer builds its passes with.
            ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_program.c
            ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_recipe.c
    )
    # vprof, the profiler and its replay: diagnostics builds only.
    if (ENABLE_VITA_DIAGNOSTICS)
        target_sources(video_core PRIVATE
                renderer_gxm/gxm_rasterizer_vprof.cpp
                renderer_gxm/vprof.cpp
                renderer_gxm/vprof.h
                renderer_gxm/vprof_replay.cpp
                renderer_gxm/vprof_replay.h
        )
    endif()
    # sgxd_recipe.h takes the op list's types from the module's sgxd.h, plain C on any host.
    target_include_directories(video_core PRIVATE ${CMAKE_SOURCE_DIR}/src/sgxd
                                                  ${CMAKE_SOURCE_DIR}/vita_native/src)
    # The precompiled header is C++; a C file given it makes CMake build a C one from it. The
    # project's C++-only warning options are warnings of their own to a C compiler, which
    # -Werror would stop on.
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_program.c
        ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_recipe.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS "-Wno-error")
    # libgxm only: every program comes from the USSE emitters, nothing compiles on the console.
    target_include_directories(video_core PRIVATE $ENV{VITASDK}/arm-vita-eabi/include)
    target_link_libraries(video_core PRIVATE SceGxm_stub)
    # sgxd's user-side pieces that submit: the PTLA and the session (the frontend's
    # presenter uses the session and the recipe from here too), and the microkernel that runs
    # their lists with its compiler and assembler.
    target_sources(video_core PRIVATE ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_ptla.c
                                      ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_session.c
                                      ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_mkq.c
                                      ${CMAKE_SOURCE_DIR}/vita_native/src/sgxd_mk.c
                                      ${CMAKE_SOURCE_DIR}/vita_native/src/sgxd_usse.c)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_ptla.c
        ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_session.c ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_mkq.c
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/vita_native/src/sgxd_mk.c
        ${CMAKE_SOURCE_DIR}/vita_native/src/sgxd_usse.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON
        COMPILE_OPTIONS "-Wno-error")
endif ()

create_target_directory_groups(video_core)

target_link_libraries(video_core PUBLIC citra_common citra_core)
target_link_libraries(video_core PRIVATE Boost::serialization dds-ktx json-headers nihstro-headers tsl::robin_map)

if ("x86_64" IN_LIST ARCHITECTURE)
    target_link_libraries(video_core PUBLIC xbyak)
endif ()

if ("arm64" IN_LIST ARCHITECTURE)
    target_link_libraries(video_core PUBLIC oaknut)
endif ()

if ("arm" IN_LIST ARCHITECTURE)
    target_sources(video_core PRIVATE pica/pica_cmdlist_a32.S)
    set_source_files_properties(pica/pica_cmdlist_a32.S PROPERTIES LANGUAGE ASM)
endif ()

if (CITRA_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(video_core PRIVATE precompiled_headers.h)
endif ()

add_subdirectory(host_shaders)

add_library(video_core STATIC EXCLUDE_FROM_ALL
        custom_textures/custom_format.cpp
        custom_textures/custom_format.h
        custom_textures/custom_tex_manager.cpp
        custom_textures/custom_tex_manager.h
        custom_textures/material.cpp
        custom_textures/material.h
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
        renderer_base.cpp
        renderer_base.h
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
        rasterizer_cache/framebuffer_base.h
        rasterizer_cache/pixel_format.cpp
        rasterizer_cache/pixel_format.h
        rasterizer_cache/rasterizer_cache.cpp
        rasterizer_cache/rasterizer_cache.h
        rasterizer_cache/rasterizer_cache_base.h
        rasterizer_cache/sampler_params.h
        rasterizer_cache/slot_id.h
        rasterizer_cache/surface_base.cpp
        rasterizer_cache/surface_base.h
        rasterizer_cache/surface_params.cpp
        rasterizer_cache/surface_params.h
        rasterizer_cache/texture_codec.h
        rasterizer_cache/texture_cube.h
        rasterizer_cache/utils.cpp
        rasterizer_cache/utils.h
        # Needed as a fallback regardless of enabled renderers.
        renderer_software/sw_blitter.cpp
        renderer_software/sw_blitter.h
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
        video_core.cpp
        video_core.h
)

if (ENABLE_GXM)
    target_sources(video_core PRIVATE
            renderer_gxm/gxm_flags.cpp
            renderer_gxm/gxm_flags.h
            renderer_gxm/gxm_rasterizer.cpp
            renderer_gxm/gxm_rasterizer.h
            renderer_gxm/gxm_shader_tier.cpp
            renderer_gxm/gxm_shader_tier.h
            renderer_gxm/guest_gpu_memory.cpp
            renderer_gxm/guest_gpu_memory.h
            renderer_gxm/usse/fixed_usse_gen.cpp
            renderer_gxm/usse/fixed_usse_gen.h
            renderer_gxm/usse/fs_usse_gen.cpp
            renderer_gxm/usse/fs_usse_gen.h
            renderer_gxm/usse/gxp_writer.cpp
            renderer_gxm/usse/gxp_writer.h
            renderer_gxm/usse/usse_encoder.cpp
            renderer_gxm/usse/usse_encoder.h
            renderer_gxm/usse/usse_ir.cpp
            renderer_gxm/usse/usse_ir.h
            renderer_gxm/usse/vs_usse_gen.cpp
            renderer_gxm/usse/vs_usse_gen.h
            renderer_gxm/render_thread.cpp
            renderer_gxm/render_thread.h
            renderer_gxm/renderer_gxm.cpp
            renderer_gxm/renderer_gxm.h
            renderer_gxm/sgxd_backend.cpp
            renderer_gxm/sgxd_backend.h
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
    # presenter uses the session and the recipe from here too).
    target_sources(video_core PRIVATE ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_ptla.c
                                      ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_session.c)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_ptla.c
        ${CMAKE_SOURCE_DIR}/src/sgxd/sgxd_session.c PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif ()

if (ENABLE_SOFTWARE_RENDERER)
    target_sources(video_core PRIVATE
            renderer_software/renderer_software.cpp
            renderer_software/renderer_software.h
            renderer_software/sw_clipper.cpp
            renderer_software/sw_clipper.h
            renderer_software/sw_framebuffer.cpp
            renderer_software/sw_framebuffer.h
            renderer_software/sw_lighting.cpp
            renderer_software/sw_lighting.h
            renderer_software/sw_proctex.cpp
            renderer_software/sw_proctex.h
            renderer_software/sw_rasterizer.cpp
            renderer_software/sw_rasterizer.h
            renderer_software/sw_texturing.cpp
            renderer_software/sw_texturing.h
    )
endif ()
if (ENABLE_OPENGL)
    target_sources(video_core PRIVATE
            renderer_opengl/frame_dumper_opengl.cpp
            renderer_opengl/frame_dumper_opengl.h
            renderer_opengl/gl_blit_helper.cpp
            renderer_opengl/gl_blit_helper.h
            renderer_opengl/gl_driver.cpp
            renderer_opengl/gl_driver.h
            renderer_opengl/gl_rasterizer.cpp
            renderer_opengl/gl_rasterizer.h
            renderer_opengl/gl_rasterizer_cache.cpp
            renderer_opengl/gl_resource_manager.cpp
            renderer_opengl/gl_resource_manager.h
            renderer_opengl/gl_shader_disk_cache.cpp
            renderer_opengl/gl_shader_disk_cache.h
            renderer_opengl/gl_shader_manager.cpp
            renderer_opengl/gl_shader_manager.h
            renderer_opengl/gl_shader_util.cpp
            renderer_opengl/gl_shader_util.h
            renderer_opengl/gl_state.cpp
            renderer_opengl/gl_state.h
            renderer_opengl/gl_stream_buffer.cpp
            renderer_opengl/gl_stream_buffer.h
            renderer_opengl/gl_texture_mailbox.cpp
            renderer_opengl/gl_texture_mailbox.h
            renderer_opengl/gl_texture_runtime.cpp
            renderer_opengl/gl_texture_runtime.h
            renderer_opengl/gl_vars.cpp
            renderer_opengl/gl_vars.h
            renderer_opengl/pica_to_gl.h
            renderer_opengl/post_processing_opengl.cpp
            renderer_opengl/post_processing_opengl.h
            renderer_opengl/renderer_opengl.cpp
            renderer_opengl/renderer_opengl.h
    )
    target_link_libraries(video_core PRIVATE glad)
endif ()
if (ENABLE_VULKAN)
    target_sources(video_core PRIVATE
            renderer_vulkan/pica_to_vk.h
            renderer_vulkan/renderer_vulkan.cpp
            renderer_vulkan/renderer_vulkan.h
            renderer_vulkan/vk_blit_helper.cpp
            renderer_vulkan/vk_blit_helper.h
            renderer_vulkan/vk_common.cpp
            renderer_vulkan/vk_common.h
            renderer_vulkan/vk_descriptor_update_queue.cpp
            renderer_vulkan/vk_descriptor_update_queue.h
            renderer_vulkan/vk_graphics_pipeline.cpp
            renderer_vulkan/vk_graphics_pipeline.h
            renderer_vulkan/vk_master_semaphore.cpp
            renderer_vulkan/vk_master_semaphore.h
            renderer_vulkan/vk_memory_util.cpp
            renderer_vulkan/vk_memory_util.h
            renderer_vulkan/vk_rasterizer.cpp
            renderer_vulkan/vk_rasterizer.h
            renderer_vulkan/vk_rasterizer_cache.cpp
            renderer_vulkan/vk_scheduler.cpp
            renderer_vulkan/vk_scheduler.h
            renderer_vulkan/vk_resource_pool.cpp
            renderer_vulkan/vk_resource_pool.h
            renderer_vulkan/vk_instance.cpp
            renderer_vulkan/vk_instance.h
            renderer_vulkan/vk_pipeline_cache.cpp
            renderer_vulkan/vk_pipeline_cache.h
            $<$<NOT:$<BOOL:${ENABLE_LIBRETRO}>>:renderer_vulkan/vk_platform.cpp>
            renderer_vulkan/vk_platform.h
            $<$<NOT:$<BOOL:${ENABLE_LIBRETRO}>>:renderer_vulkan/vk_present_window.cpp>
            $<$<NOT:$<BOOL:${ENABLE_LIBRETRO}>>:renderer_vulkan/vk_present_window.h>
            renderer_vulkan/vk_render_manager.cpp
            renderer_vulkan/vk_render_manager.h
            renderer_vulkan/vk_shader_disk_cache.cpp
            renderer_vulkan/vk_shader_disk_cache.h
            renderer_vulkan/vk_shader_util.cpp
            renderer_vulkan/vk_shader_util.h
            renderer_vulkan/vk_stream_buffer.cpp
            renderer_vulkan/vk_stream_buffer.h
            $<$<NOT:$<BOOL:${ENABLE_LIBRETRO}>>:renderer_vulkan/vk_swapchain.cpp>
            $<$<NOT:$<BOOL:${ENABLE_LIBRETRO}>>:renderer_vulkan/vk_swapchain.h>
            renderer_vulkan/vk_texture_runtime.cpp
            renderer_vulkan/vk_texture_runtime.h
            shader/generator/spv_fs_shader_gen.cpp
            shader/generator/spv_fs_shader_gen.h
    )
    target_link_libraries(video_core PRIVATE vulkan-headers vma sirit SPIRV glslang)
endif ()

if (ENABLE_SDL2)
    target_link_libraries(video_core PRIVATE SDL2::SDL2)
endif ()

add_dependencies(video_core host_shaders)
target_include_directories(video_core PRIVATE ${HOST_SHADERS_INCLUDE})

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
    # The software rasterizer's per-pixel loop is register bound rather than arithmetic bound -
    # a 3 KB stack frame, and about a fifth of its samples are stack reloads and a float
    # constant fetched from the literal pool three times over. It is built at the build type's
    # level, which for a debug-info build is -O2; give these -O3 regardless, since this is the
    # renderer whose throughput decides whether a frame arrives. The frame pointer goes with
    # it: a debug-info build keeps r11 reserved, and on a target with fourteen usable general
    # registers that is one the pixel loop would rather have.
    # APPEND, not PROPERTIES: COMPILE_OPTIONS is one property, so a second plain set on any of
    # these files replaces this one instead of adding to it. That is what happened - the
    # -ffp-contract=off block below overlaps three of these four, and sw_rasterizer.cpp, the
    # file this is for, was building at -O2 with no frame-pointer flag.
    set_property(SOURCE
            renderer_software/sw_rasterizer.cpp
            renderer_software/sw_texturing.cpp
            renderer_software/sw_lighting.cpp
            renderer_software/sw_blitter.cpp
            APPEND PROPERTY COMPILE_OPTIONS -O3 -fomit-frame-pointer -funsafe-math-optimizations)
    # Clang refuses a precompiled header built without unsafe float math in a translation unit
    # that enables it, and the Vita build uses one. These four are the only files that differ,
    # so they parse the headers themselves rather than the whole target losing its PCH or
    # getting the math flag it has no use for.
    set_source_files_properties(
            renderer_software/sw_rasterizer.cpp
            renderer_software/sw_texturing.cpp
            renderer_software/sw_lighting.cpp
            renderer_software/sw_blitter.cpp
            PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif ()

# The software renderer keeps a scalar reference implementation and a NEON one that must agree.
# GCC contracts float expressions into fused multiply-adds by default, which the scalar path
# takes and the intrinsics do not, so the two disagree in the last bit - enough, at an exact
# texel or depth-test boundary, to pick a different texel or a different side of the test.
# Turning contraction off makes them agree. It is also free on cores without FMA (Cortex-A9 and
# anything else on VFPv3), which is a target for this renderer.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_property(SOURCE
            renderer_software/sw_rasterizer.cpp
            renderer_software/sw_framebuffer.cpp
            renderer_software/sw_lighting.cpp
            renderer_software/sw_clipper.cpp
            renderer_software/sw_texturing.cpp
            renderer_software/sw_proctex.cpp
            APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
endif ()

if (CITRA_USE_PRECOMPILED_HEADERS)
    target_precompile_headers(video_core PRIVATE precompiled_headers.h)
endif ()

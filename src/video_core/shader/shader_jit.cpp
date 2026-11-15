// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/emu_prof.h"
#include "common/pipeline_stats.h"
#ifdef __vita__
#include <psp2/kernel/clib.h>
#endif
#include "common/arch.h"
#if CITRA_HAS_SHADER_JIT

#include <chrono>
#include <mutex>

#include "common/assert.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "video_core/shader/shader.h"
#include "video_core/shader/shader_jit.h"
#if CITRA_ARCH(arm64)
#include "video_core/shader/shader_jit_a64_compiler.h"
#endif
#if CITRA_ARCH(x86_64)
#include "video_core/shader/shader_jit_x64_compiler.h"
#endif
#if CITRA_ARCH(arm32)
#include "video_core/shader/shader_jit_a32_compiler.h"
#endif

namespace Pica::Shader {

JitEngine::JitEngine() = default;
JitEngine::~JitEngine() = default;

void JitEngine::SetupBatch(ShaderSetup& setup, u32 entry_point) {
    ASSERT(entry_point < MAX_PROGRAM_CODE_LENGTH);
    setup.entry_point = entry_point;

    setup.DoProgramCodeFixup();
    const u64 code_hash = setup.GetProgramCodeHash();
    const u64 swizzle_hash = setup.GetSwizzleDataHash();

#if CITRA_ARCH(arm32)
    // The A32 compiler builds only what the entry point reaches.
    const u64 cache_key =
        Common::HashCombine(Common::HashCombine(code_hash, swizzle_hash), u64{entry_point});
#else
    const u64 cache_key = Common::HashCombine(code_hash, swizzle_hash);
#endif
    auto iter = cache.find(cache_key);
    if (iter != cache.end()) {
        setup.cached_shader = iter->second.get();
#if CITRA_ARCH(arm32)
        if (!iter->second->Compiled()) {
            setup.cached_shader = nullptr;
        }
#endif
    } else {
        setup.cached_shader = nullptr;
        std::shared_ptr<JitShader> shader = Shared(cache_key, setup, entry_point);
        if (shader) {
            setup.cached_shader = shader.get();
#if CITRA_ARCH(arm32)
            if (!shader->Compiled()) {
                setup.cached_shader = nullptr;
            }
#endif
            cache.emplace_hint(iter, cache_key, std::move(shader));
        }
    }
}

std::shared_ptr<JitShader> JitEngine::Shared(u64 cache_key, ShaderSetup& setup, u32 entry_point) {
    // One compiled copy per guest shader for the whole process. The lock is held across the
    // compile so two threads meeting the same new shader translate it once; only engine-cache
    // misses come here, a few dozen per area.
    static std::mutex mutex;
    static std::unordered_map<u64, std::shared_ptr<JitShader>> shared;
    std::scoped_lock lock{mutex};
    if (const auto it = shared.find(cache_key); it != shared.end()) {
        return it->second;
    }
    // Every compile here is a guest shader being translated on whichever thread is running
    // the command list, which is a stall on that thread. Logged so a hitch can be matched
    // against a compile rather than guessed at.
    auto shader = std::make_shared<JitShader>();
    const auto start = std::chrono::steady_clock::now();
#if CITRA_ARCH(arm32)
    shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData(), entry_point);
#else
    shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());
#endif
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    Common::EmuProf::Add("vs jit compile", static_cast<u64>(us));
    LOG_INFO(HW_GPU, "shader JIT compiled {:016x} entry {} in {}.{:03} ms (shared {})", cache_key,
             entry_point, us / 1000, us % 1000, shared.size() + 1);
    shared.emplace(cache_key, shader);
#ifdef __vita__
    sceClibPrintf("[mh4u] shader JIT #%u: %016llx entry %u, %u KiB in %u ms, arena %u KiB%s\n",
                  static_cast<unsigned>(shared.size()), static_cast<unsigned long long>(cache_key),
                  entry_point,
                  static_cast<unsigned>(Common::PipelineStats::jit_last_bytes.load() / 1024),
                  static_cast<unsigned>(us / 1000),
                  static_cast<unsigned>(Common::PipelineStats::jit_arena_bytes.load() / 1024),
                  shader->Compiled() ? "" : " (NOT COMPILED: interpreted)");
#endif
    return shader;
}

MICROPROFILE_DECLARE(GPU_Shader);

void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {
#if CITRA_ARCH(arm32)
    if (setup.cached_shader == nullptr) {
        Common::PipelineStats::jit_interp_runs.fetch_add(1, std::memory_order_relaxed);
        fallback.Run(setup, state);
        return;
    }
#endif
    ASSERT(setup.cached_shader != nullptr);

    MICROPROFILE_SCOPE(GPU_Shader);

    const JitShader* shader = static_cast<const JitShader*>(setup.cached_shader);
    shader->Run(setup, state, setup.entry_point);
}

} // namespace Pica::Shader

#endif // CITRA_HAS_SHADER_JIT

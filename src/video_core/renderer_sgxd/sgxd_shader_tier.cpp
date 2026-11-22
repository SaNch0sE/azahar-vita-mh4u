// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <utility>
#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/renderer_sgxd/sgxd_shader_tier.h"
#include "video_core/renderer_sgxd/usse/fs_usse_gen.h"
#include "video_core/renderer_sgxd/usse/vs_usse_gen.h"

namespace SgxdRenderer {

ShaderTier2::ShaderTier2()
    : thread{Common::ThreadCfg{"GxmTier2"}, [this](std::stop_token stop) { Loop(stop); }} {}

ShaderTier2::~ShaderTier2() {
    thread.request_stop();
    wake.RaiseAll();
    thread.join();
}

void ShaderTier2::QueueFragment(u64 key, const Pica::Shader::FSConfig& config) {
    {
        std::scoped_lock lock{mutex};
        auto job = std::make_unique<Job>();
        job->key = key;
        job->fs.emplace(config);
        jobs.push_back(std::move(job));
    }
    wake.Raise();
}

void ShaderTier2::QueueVertex(u64 key, const Usse::VsRequest& request) {
    auto job = std::make_unique<Job>();
    job->key = key;
    std::copy(request.code.begin(), request.code.end(), job->code.begin());
    std::copy(request.swizzle.begin(), request.swizzle.end(), job->swizzle.begin());
    job->vs = request;
    job->vs.code = std::span<const u32>{job->code.data(), request.code.size()};
    job->vs.swizzle = std::span<const u32>{job->swizzle.data(), request.swizzle.size()};
    {
        std::scoped_lock lock{mutex};
        jobs.push_back(std::move(job));
    }
    wake.Raise();
}

std::vector<ShaderTier2::Result> ShaderTier2::Take() {
    std::scoped_lock lock{mutex};
    ready.store(false, std::memory_order_relaxed);
    return std::exchange(results, {});
}

void ShaderTier2::Loop(std::stop_token stop) {
    Common::SetCurrentThreadRole(Common::ThreadRole::ShaderCompiler);
    std::stop_callback wake_on_stop(stop, [this] { wake.RaiseAll(); });
    while (true) {
        std::unique_ptr<Job> job;
        {
            std::unique_lock lock{mutex};
            // Lowered before the looks, as ShaderTier1's: a stop's RaiseAll (not under this
            // mutex) or a job after them raises again, where one between the stop's look and
            // the lowering would be lost and the join would wait for ever.
            wake.Lower();
            if (stop.stop_requested()) {
                // The two workers share the flag: this lowering may have cleared the other's
                // stop wake (it looked before the stop and waits after), so it is raised again.
                wake.RaiseAll();
                return;
            }
            if (jobs.empty()) {
                lock.unlock();
                wake.Wait();
                continue;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        const auto start = std::chrono::steady_clock::now();
        Result result{};
        result.key = job->key;
        result.vertex = !job->fs;
        if (job->fs) {
            result.program = Usse::EmitFragmentProgram(*job->fs, &result.refusal, 2);
        } else {
            result.program =
                Usse::EmitVertexProgram(job->vs, result.input_regs, &result.refusal, 2);
        }
        result.took_us = static_cast<u32>(std::chrono::duration_cast<std::chrono::microseconds>(
                                              std::chrono::steady_clock::now() - start)
                                              .count());
        std::scoped_lock lock{mutex};
        results.push_back(std::move(result));
        ready.store(true, std::memory_order_release);
    }
}

} // namespace SgxdRenderer

// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "common/named_thread.h"
#include "common/wake_flag.h"
#include "video_core/renderer_gxm/usse/gxp_writer.h"
#include "video_core/renderer_gxm/usse/vs_usse_gen.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/shader/generator/pica_fs_config.h"

namespace SgxdRenderer {

/**
 * The second tier: fragment and vertex programs that draw a lot are emitted again with the IR's
 * optimising passes (usse_ir level 2) on a worker thread (ThreadRole::ShaderCompiler, core 1),
 * and the render thread installs the better program before a later draw. The first tier's
 * program serves until then, so no draw ever waits for the passes.
 */
class ShaderTier2 {
public:
    ShaderTier2();
    ~ShaderTier2();

    ShaderTier2(const ShaderTier2&) = delete;
    ShaderTier2& operator=(const ShaderTier2&) = delete;

    void QueueFragment(u64 key, const Pica::Shader::FSConfig& config);
    /// Copies the PICA code and swizzle data the request points at.
    void QueueVertex(u64 key, const Usse::VsRequest& request);

    struct Result {
        u64 key;
        bool vertex;
        std::optional<Usse::GxpProgram> program; ///< empty when level 2 failed
        u32 input_regs; ///< a vertex program's, which the first tier's must equal
        std::string refusal;
        u32 took_us;
        u32 took_cycles; ///< the same time in the cycle counter's counts
    };
    [[nodiscard]] bool Ready() const {
        return ready.load(std::memory_order_acquire);
    }
    std::vector<Result> Take();

private:
    struct Job {
        u64 key;
        std::optional<Pica::Shader::FSConfig> fs; ///< a fragment program's, else a vertex one
        Usse::VsRequest vs;                       ///< its spans point into code and swizzle
        Pica::ProgramCode code;
        Pica::SwizzleData swizzle;
    };
    void Loop(std::stop_token stop);

    std::mutex mutex;
    /// unique_ptr: a vertex job's request points into the job's own arrays.
    std::deque<std::unique_ptr<Job>> jobs;
    std::vector<Result> results;
    std::atomic<bool> ready{};
    Common::WakeFlag wake;
    Common::NamedThread thread;
};

} // namespace SgxdRenderer

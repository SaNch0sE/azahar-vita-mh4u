// Copyright 2023 Citra Emulator Project
// Copyright 2024 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include "common/archives.h"
#include "common/microprofile.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "video_core/gpu.h"
#include "video_core/gpu_debugger.h"
#include "video_core/gpu_impl.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_base.h"
#include "video_core/right_eye_disabler.h"
#include "video_core/video_core.h"

namespace VideoCore {
struct GPU::Impl {
    Core::Timing& timing;
    Core::System& system;
    Memory::MemorySystem& memory;
    Pica::PicaCore pica;
    GraphicsDebugger gpu_debugger;
    std::unique_ptr<RendererBase> renderer;
    RasterizerInterface* rasterizer;
    Core::TimingEventType* vblank_event;
    Service::GSP::InterruptHandler signal_interrupt;

#ifdef VITA_DIAGNOSTICS
    std::atomic<u64> kind_wait_us[static_cast<std::size_t>(GPU::OpKind::Count)]{};
    std::atomic<u64> present_us{};
    std::atomic<u32> presents{};
    std::atomic<u32> presents_dropped{};
    /// The last present's time: VBlankCallback drops one that comes too soon after it.
    std::chrono::steady_clock::time_point last_present{};
    /// vprof's replay runs: GPU::Signal drops the interrupts.
    bool interrupts_muted{};
    /// The interrupts GPU::Signal holds until the GPU has done the work before them: the id and
    /// the rasterizer's fence, oldest first (GPU::DeliverInterrupts).
    std::deque<std::pair<Service::GSP::InterruptId, u64>> held_interrupts;
    /// gxm_flags `irqnow`: every interrupt at once, as upstream (no fence).
    bool interrupts_now{};

    explicit Impl(Core::System& system, Frontend::EmuWindow& emu_window,
                  Frontend::EmuWindow* secondary_window)
        : timing{system.CoreTiming()}, system{system}, memory{system.Memory()},
          debug_context{Pica::g_debug_context}, pica{memory, debug_context},
          renderer{VideoCore::CreateRenderer(emu_window, secondary_window, pica, system)},
          rasterizer{renderer->Rasterizer()} {}
    ~Impl() = default;
};
} // namespace VideoCore
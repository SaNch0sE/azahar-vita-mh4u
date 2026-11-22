// Copyright 2023 Citra Emulator Project
// Copyright 2024 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
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
#ifdef ENABLE_GXM
#include "video_core/renderer_gxm/render_thread.h"
#endif

namespace VideoCore {
struct GPU::Impl {
    Core::Timing& timing;
    Core::System& system;
    Memory::MemorySystem& memory;
    /// The PICA, the emulation side's: command lists, registers, shader setups, interrupts.
    Pica::PicaCore pica;
#ifdef ENABLE_GXM
    /// The parse thread's, which parses the lists (SgxdRenderer::RenderThread::ParseLoop).
    Pica::PicaCore parse_pica;
    /// The render thread's copy of its state, as of the work that thread has run: the sgxd
    /// rasterizer reads this one (SgxdRenderer::RenderThread).
    Pica::PicaCore render_pica;
#endif
    GraphicsDebugger gpu_debugger;
    std::unique_ptr<RendererBase> renderer;
    /// What the PICA and this GPU call: with GXM the render thread, which queues to the
    /// renderer's own rasterizer.
    RasterizerInterface* rasterizer;
    Core::TimingEventType* vblank_event;
    Service::GSP::InterruptHandler signal_interrupt;

#ifdef VITA_DIAGNOSTICS
    /// GPU::GuestIdle's: each list's scan end (process time, us) and length, until the next idle,
    /// and the totals TakeScanRoom takes. The emulation thread's alone.
    std::vector<std::pair<u64, u32>> scans_pending;
    GPU::ScanRoom scan_room{};
#endif
#ifndef ENABLE_GXM
    /// The last present's time: VBlankCallback drops one that comes too soon after it.
    std::chrono::steady_clock::time_point last_present{};
#endif
    /// vprof's replay runs: GPU::Signal drops the interrupts.
    bool interrupts_muted{};
    /// The rasterizer's ticket for the work queued before the last vblank: the next vblank
    /// waits for it (GPU::VBlankCallback).
    u64 vblank_ticket{};
#ifdef ENABLE_GXM
    /// Whether the guest's screens may show something new since the last present a vblank
    /// sent: a buffer swap, a write into a framebuffer the LCD shows (GPU::ScreensWritten), a
    /// loaded state. The LCD's registers and fill are compared with the last present's too. A
    /// vblank with none of them sends no present: a 30 fps game's second vblank would show the same
    /// picture again.
    bool screens_changed{true};
    std::array<Pica::FramebufferConfig, 2> presented_fb{};
    std::array<Pica::ColorFill, 2> presented_fill{};
    /// Last: it stops before anything it runs goes (GPU::~GPU stops it first).
    std::unique_ptr<SgxdRenderer::RenderThread> render;
#endif

    explicit Impl(Core::System& system, Frontend::EmuWindow& emu_window,
                  Frontend::EmuWindow* secondary_window)
        : timing{system.CoreTiming()}, system{system}, memory{system.Memory()},
#ifdef ENABLE_GXM
          pica{memory}, parse_pica{memory}, render_pica{memory},
          renderer{VideoCore::CreateRenderer(emu_window, secondary_window, render_pica, system)},
#else
          renderer{VideoCore::CreateRenderer(emu_window, secondary_window, pica, system)},
#endif
          rasterizer{renderer->Rasterizer()} {
    }
    ~Impl() = default;
};
} // namespace VideoCore
// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <psp2/power.h>
#include "common/heap_check.h"
#include "common/host_clock.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/perf_stats.h"
#include "video_core/renderer_sgxd/renderer_sgxd.h"

namespace SgxdRenderer {

RendererSgxd::RendererSgxd(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window)
    : system{system}, render_window{window}, memory{system.Memory()},
      guest_memory{system.Memory()}, rasterizer{pica, system.Memory(), guest_memory} {
    rasterizer.SetFlipHook([this](u32* fence) { return FlipPending(fence); },
                           [this] { return FlipDueIn(); }, [this] { FlipDone(); });
}

RendererSgxd::~RendererSgxd() {
    // The GPU's view of guest memory goes with guest_memory.
    render_window.ForgetGuestScreens();
}

void RendererSgxd::PresentFrame(const std::array<Pica::FramebufferConfig, 2>& fb,
                               const std::array<Pica::ColorFill, 2>& fill) {
    // The LCD scans 240-pixel columns: 400 of them on the top screen, 320 on the bottom.
    // Screen 0's right-eye buffer (3D) is not shown.
    Frontend::EmuWindow::GuestScreen screens[2]{};
    for (u32 i = 0; i < 2; i++) {
        const Pica::FramebufferConfig& f = fb[i];
        Frontend::EmuWindow::GuestScreen& s = screens[i];
        s.address = f.second_fb_active ? f.address_left2 : f.address_left1;
        s.stride = f.stride;
        s.columns = i == 0 ? 400 : 320;
        s.format = static_cast<u32>(f.color_format.Value());
        // Only guest memory the GPU has mapped (GuestGpuMemory) can be sampled; a framebuffer
        // elsewhere is not drawn.
        const auto inside = [&](PAddr base, u32 size) {
            return s.address >= base && s.address - base + u64{s.stride} * s.columns <= size;
        };
        const bool mapped = inside(Memory::VRAM_PADDR, Memory::VRAM_SIZE) ||
                            inside(Memory::FCRAM_PADDR, Memory::FCRAM_SIZE);
        s.pixels = mapped ? memory.GetPhysicalPointer(s.address) : nullptr;
        s.uncached = mapped && guest_memory.IsUncached(s.address, s.stride * s.columns);
        s.fill = fill[i].is_enabled != 0;
        s.fill_r = static_cast<u8>(fill[i].color_r);
        s.fill_g = static_cast<u8>(fill[i].color_g);
        s.fill_b = static_cast<u8>(fill[i].color_b);
    }
    // No swap timing: PerfStats' swap counters are the emulation thread's. The rasterizer's
    // frame ends at every vblank, shown or not (RenderThread's Op::FrameEnd).
    render_window.PresentGuestScreens(screens);
}

void RendererSgxd::EndFrame() {
    Common::HeapCheck::Check("a guest frame's end");
    system.perf_stats->EndSystemFrame();
    if (!render_window.PaceFrame()) {
        system.frame_limiter.DoFrameLimiting(system.CoreTiming().GetGlobalTimeUs());
    }
    Common::SetHostClockMhz(static_cast<u32>(std::max(1, scePowerGetArmClockFrequency())));
    system.perf_stats->BeginSystemFrame();
}

void RendererSgxd::UpdateCurrentFramebufferLayout(bool is_portrait_mode) {
    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();
    render_window.UpdateCurrentFramebufferLayout(layout.width, layout.height, is_portrait_mode);
}

void RendererSgxd::PollWindow() {
    render_window.PollEvents();
}

bool RendererSgxd::PresentBusy() {
    return render_window.PresentBusy();
}

void RendererSgxd::FlipDone() {
    render_window.FlipDone();
}

bool RendererSgxd::FlipPending(u32* fence) {
    return render_window.FlipPending(fence);
}

s64 RendererSgxd::FlipDueIn() {
    return render_window.FlipDueIn();
}

bool RendererSgxd::WouldMakeUncached(PAddr addr, u32 size) const {
    return guest_memory.WouldMove(addr, size);
}

void RendererSgxd::MakeUncached(PAddr addr, u32 size, bool overwritten) {
    guest_memory.MakeUncached(addr, size, overwritten);
}

} // namespace SgxdRenderer

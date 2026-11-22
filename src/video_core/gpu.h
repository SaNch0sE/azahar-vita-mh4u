// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <boost/serialization/access.hpp>

#include "core/hle/service/gsp/gsp_interrupt.h"
#include "video_core/rasterizer_interface.h"

namespace Service::GSP {
struct Command;
struct FrameBufferInfo;
} // namespace Service::GSP

namespace Core {
class System;
}

namespace Pica {
class PicaCore;
struct RegsLcd;
union ColorFill;
struct DisplayTransferConfig;
} // namespace Pica

namespace Frontend {
class EmuWindow;
}

namespace VideoCore {

/// Measured on hardware to be 2240568 timer cycles or 4481136 ARM11 cycles
constexpr u64 FRAME_TICKS = 4481136ull;

class GraphicsDebugger;
class RendererBase;
class RightEyeDisabler;

/**
 * The GPU class is the high level interface to the video_core for core services.
 */
class GPU {
public:
    explicit GPU(Core::System& system, Frontend::EmuWindow& emu_window,
                 Frontend::EmuWindow* secondary_window);
    ~GPU();

    /// Sets the function to call for signalling GSP interrupts.
    void SetInterruptHandler(Service::GSP::InterruptHandler handler);

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    void FlushRegion(PAddr addr, u32 size);

    /// Notify rasterizer that any caches of the specified region should be invalidated
    void InvalidateRegion(PAddr addr, u32 size);

    /// Flushes and invalidates all memory in the rasterizer cache and removes any leftover state.
    void ClearAll(bool flush);

    /// Executes the provided GSP command.
    void Execute(const Service::GSP::Command& command);

    /// Updates GPU display framebuffer configuration using the specified parameters.
    void SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info);

    /// Sets the LCD color fill configuration for the top and bottom screens.
    void SetColorFill(const Pica::ColorFill& fill);

    /// Reads a word from the GPU virtual address.
    u32 ReadReg(VAddr addr);

    /// Writes the provided value to the GPU virtual address.
    void WriteReg(VAddr addr, u32 data);

    /// Returns a mutable reference to the renderer.
    [[nodiscard]] VideoCore::RendererBase& Renderer();

    /// Returns a mutable reference to the PICA GPU.
    [[nodiscard]] Pica::PicaCore& PicaCore();

    /// Returns an immutable reference to the PICA GPU.
    [[nodiscard]] const Pica::PicaCore& PicaCore() const;

    /// Returns a mutable reference to the GSP command debugger.
    [[nodiscard]] GraphicsDebugger& Debugger();

    RightEyeDisabler& GetRightEyeDisabler() {
        return *right_eye_disabler;
    }

    void ApplyPerProgramSettings(u64 program_ID);

    /// Recreates the renderer (for GL context reset in libretro)
    void RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window);

    /// Releases the renderer (for GL context destroy in libretro)
    void ReleaseRenderer();

    /// What a GPU operation is, for `perflog`'s split of the GPU's work and vprof's numbering.
    enum class OpKind : u32 { CmdList, RegWrite, Fill, Transfer, Dma, Count };

    /// Since the last call (diagnostics: `perflog`): the GPU operations' time by kind, the part
    /// of it spent waiting for the GPU, and the presents' time; presents dropped in any build.
    struct OpStats {
        u64 kind_us[static_cast<std::size_t>(OpKind::Count)];
        u32 kind_ops[static_cast<std::size_t>(OpKind::Count)];
        u64 kind_wait_us[static_cast<std::size_t>(OpKind::Count)];
        u64 present_us;
        u32 presents;
        u32 presents_dropped;
    };
    OpStats TakeOpStats();
    /// The guest goes idle (every core waiting: its clock jumps to the next event). Each command
    /// list's scan since the last call is held against the emulation thread's time between the
    /// scan's end and this idle: what a scan off the emulation thread could hide, if the guest
    /// waited for nothing but the list's P3D here (diagnostics: `perflog`).
    void GuestIdle();
    struct ScanRoom {
        u32 lists;               ///< scans an idle has followed
        u64 scan_us;             ///< their time on the emulation thread
        u64 hidden_us;           ///< of it, what the time before the idle covers
        std::array<u32, 5> gaps; ///< scan end to idle: under 0.5, 2, 8, 16 ms, longer
        u32 pending;             ///< scans no idle has followed yet
    };
    ScanRoom TakeScanRoom();
#endif

    /// vprof's replay: the GSP commands it executes carry physical addresses already, which
    /// VirtualToPhysicalAddress passes through.
    bool replay_physical{};
#ifdef VITA_DIAGNOSTICS
    /// vprof's replay (vprof_replay.h) in the replay player, with no guest at all
    /// (Core::System::InitReplay). `first` is the first batch of requests; more come from
    /// host0:vprof-replay.txt until one quits.
    void VprofReplay(const std::string& capture, const std::string& first);
#endif

private:
    void SubmitCmdList(u32 index);

    // Interrupt index must be 0 or 1 to signal the relative PSC interrupt.
    void MemoryFill(u32 index, u32 intr_index);

    void MemoryTransfer();
    void RunMemoryTransfer(const Pica::DisplayTransferConfig& config, bool allow_display_transfer);

    void VBlankCallback(uintptr_t user_data, s64 cycles_late);

    /// Has the renderer keep [addr, addr + size) where the CPU reaches it uncached, if it would
    /// (RendererBase::WouldMakeUncached), before the GPU writes or shows it.
    void KeepUncached(PAddr addr, u32 size, bool overwritten);

    /// A DMA's piece that is contiguous in physical memory on both sides.
    struct DmaRun {
        PAddr src;
        PAddr dst;
        u32 size;
    };
    /// GSP's DMA of size bytes from src to dst (the current process's addresses) as physical
    /// runs. Memory outside guest FCRAM and VRAM stops the emulator.
    std::vector<DmaRun> DmaRuns(VAddr src, VAddr dst, u32 size);
    /// A DMA's runs, queued to the PTLA; the DMA interrupt after delay.
    void RunDma(const std::vector<DmaRun>& runs, u64 delay);

    /// Runs op, a GSP command's or a register trigger's work (vprof numbers it, perflog times
    /// it), then lets the rasterizer start what it held back (RasterizerInterface::GpuIdle).
    void RunGpuOp(OpKind kind, const std::function<void()>& op);
    /// Raises a GSP interrupt after delay nanoseconds of guest time, as upstream; none while
    /// vprof's replay runs.
    void Signal(Service::GSP::InterruptId id, u64 delay);

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

    std::unique_ptr<RightEyeDisabler> right_eye_disabler;

private:
    friend class RightEyeDisabler;
    struct Impl;
    std::unique_ptr<Impl> impl;

    PAddr VirtualToPhysicalAddress(VAddr addr);
};

} // namespace VideoCore

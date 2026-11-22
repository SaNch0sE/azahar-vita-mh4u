// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <boost/serialization/access.hpp>
#include <boost/serialization/version.hpp>

#include "core/hle/result.h"
#include "core/hle/service/gsp/gsp_interrupt.h"
#include "video_core/rasterizer_interface.h"

struct AzaharStorePermits;

namespace Service::GSP {
struct Command;
struct FrameBufferInfo;
} // namespace Service::GSP

namespace Core {
class System;
struct TimingEventType;
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

namespace SgxdRenderer {
class RenderThread;
class RendererSgxd;
}

namespace VideoCore {

/// Measured on hardware to be 2240568 timer cycles or 4481136 ARM11 cycles
constexpr u64 FRAME_TICKS = 4481136ull;

class GraphicsDebugger;
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

    /// Both, in one call
    void FlushAndInvalidateRegion(PAddr addr, u32 size);

    /// Flushes and invalidates all memory in the rasterizer cache and removes any leftover state.
    void ClearAll(bool flush);

    /// A watcher of the game's (svc 0xF0, a title patch's): an engine's resource file at
    /// [addr, addr + size), whose textures, indices and vertices the game does not write once
    /// loaded. The lists' scan holds no page for the GPU's reads inside it (RenderThread::
    /// WatchResource); outside GPU memory it is not the GPU's.
    void WatchResource(VAddr addr, u32 size);

    /// A watcher of the game's: a buffer [addr, addr + size) of the game's own per-frame command
    /// lists and vertices, which it writes from now on, and which the GPU read frames ago. The
    /// emulator holds no page for the GPU's reads inside it, and waits here for the work that
    /// read it last (RenderThread::WatchFrameBuffer).
    void WatchFrameBuffer(VAddr addr, u32 size);

    /// Executes the provided GSP command.
    Result Execute(const Service::GSP::Command& command);

    /// Updates GPU display framebuffer configuration using the specified parameters.
    void SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info);

    /// Sets the LCD color fill configuration for the top and bottom screens.
    void SetColorFill(const Pica::ColorFill& fill);

    /// Reads a word from the GPU virtual address.
    u32 ReadReg(VAddr addr);

    /// Writes the provided value to the GPU virtual address.
    void WriteReg(VAddr addr, u32 data);

    /// Returns a mutable reference to the renderer.
    [[nodiscard]] SgxdRenderer::RendererSgxd& Renderer();

    /// The window's framebuffer layout made again from its size (the settings changed).
    void UpdateFramebufferLayout();

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

    /// The emulation loop's, between slices: lets go of the page holds whose GPU work has
    /// completed (SgxdRenderer::PageHolds).
    void DeliverInterrupts();

    /// The guest's table is installed (the native core's first slice): from now on the GPU's
    /// work holds the guest pages it reads and writes until it is done, and the HLE's own
    /// accesses to them wait as the guest's do (Memory::MemorySystem::SetHostGuard).
    void EnablePageHolds();
    /// A guest access aborted on the page at host address `host`: whether a page hold refused
    /// it. If so, the GPU work it waited for is done and the guest may run the access again.
    /// `guest_pc` and `guest_va` are the abort's instruction and address (for `holdlog`).
    bool GuestAbort(const u8* host, bool write, u32 guest_pc, u32 guest_va);
    /// After an abort no hold refused (GuestAbort false): the holds' changes sent, whether the
    /// page is open to the access now (RenderThread::AccessOpenAfterFlush).
    bool GuestAccessOpen(const u8* host, bool write);
    /// Whether an aborted guest access at host opened its page, held for the display alone,
    /// the `reach` bytes its instruction may touch from host on being clear of the display's
    /// bytes (renderer_sgxd's PageHolds::OpenPastDisplay).
    bool GuestOpensDisplayPage(const u8* host, bool write, u32 reach);
    /// Whether an aborted guest read of [host, host + size), on one page, misses the bytes the
    /// GPU's queued work writes there, so it can be made at once with the page still held;
    /// write_ticket names that work (the page's last write hold).
    bool GuestReadMissesWrites(const u8* host, u32 size, u32& write_ticket);
    /// Whether an aborted guest store to [host, host + size), on one page, misses the bytes
    /// the GPU's queued work reads and writes there, so it can be made at once with the page
    /// still held; ticket names the page's last live hold (renderer_sgxd's
    /// PageHolds::StoreMisses).
    bool GuestStoreMissesHolds(const u8* host, u32 size, u32& ticket);
    /// Whether an aborted guest access of up to [host, host + size), on one page the guest keeps
    /// accessing past its holds, opened that page without a wait (renderer_sgxd's
    /// RenderThread::OpensHeldPage: a store's version; a picture's rows written back, the page
    /// opening later): the access runs again natively, and so do the guest's next ones there.
    bool GuestOpensHeldPage(const u8* host, u32 size, bool write);
    /// The bytes of the page a store GuestStoreMissesHolds passed must miss, as six [lo, hi) page
    /// offsets (hi 0 for none), for the native core's store permit at `slot`; the permits' table,
    /// whose permits a page's next hold voids one by one, and their generation (renderer_sgxd's
    /// PageHolds::StoreForbidden, PermitGranted, SetStorePermits, Generation).
    void GuestStoreForbidden(const u8* host, std::array<u16, 6>& lo, std::array<u16, 6>& hi,
                             u32 slot);
    void SetStorePermits(AzaharStorePermits* table);
    const u32* PageHoldsGeneration() const;

    /**
     * With the sgxd renderer, its render thread held (RenderThread::Hold): for the guard's life the
     * rasterizer, the renderer and sgxd are the caller's, with everything queued before done. For
     * the work that cannot run on the render thread or beside it: FCRAM page moves (the guest
     * stopped), the frontend's presents and settings, savestates, `perflog`'s counters.
     */
    class RenderHold {
    public:
        explicit RenderHold(GPU& gpu);
        ~RenderHold();
        RenderHold(const RenderHold&) = delete;
        RenderHold& operator=(const RenderHold&) = delete;

    private:
        GPU& gpu;
    };
    [[nodiscard]] RenderHold HoldRender() {
        return RenderHold{*this};
    }
    /// The render thread, the rasterizer the PICA calls (renderer_sgxd/render_thread.h).
    [[nodiscard]] SgxdRenderer::RenderThread& GetRenderThread();
    /// The vblank's timer event (VBlankCallback): the frame's end, the presents and the page
    /// holds' remaps.
    [[nodiscard]] const Core::TimingEventType* VBlankEventType() const;

    /// Switches the rasterizer's disk shader cache to the title's, in order with the GPU's work.
    void SwitchDiskResources(u64 title_id);

    /// What a GPU operation is, for `perflog`'s split of the GPU's work and vprof's numbering.
    enum class OpKind : u32 { CmdList, RegWrite, Fill, Transfer, Dma, Count };

#ifdef VITA_DIAGNOSTICS
    /// The guest goes idle (every core waiting: its clock jumps to the next event). Each command
    /// list's scan since the last call is held against the emulation thread's time between the
    /// scan's end and this idle: what a scan off the emulation thread could hide, if the guest
    /// waited for nothing but the list's P3D here (diagnostics: `perflog`).
    void GuestIdle();
    /// The page holds' record agrees with the guest's view as the guest goes on (`where`).
    void CheckHoldsSent(const char* where);
    /// The page holds' record of the page at `host`, for a guest fault's report.
    std::string GuestHoldState(const u8* host);
    struct ScanRoom {
        u32 lists;               ///< scans an idle has followed
        u64 scan_us;             ///< their time on the emulation thread
        u64 hidden_us;           ///< of it, what the time before the idle covers
        std::array<u32, 5> gaps; ///< scan end to idle: under 0.5, 2, 8, 16 ms, longer
        u32 pending;             ///< scans no idle has followed yet
    };
    ScanRoom TakeScanRoom();
#endif

    /**
     * With no title (Core::System::InitGpuOnly): `work` runs on this thread with the render
     * thread held and the GPU's interrupts muted, its GSP commands given physical addresses
     * (ExecutePhysical); then the LCD's screens go to the window as a vblank shows them
     * (RendererSgxd::PresentFrame), the GPU idle after. The game list's banners.
     */
    void HostFrame(const std::function<void()>& work);
    /// In HostFrame's `work`: a GSP command whose addresses are physical.
    void ExecutePhysical(const Service::GSP::Command& command);
    /// In HostFrame's `work`: the top screen's framebuffer, as the LCD's registers take it (the
    /// left eye's first buffer, shown): its physical address, colour format and column stride.
    void SetHostTopScreen(PAddr address, u32 format, u32 stride);
    /// In HostFrame's `work`: the host wrote guest memory there (through
    /// Memory::GetPhysicalPointer): the textures over it are untiled again at their next use.
    void HostWrote(PAddr address, u32 size);

    /// vprof's replay: the GSP commands it executes carry physical addresses already, which
    /// VirtualToPhysicalAddress passes through.
    bool replay_physical{};
#ifdef VITA_DIAGNOSTICS
    /// vprof's replay (vprof_replay.h) in the replay player, with no guest at all
    /// (Core::System::InitGpuOnly). `first` is the first batch of requests; more come from
    /// host0:vprof-replay.txt until one quits.
    void VprofReplay(const std::string& capture, const std::string& first);
#endif

private:
    void SubmitCmdList(u32 index);

    /// With the render thread held and no title: the LCD's screens to the window as
    /// VBlankCallback shows them, then the GPU idle (the replay's presents, HostFrame's).
    void PresentScreensNow();

    /// Runs fill unit `index` if triggered and raises `interrupt` for it (none: GSP's command
    /// raises one for both of its fills).
    void MemoryFill(u32 index, std::optional<Service::GSP::InterruptId> interrupt);

    void MemoryTransfer();
    void RunMemoryTransfer(const Pica::DisplayTransferConfig& config, bool allow_display_transfer);

    void VBlankCallback(uintptr_t user_data, s64 cycles_late);

    /// Has the renderer keep [addr, addr + size) where the CPU reaches it uncached, if it would
    /// (RendererSgxd::WouldMakeUncached), before the GPU writes or shows it.
    void KeepUncached(PAddr addr, u32 size);

    /// A DMA's piece that is contiguous in physical memory on both sides.
    struct DmaRun {
        PAddr src;
        PAddr dst;
        u32 size;
    };
    /// GSP's DMA of size bytes from src to dst (the current process's addresses) as physical
    /// runs. Memory outside guest FCRAM and VRAM stops the emulator.
    std::vector<DmaRun> DmaRuns(VAddr src, VAddr dst, u32 size);
    /// A DMA's runs, queued to the PTLA; the DMA interrupt with upstream's delay for it.
    void RunDma(const std::vector<DmaRun>& runs, u64 delay_ns);

    /// Runs op, a GSP command's or a register trigger's work (vprof numbers it, perflog times
    /// it), then lets the rasterizer start what it held back (RasterizerInterface::GpuIdle).
    void RunGpuOp(OpKind kind, const std::function<void()>& op);
    /// A write of [addr, addr + size): one into a framebuffer the LCD shows has the next vblank
    /// present (VBlankCallback).
    void ScreensWritten(PAddr addr, u32 size);
    /// Raises a GSP interrupt as upstream does: when its work is submitted, after the delay
    /// generators' estimate of that work, which GSP adds. Any but a vblank
    /// first takes the rasterizer's ticket for the work before it, which the page holds made
    /// for that work wait for (SgxdRenderer::PageHolds): the guest hears of the work before the
    /// SGX has done it, and a guest access to its pages waits. None while vprof's replay runs.
    void Signal(Service::GSP::InterruptId id, u64 delay_ns);
    /// vprof's capture records its frame with the render thread held, so that the frame's ops
    /// and the draws they make interleave as they run: on from the vblank the frame starts at
    /// to the one it ends at.
    void SyncCaptureHold();
    bool capture_hold = false;

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

    std::unique_ptr<RightEyeDisabler> right_eye_disabler;

private:
    friend class RightEyeDisabler;
    struct Impl;
    std::unique_ptr<Impl> impl;

    /// Translates the address of a GPU buffer as the GSP module does, or returns std::nullopt when
    /// the buffer is not in GPU visible memory.
    std::optional<PAddr> VirtualToPhysicalAddress(VAddr addr, u32 size);
};

} // namespace VideoCore

BOOST_CLASS_VERSION(VideoCore::GPU, 1)

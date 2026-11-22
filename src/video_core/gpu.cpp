// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstring>
#include "common/archives.h"
#include "common/hacks/hack_manager.h"
#include "common/microprofile.h"
#include "common/process_clock.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/loader/loader.h"
#include "video_core/gpu.h"
#include "video_core/gpu_debugger.h"
#include "video_core/gpu_impl.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_sgxd/vprof.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"
#include "video_core/renderer_sgxd/render_thread.h"
#include "video_core/renderer_sgxd/renderer_sgxd.h"
#ifdef VITA_DIAGNOSTICS
#include "video_core/renderer_sgxd/vprof_replay.h"
#endif
#include "video_core/right_eye_disabler.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#define GPU_TRACE(...) SgxdRenderer::SgxdTrace(__VA_ARGS__)

namespace VideoCore {

constexpr VAddr VADDR_LCD = 0x1ED02000;
constexpr VAddr VADDR_GPU = 0x1EF00000;

// Upstream's: how long the 3DS's transfer engines take, which GSP adds to their
// interrupts.
class DelayGenerator {
private:
    DelayGenerator() = default;

    // Average transfer speed based on measurements taken from real
    // hardware. 4 different modes have been taken into consideration:
    // RAM -> RAM, RAM -> VRAM, VRAM -> RAM and VRAM -> VRAM.
    // Furthermore, measurements are split into DMA transfers and tex
    // copies. For simplicity, we will assume fills are as fast as
    // texture copies.

    static constexpr double mibps_to_ns_per_byte(double mib_per_sec) {
        return 1'000'000'000.0 / (mib_per_sec * 1024.0 * 1024.0);
    }

    static constexpr std::array<std::array<double, 4>, 2> speed_mibps = {
        {{
             190.0, // DMA RAMTORAM
             310.0, // DMA RAMTOVRAM
             380.0, // DMA VRAMTORAM
             380.0, // DMA VRAMTOVRAM
         },
         {
             450.0,  // TEX RAMTORAM
             3100.0, // TEX RAMTOVRAM
             5400.0, // TEX VRAMTORAM
             5400.0, // TEX VRAMTOVRAM
         }}};

public:
    enum class CopyMode {
        RAMTORAM,
        RAMTOVRAM,
        VRAMTORAM,
        VRAMTOVRAM,
    };

    static CopyMode GetCopyMode(bool input_vram, bool output_vram) {
        if (!input_vram && !output_vram) {
            return CopyMode::RAMTORAM;
        } else if (!input_vram && output_vram) {
            return CopyMode::RAMTOVRAM;
        } else if (input_vram && !output_vram) {
            return CopyMode::VRAMTORAM;
        } else {
            return CopyMode::VRAMTOVRAM;
        }
    }

    static u64 CalculateDelayNanoseconds(CopyMode mode, bool is_textre, size_t size) {
        double base_ns_per_byte =
            mibps_to_ns_per_byte(speed_mibps[is_textre][static_cast<u32>(mode)]);

        return static_cast<u64>(size * base_ns_per_byte);
    }
};

MICROPROFILE_DEFINE(GPU_DisplayTransfer, "GPU", "DisplayTransfer", MP_RGB(100, 100, 255));
MICROPROFILE_DEFINE(GPU_CmdlistProcessing, "GPU", "Cmdlist Processing", MP_RGB(100, 255, 100));

GPU::GPU(Core::System& system, Frontend::EmuWindow& emu_window,
         Frontend::EmuWindow* secondary_window)
    : right_eye_disabler{std::make_unique<RightEyeDisabler>(*this)},
      impl{std::make_unique<Impl>(system, emu_window, secondary_window)} {
    impl->vblank_event = impl->timing.RegisterEvent(
        "GPU::VBlankCallback",
        [this](uintptr_t user_data, s64 cycles_late) { VBlankCallback(user_data, cycles_late); });
    impl->timing.ScheduleEvent(FRAME_TICKS, impl->vblank_event);

    // The PICA raises P3D from inside a command list, when the list's IRQ request comes, with
    // its delay generator's estimate of the list.
    Service::GSP::InterruptHandler pica_interrupt = [this](Service::GSP::InterruptId id,
                                                           u64 delay_ns) { Signal(id, delay_ns); };
    impl->pica.SetInterruptHandler(pica_interrupt);
    // The renderer's rasterizer runs on the render thread, from its copy of the PICA's state;
    // the PICA here calls the thread, which queues.
    impl->render = std::make_unique<SgxdRenderer::RenderThread>(
        impl->pica, impl->parse_pica, impl->render_pica, *impl->renderer,
        impl->renderer->Rasterizer(), impl->memory, *right_eye_disabler);
    impl->rasterizer = impl->render.get();

    // Bind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);
}

GPU::~GPU() {
    // Before the lock it uses goes, and the renderer it runs.
    impl->render.reset();
}

GPU::RenderHold::RenderHold(GPU& gpu_) : gpu{gpu_} {
    gpu.impl->render->Hold();
}

GPU::RenderHold::~RenderHold() {
    gpu.impl->render->Release();
}

SgxdRenderer::RenderThread& GPU::GetRenderThread() {
    return *impl->render;
}

const Core::TimingEventType* GPU::VBlankEventType() const {
    return impl->vblank_event;
}

void GPU::SyncCaptureHold() {
    const bool recording = SgxdRenderer::Vprof::Recording();
    if (recording && !capture_hold) {
        impl->render->Hold();
        capture_hold = true;
    } else if (!recording && capture_hold) {
        impl->render->Release();
        capture_hold = false;
    }
}

void GPU::RunGpuOp([[maybe_unused]] OpKind kind, const std::function<void()>& op) {
#ifndef VITA_DIAGNOSTICS
    op();
#else
    // The time by kind is `perflog`'s, in the diagnostics build only: the emulation side's
    // (the PICA's parse and the queueing), the render thread's work is its own line.
    // vprof: the op's number in the recorded frame, 0 outside it.
    const u32 seq = SgxdRenderer::Vprof::OpQueued(static_cast<u32>(kind));
    const auto start = Common::ProcessClock::now();
    GPU_TRACE("op", static_cast<unsigned>(kind));
    SgxdRenderer::Vprof::OpBegin(seq);
    op();
    SgxdRenderer::Vprof::OpEnd(seq);
    GPU_TRACE("done");
    const auto end = Common::ProcessClock::now();
    if (kind == OpKind::CmdList) {
        impl->scans_pending.emplace_back(static_cast<u64>(end.time_since_epoch().count()),
                                         static_cast<u32>((end - start).count()));
    }
#endif
    // GPU work the op held back starts now (the last pass's 3D, when a transfer waits on it).
    impl->rasterizer->GpuIdle();
    // The pages the op's work reads and writes, held before the guest runs again.
    impl->render->ApplyPageHolds();
}

void GPU::Signal(Service::GSP::InterruptId id, u64 delay_ns) {
    using Service::GSP::InterruptId;
    if (impl->interrupts_muted) {
        return;
    }
    // Any but a vblank (core timing's cadence, VBlankCallback): the work before it gets a
    // ticket, which ends the page holds that work made (SgxdRenderer::PageHolds).
    if (id != InterruptId::PDC0 && id != InterruptId::PDC1) {
        impl->rasterizer->InterruptFence();
    }
    GPU_TRACE("irq", static_cast<unsigned>(id));
    impl->signal_interrupt(id, delay_ns);
}

void GPU::DeliverInterrupts() {
    impl->render->PollHeld();
    impl->render->ReleasePageHolds();
}

void GPU::EnablePageHolds() {
    auto& render = *impl->render;
    render.EnablePageHolds();
    impl->memory.SetHostGuard({
        render.PageHeldMap(),
        [this](const u8* host, u32 size, bool write) {
            impl->render->HostAccess(host, size, write);
        },
        [this](const u8* host, u32 size) { impl->render->HostRemapped(host, size); },
    });
}

bool GPU::GuestStoreMissesHolds([[maybe_unused]] const u8* host, [[maybe_unused]] u32 size,
                                [[maybe_unused]] u32& ticket) {
    return impl->render->StoreMissesHolds(host, size, ticket);
}

void GPU::GuestStoreForbidden(const u8* host, std::array<u16, 6>& lo, std::array<u16, 6>& hi,
                              u32 slot) {
    impl->render->StoreForbidden(host, lo, hi, slot);
}

void GPU::SetStorePermits(AzaharStorePermits* table) {
    impl->render->SetStorePermits(table);
}

const u32* GPU::PageHoldsGeneration() const {
    return impl->render->PageHoldsGeneration();
}

bool GPU::GuestReadMissesWrites([[maybe_unused]] const u8* host, [[maybe_unused]] u32 size,
                                [[maybe_unused]] u32& write_ticket) {
    return impl->render->ReadMissesWrites(host, size, write_ticket);
}

bool GPU::GuestOpensHeldPage([[maybe_unused]] const u8* host, [[maybe_unused]] u32 size,
                             [[maybe_unused]] bool write) {
    return impl->render->OpensHeldPage(host, size, write);
}

bool GPU::GuestOpensDisplayPage([[maybe_unused]] const u8* host, [[maybe_unused]] bool write,
                                [[maybe_unused]] u32 reach) {
    return impl->render->OpensDisplayPage(host, write, reach);
}

bool GPU::GuestAbort([[maybe_unused]] const u8* host, [[maybe_unused]] bool write,
                     [[maybe_unused]] u32 guest_pc, [[maybe_unused]] u32 guest_va) {
    return impl->render->PageFault(host, write, guest_pc, guest_va);
}

#ifdef VITA_DIAGNOSTICS
void GPU::CheckHoldsSent(const char* where) {
    impl->render->CheckHoldsSent(where);
}

std::string GPU::GuestHoldState(const u8* host) {
    return impl->render->HoldState(host);
}

void GPU::GuestIdle() {
    if (impl->scans_pending.empty()) {
        return;
    }
    const u64 now = static_cast<u64>(Common::ProcessClock::now().time_since_epoch().count());
    auto& room = impl->scan_room;
    for (const auto& [end, scan_us] : impl->scans_pending) {
        const u64 gap = now > end ? now - end : 0;
        room.lists++;
        room.scan_us += scan_us;
        room.hidden_us += std::min<u64>(scan_us, gap);
        room.gaps[gap < 500 ? 0 : gap < 2000 ? 1 : gap < 8000 ? 2 : gap < 16000 ? 3 : 4]++;
    }
    impl->scans_pending.clear();
}

GPU::ScanRoom GPU::TakeScanRoom() {
    ScanRoom r = std::exchange(impl->scan_room, {});
    r.pending = static_cast<u32>(impl->scans_pending.size());
    return r;
}
#endif

constexpr Result ResultInvalidGpuBuffer(ErrorDescription::InvalidAddress, ErrorModule::GX,
                                        ErrorSummary::InvalidArgument,
                                        ErrorLevel::Usage); // 0xE0E02BF5
constexpr Result ResultUnknownGxCommand(ErrorDescription::NotImplemented, ErrorModule::GX,
                                        ErrorSummary::NotSupported,
                                        ErrorLevel::Usage); // 0xE0C02BF4

std::optional<PAddr> GPU::VirtualToPhysicalAddress(VAddr addr, u32 size) {
    // A replay's commands carry physical addresses already (vprof).
    if (replay_physical) {
        return addr;
    }
    // Address translation of the GSP module.
    // The linear heaps and the New 3DS extra memory must contain both the start,
    // and the end of the buffer, VRAM only the start. The ends of the
    // ranges are inclusive.
    const VAddr end = addr + size;
    const auto contains = [addr, end](VAddr range_start, VAddr range_end) {
        return addr >= range_start && addr <= range_end && end >= range_start && end <= range_end;
    };

    if (contains(Memory::LINEAR_HEAP_VADDR, Memory::LINEAR_HEAP_VADDR_END)) {
        return addr - Memory::LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    if (contains(Memory::NEW_LINEAR_HEAP_VADDR, Memory::NEW_LINEAR_HEAP_VADDR_END)) {
        return addr - Memory::NEW_LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    if (addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END) {
        return addr - Memory::VRAM_VADDR + Memory::VRAM_PADDR;
    }
    if (contains(Memory::N3DS_EXTRA_RAM_VADDR, Memory::N3DS_EXTRA_RAM_VADDR_END)) {
        return addr - Memory::N3DS_EXTRA_RAM_VADDR + Memory::N3DS_EXTRA_RAM_PADDR;
    }

    // TODO(PabloMK7): Find a better way to handle this.
    PAddr plg_fb_addr;
    if (contains(Memory::PLUGIN_3GX_FB_VADDR, Memory::PLUGIN_3GX_FB_VADDR_END) &&
        (plg_fb_addr = impl->system.Memory().Plugin3GXFramebufferAddress())) {
        return addr - Memory::PLUGIN_3GX_FB_VADDR + plg_fb_addr;
    }

    LOG_ERROR(HW_GPU, "Invalid GPU buffer address 0x{:08X} (size 0x{:X})", addr, size);
    return std::nullopt;
}

void GPU::SetInterruptHandler(Service::GSP::InterruptHandler handler) {
    impl->signal_interrupt = handler;
}

void GPU::FlushRegion(PAddr addr, u32 size) {
    impl->rasterizer->FlushRegion(addr, size);
}

void GPU::InvalidateRegion(PAddr addr, u32 size) {
    ScreensWritten(addr, size);
    impl->rasterizer->InvalidateRegion(addr, size);
}

void GPU::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    ScreensWritten(addr, size);
    impl->rasterizer->FlushAndInvalidateRegion(addr, size);
}

void GPU::ScreensWritten([[maybe_unused]] PAddr addr, [[maybe_unused]] u32 size) {
    // The LCD scans 400 columns on the top screen, 320 on the bottom (as VBlankCallback).
    for (u32 i = 0; i < 2; i++) {
        const Pica::FramebufferConfig& f = impl->pica.regs.framebuffer_config[i];
        const PAddr shown = f.second_fb_active ? f.address_left2 : f.address_left1;
        if (addr < shown + f.stride * (i == 0 ? 400u : 320u) && shown < addr + size) {
            impl->screens_changed = true;
        }
    }
}

void GPU::SwitchDiskResources(u64 title_id) {
    impl->rasterizer->SwitchDiskResources(title_id);
}

void GPU::ClearAll(bool flush) {
    impl->rasterizer->ClearAll(flush);
}

Result GPU::Execute(const Service::GSP::Command& command) {
    using Service::GSP::CommandId;
    auto& regs = impl->pica.regs;
#ifdef VITA_DIAGNOSTICS
    // vprof: the command as the PICA's registers take it, its addresses physical (a DMA's
    // physical runs are recorded where they are made).
    if (SgxdRenderer::Vprof::Recording() && command.id != CommandId::RequestDma &&
        !replay_physical) {
        Service::GSP::Command p = command;
        const auto phys = [this](u32& a) {
            if (a != 0) {
                a = VirtualToPhysicalAddress(a, 1).value_or(0);
            }
        };
        switch (command.id) {
        case CommandId::SubmitCmdList:
            phys(p.submit_gpu_cmdlist.address);
            break;
        case CommandId::MemoryFill:
            phys(p.memory_fill.start1);
            phys(p.memory_fill.end1);
            phys(p.memory_fill.start2);
            phys(p.memory_fill.end2);
            break;
        case CommandId::DisplayTransfer:
            phys(p.display_transfer.in_buffer_address);
            phys(p.display_transfer.out_buffer_address);
            break;
        case CommandId::TextureCopy:
            phys(p.texture_copy.in_buffer_address);
            phys(p.texture_copy.out_buffer_address);
            break;
        default:
            break;
        }
        std::array<u32, 8> words;
        std::memcpy(words.data(), &p, sizeof(words));
        SgxdRenderer::Vprof::Gsp(words.data());
    }
#endif
    Result result = ResultSuccess;

    switch (command.id) {
    case CommandId::RequestDma: {
        impl->system.Memory().RasterizerFlushVirtualRegion(
            command.dma_request.source_address, command.dma_request.size, Memory::FlushMode::Flush);
        impl->system.Memory().RasterizerFlushVirtualRegion(command.dma_request.dest_address,
                                                           command.dma_request.size,
                                                           Memory::FlushMode::Invalidate);
        // The PTLA copies it, run by run.
        auto runs = DmaRuns(command.dma_request.source_address, command.dma_request.dest_address,
                            command.dma_request.size);

        auto is_vram = [&](u32 addr) {
            return addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END;
        };
        const u64 delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(is_vram(command.dma_request.source_address),
                                        is_vram(command.dma_request.dest_address)),
            false, command.dma_request.size);
        RunDma(runs, delay);
        break;
    }
    case CommandId::SubmitCmdList: {
        auto& params = command.submit_gpu_cmdlist;
        // The GSP module uses address 0 when the buffer is not in GPU visible memory.
        const PAddr address = VirtualToPhysicalAddress(params.address, params.size).value_or(0);
        const u32 size = params.size;
        RunGpuOp(OpKind::CmdList, [this, &regs, address, size] {
            auto& cmdbuffer = regs.internal.pipeline.command_buffer;

            // Write to the command buffer GPU registers
            cmdbuffer.addr[0].Assign(address >> 3);
            cmdbuffer.size[0].Assign(size >> 3);
            cmdbuffer.trigger[0] = 1;
            // The render thread's PicaCore takes them too, in order with the lists.
            for (const u32 id : {PICA_REG_INDEX(pipeline.command_buffer.addr[0]),
                                 PICA_REG_INDEX(pipeline.command_buffer.size[0]),
                                 PICA_REG_INDEX(pipeline.command_buffer.trigger[0])}) {
                impl->render->RegWrite(id, regs.internal.reg_array[id]);
            }

            // Trigger processing of the command list
            SubmitCmdList(0);
        });
        break;
    }
    case CommandId::MemoryFill: {
        auto& params = command.memory_fill;
        auto& memfill = regs.memory_fill_config;

        // Write to the memory fill GPU registers. GSP skips a buffer whose start is 0, and
        // with both buffers set it raises PSC0 alone (3dbrew GSP Shared Memory, command 2):
        // here behind the second fill.
        using Service::GSP::InterruptId;
        const auto translate = [this](u32 start, u32 end) -> std::optional<PAddr> {
            if (start >= end) {
                return std::nullopt;
            }
            return VirtualToPhysicalAddress(start, end - start);
        };
        const bool has_both_bufs = params.start1 != 0 && params.start2 != 0;
        if (params.start1 != 0) {
            const auto start = translate(params.start1, params.end1);
            if (!start) {
                result = ResultInvalidGpuBuffer;
                break;
            }
            memfill[0].address_start = *start >> 3;
            memfill[0].address_end = (*start + params.end1 - params.start1) >> 3;
            memfill[0].value_32bit = params.value1;
            memfill[0].control = params.control1;
            MemoryFill(0, has_both_bufs ? std::nullopt : std::optional{InterruptId::PSC0});
        }
        if (params.start2 != 0) {
            const auto start = translate(params.start2, params.end2);
            if (!start) {
                result = ResultInvalidGpuBuffer;
                break;
            }
            memfill[1].address_start = *start >> 3;
            memfill[1].address_end = (*start + params.end2 - params.start2) >> 3;
            memfill[1].value_32bit = params.value2;
            memfill[1].control = params.control2;
            MemoryFill(1, has_both_bufs ? InterruptId::PSC0 : InterruptId::PSC1);
        }
        break;
    }
    case CommandId::DisplayTransfer: {
        auto& params = command.display_transfer;
        auto& display_transfer = regs.display_transfer_config;

        // Write to the transfer engine GPU registers.
        // The GSP module uses address 0 when the buffer is not in GPU visible memory (only the
        // first byte is checked).
        display_transfer.input_address =
            VirtualToPhysicalAddress(params.in_buffer_address, 1).value_or(0) >> 3;
        display_transfer.output_address =
            VirtualToPhysicalAddress(params.out_buffer_address, 1).value_or(0) >> 3;
        display_transfer.input_size = params.in_buffer_size;
        display_transfer.output_size = params.out_buffer_size;
        display_transfer.flags = params.flags;
        display_transfer.trigger.Assign(1);

        // Trigger the display transfer.
        MemoryTransfer();
        break;
    }
    case CommandId::TextureCopy: {
        auto& params = command.texture_copy;
        auto& texture_copy = regs.display_transfer_config;

        // Write to the transfer engine GPU registers.
        // The GSP module uses address 0 when the buffer is not in GPU visible memory.
        texture_copy.input_address =
            VirtualToPhysicalAddress(params.in_buffer_address, params.size).value_or(0) >> 3;
        texture_copy.output_address =
            VirtualToPhysicalAddress(params.out_buffer_address, params.size).value_or(0) >> 3;
        texture_copy.texture_copy.size = params.size;
        texture_copy.texture_copy.input_size = params.in_width_gap;
        texture_copy.texture_copy.output_size = params.out_width_gap;
        texture_copy.flags = params.flags;
        texture_copy.trigger.Assign(1);

        // Trigger the texture copy.
        MemoryTransfer();
        break;
    }
    case CommandId::CacheFlush: {
        // Rasterizer flushing handled elsewhere in CPU read/write and other GPU handlers
        // Use command.cache_flush.regions to implement this handler
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Unknown command {:#08X}", command.id.Value());
        result = ResultUnknownGxCommand;
    }

    // Notify debugger that a GSP command was processed.
    return result;
}

void GPU::SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info) {
    // The GSP module does not apply a framebuffer outside GPU visible memory. The right address
    // is used only for the top screen in stereoscopic mode and when it is not zero.
    // Otherwise the left address is used for both eyes.
    const auto phys_address_left = VirtualToPhysicalAddress(info.address_left, info.stride);
    if (!phys_address_left) {
        return;
    }

    // Update framebuffer properties.
    auto& framebuffer = impl->pica.regs.framebuffer_config[screen_id];
    if (screen_id == 0) {
        const bool stereoscopic = (info.format & 0x70) == 0x20;
        const PAddr phys_address_right =
            (!stereoscopic || info.address_right == 0)
                ? *phys_address_left
                : VirtualToPhysicalAddress(info.address_right, info.stride).value_or(0);
        if (info.active_fb == 0) {
            framebuffer.address_right1 = phys_address_right;
        } else {
            framebuffer.address_right2 = phys_address_right;
        }
    }
    if (info.active_fb == 0) {
        framebuffer.address_left1 = *phys_address_left;
    } else {
        framebuffer.address_left2 = *phys_address_left;
    }

    framebuffer.stride = info.stride;
    framebuffer.format = (framebuffer.format & 0xFFFF0000) | (info.format & 0xFFFF);
    framebuffer.active_fb = info.shown_fb & 0xFF;
    // The guest's frame, even to the buffer already shown: written again.
    impl->screens_changed = true;
    // vprof: the swap as the register writes it is.
    if (SgxdRenderer::Vprof::Recording()) {
        const u32 first = GPU_REG_INDEX(framebuffer_config[0]) +
                          screen_id * static_cast<u32>(sizeof(Pica::FramebufferConfig) / 4);
        for (u32 i = 0; i < sizeof(Pica::FramebufferConfig) / 4; i++) {
            SgxdRenderer::Vprof::RegWrite(first + i, impl->pica.regs.reg_array[first + i], false);
        }
    }

    // Notify debugger about the buffer swap.

    if (screen_id == 0) {
        MicroProfileFlip();
        impl->system.perf_stats->EndGameFrame();
        right_eye_disabler->ReportEndFrame();
    }
}

void GPU::SetColorFill(const Pica::ColorFill& fill) {
    impl->pica.regs_lcd.color_fill_top = fill;
    impl->pica.regs_lcd.color_fill_bottom = fill;
}

u32 GPU::ReadReg(VAddr addr) {
    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        return impl->pica.regs_lcd[index];
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        return impl->pica.regs.reg_array[index];
    }
    default:
        UNREACHABLE_MSG("Read from unknown GPU address {:#08X}", addr);
    }
}

void GPU::WriteReg(VAddr addr, u32 data) {
    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        impl->pica.regs_lcd[index] = data;
        SgxdRenderer::Vprof::RegWrite(index, data, true);
        break;
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);

        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        SgxdRenderer::Vprof::RegWrite(index, data, false);
        if (index >= GPU_REG_INDEX(internal)) {
            // An op like the GSP's commands, numbered with them in a vprof recording.
            RunGpuOp(OpKind::RegWrite, [this, index, data] {
                impl->pica.regs.reg_array[index] = data;
                // The render thread's PicaCore takes it too, in order with the lists.
                impl->render->RegWrite(index - static_cast<u32>(GPU_REG_INDEX(internal)), data);
                if (index == GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[0])) {
                    SubmitCmdList(0);
                } else if (index == GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[1])) {
                    SubmitCmdList(1);
                }
            });
            break;
        }
        impl->pica.regs.reg_array[index] = data;

        // Handle registers that trigger GPU actions
        switch (index) {
        case GPU_REG_INDEX(memory_fill_config[0].trigger):
            MemoryFill(0, Service::GSP::InterruptId::PSC0);
            break;
        case GPU_REG_INDEX(memory_fill_config[1].trigger):
            MemoryFill(1, Service::GSP::InterruptId::PSC1);
            break;
        case GPU_REG_INDEX(display_transfer_config.trigger):
            MemoryTransfer();
            break;
        default:
            break;
        }
        break;
    }
    default:
        UNREACHABLE_MSG("Write to unknown GPU address {:#08X}", addr);
    }
}

SgxdRenderer::RendererSgxd& GPU::Renderer() {
    return *impl->renderer;
}

void GPU::UpdateFramebufferLayout() {
    impl->renderer->UpdateCurrentFramebufferLayout();
}

Pica::PicaCore& GPU::PicaCore() {
    return impl->pica;
}

const Pica::PicaCore& GPU::PicaCore() const {
    return impl->pica;
}

GraphicsDebugger& GPU::Debugger() {
    return impl->gpu_debugger;
}

void GPU::ApplyPerProgramSettings(u64 program_ID) {
    auto hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::ACCURATE_MULTIPLICATION, program_ID);
    bool use_accurate_mul = Settings::values.shaders_accurate_mul.GetValue();
    if (hack) {
        switch (hack->mode) {
        case Common::Hacks::HackAllowMode::DISALLOW:
            use_accurate_mul = false;
            break;
        case Common::Hacks::HackAllowMode::FORCE:
            use_accurate_mul = true;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    impl->render->SetRendererAccurateMul(use_accurate_mul);
}

void GPU::SubmitCmdList(u32 index) {
    // Check if a command list was triggered.
    auto& config = impl->pica.regs.internal.pipeline.command_buffer;
    if (!config.trigger[index]) {
        return;
    }

    MICROPROFILE_SCOPE(GPU_CmdlistProcessing);

    // Forward command list processing to the PICA core.
    const PAddr addr = config.GetPhysicalAddress(index);
    const u32 size = config.GetSize(index);

    // Do not process the list if the memory range is not sane.
    auto& memory = impl->system.Memory();
    const u8* head = memory.GetPhysicalPointer(addr);
    if (!head || (size != 0 && memory.GetPhysicalPointer(addr + size - 1) != head + size - 1)) {
        LOG_ERROR(HW_GPU, "Invalid command list address 0x{:08X} (size 0x{:X})", addr, size);
        impl->signal_interrupt(Service::GSP::InterruptId::P3D, 0);
        config.trigger[index] = 0;
        return;
    }

    const bool ignore = !right_eye_disabler->ShouldAllowCmdQueueTrigger(addr, size);
    // Scanned here, parsed on the render thread (RenderThread::SubmitCmdList); P3D once the list
    // is queued, so its fence's ticket covers the list's work.
    std::vector<u64> irqs;
    impl->render->SubmitCmdList(addr, size, ignore, irqs);
    for (const u64 delay_ns : irqs) {
        Signal(Service::GSP::InterruptId::P3D, delay_ns);
    }
    config.trigger[index] = 0;
    const u32 trigger = PICA_REG_INDEX(pipeline.command_buffer.trigger[0]) + index;
    impl->render->RegWrite(trigger, impl->pica.regs.internal.reg_array[trigger]);
}

void GPU::MemoryFill(u32 index, std::optional<Service::GSP::InterruptId> interrupt) {
    // Check if a memory fill was triggered.
    auto& config = impl->pica.regs.memory_fill_config[index];
    if (!config.trigger) {
        return;
    }
    // Upstream raised no interrupt for a fill from address 0, untested on hardware; the fill
    // unit raises its interrupt whenever it finishes (3dbrew GPU/External Registers). A fill
    // from 0 is not VRAM, so it is a fault to look at, not a case to handle.
    DEBUG_ASSERT_MSG(config.GetStartAddress() != 0, "GPU: memory fill {} from address 0 to {:#x}",
                     index, config.GetEndAddress());

    ScreensWritten(config.GetStartAddress(), config.GetEndAddress() - config.GetStartAddress());
    // Perform memory fill: the PTLA's, queued behind the renders before it.
    RunGpuOp(OpKind::Fill, [this, &config] {
        if (!impl->rasterizer->AccelerateFill(config)) {
            UNREACHABLE_MSG("GPU: the fill of {:#x}-{:#x} was not done on the GPU",
                            config.GetStartAddress(), config.GetEndAddress());
        }
    });

    if (interrupt) {
        // Treat fill as texture transfer from VRAM (upstream).
        const u64 delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(true, config.IsVRAM()), true,
            config.GetEndAddress() - config.GetStartAddress());
        Signal(*interrupt, delay);
    }

    // Reset "trigger" flag and set the "finish" flag
    config.trigger.Assign(0);
    config.finished.Assign(1);
}

void GPU::MemoryTransfer() {
    // Check if a transfer was triggered.
    auto& config = impl->pica.regs.display_transfer_config;
    if (!config.trigger.Value()) {
        return;
    }
    // A 2D title names its left buffers in the right registers too.
    const auto into_right_eye = [&] {
        const Pica::FramebufferConfig& top = impl->pica.regs.framebuffer_config[0];
        const PAddr out = config.GetPhysicalOutputAddress();
        const u32 out_bytes = config.OutputBytes();
        const u32 eye_bytes = top.stride * 400u;
        for (const PAddr a : {static_cast<PAddr>(top.address_right1),
                              static_cast<PAddr>(top.address_right2)}) {
            if (a != 0 && a != top.address_left1 && a != top.address_left2 &&
                out < a + eye_bytes && a < out + out_bytes) {
                return true;
            }
        }
        return false;
    };
    const bool allow_display_transfer =
        config.is_texture_copy ||
        right_eye_disabler->ShouldAllowDisplayTransfer(config.GetPhysicalInputAddress(),
                                                       config.input_height, config.output_height,
                                                       into_right_eye());
    // (The output's pages move to CDRAM once the transfer is known to run:
    // RenderThread::AccelerateDisplayTransfer.)
    if (!config.is_texture_copy && allow_display_transfer) {
        ScreensWritten(config.GetPhysicalOutputAddress(), config.OutputBytes());
    } else if (config.is_texture_copy) {
        // Its lines of output_width with output_gap after each, as the copy writes them.
        const u32 width = config.texture_copy.output_width * 16;
        const u32 line = width + config.texture_copy.output_gap * 16;
        const u32 size = config.texture_copy.size;
        ScreensWritten(config.GetPhysicalOutputAddress(),
                       width == 0 ? size : (size + width - 1) / width * line);
    }
    RunGpuOp(OpKind::Transfer,
             [this, &config, allow_display_transfer] {
                 RunMemoryTransfer(config, allow_display_transfer);
             });

    // Complete transfer, with upstream's delay for it.
    const u64 delay =
        config.is_texture_copy
            ? DelayGenerator::CalculateDelayNanoseconds(
                  DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
                  config.texture_copy.size)
            : DelayGenerator::CalculateDelayNanoseconds(
                  DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
                  config.input_width * config.input_height * BytesPerPixel(config.input_format));
    config.trigger.Assign(0);
    Signal(Service::GSP::InterruptId::PPF, delay);
}

void GPU::RunDma(const std::vector<DmaRun>& runs, u64 delay_ns) {
#ifdef VITA_DIAGNOSTICS
    if (SgxdRenderer::Vprof::Recording()) {
        std::vector<u32> words;
        for (const DmaRun& run : runs) {
            words.insert(words.end(), {run.src, run.dst, run.size});
        }
        SgxdRenderer::Vprof::Dma(words.data(), static_cast<u32>(runs.size()));
    }
#endif
    for (const DmaRun& run : runs) {
        ScreensWritten(run.dst, run.size);
    }
    RunGpuOp(OpKind::Dma, [this, &runs] {
        for (const DmaRun& run : runs) {
            SgxdRenderer::Vprof::Memory(SgxdRenderer::Vprof::Mem::DmaSource, run.src, run.size);
            if (!impl->rasterizer->AccelerateDma(run.src, run.dst, run.size)) {
                UNREACHABLE_MSG("GPU: DMA {:#x} to {:#x}, {} bytes: not done on the GPU",
                                run.src, run.dst, run.size);
            }
        }
    });
    Signal(Service::GSP::InterruptId::DMA, delay_ns);
}

#ifdef VITA_DIAGNOSTICS
void GPU::VprofReplay(const std::string& capture, const std::string& first) {
    // The replay runs the GPU's work on this thread, the render thread held: its requests read
    // the rasterizer between ops.
    const auto hold = HoldRender();
    // The guest sees none of the replay's interrupts.
    impl->interrupts_muted = true;
    auto& rasterizer = impl->render->Rasterizer();
    SgxdRenderer::Vprof::ReplayHost host{
        impl->pica,
        impl->memory,
        rasterizer,
        [this](const u32* words) {
            Service::GSP::Command command;
            std::memcpy(&command, words, sizeof(u32) * 8);
            replay_physical = true;
            Execute(command);
            replay_physical = false;
        },
        [this](u32 index, u32 value, bool lcd) {
            WriteReg((lcd ? VADDR_LCD : VADDR_GPU) + index * 4, value);
        },
        [this](const std::vector<u32>& triples) {
            std::vector<DmaRun> runs;
            for (std::size_t i = 0; i + 2 < triples.size(); i += 3) {
                runs.push_back({triples[i], triples[i + 1], triples[i + 2]});
            }
            RunDma(runs, 0);
        },
        [this](std::function<void()> fn) { RunGpuOp(OpKind::RegWrite, fn); },
        [](std::function<void()> fn) { fn(); },
        // The screens as VBlankCallback shows them.
        [this] {
            const auto& regs = impl->pica.regs;
            for (u32 i = 0; i < 2; i++) {
                const Pica::FramebufferConfig& f = regs.framebuffer_config[i];
                KeepUncached(f.second_fb_active ? f.address_left2 : f.address_left1,
                             f.stride * (i == 0 ? 400u : 320u), false);
            }
            const std::array<Pica::FramebufferConfig, 2> fb{regs.framebuffer_config[0],
                                                            regs.framebuffer_config[1]};
            const std::array<Pica::ColorFill, 2> fill{impl->pica.regs_lcd.color_fill_top,
                                                      impl->pica.regs_lcd.color_fill_bottom};
            // PresentFrame waits for the flip. RendererSgxd::EndFrame is left out: its frame
            // limiter and PerfStats belong to a running guest, and the player has neither.
            impl->renderer->PresentFrame(fb, fill);
            impl->rasterizer->GpuIdle();
        },
        [this] { right_eye_disabler->ReportEndFrame(); },
        [this] { impl->render->TakeWholeState(); },
    };
    SgxdRenderer::Vprof::Serve(host, capture, first);
    impl->interrupts_muted = false;
}
#endif

std::vector<GPU::DmaRun> GPU::DmaRuns(VAddr src, VAddr dst, u32 size) {
    // GSP copies through the process's mapping, the PTLA through the GPU's, where guest FCRAM
    // and VRAM sit at their PICA physical addresses.
    const u8* const fcram = impl->memory.GetFCRAMPointer(0);
    const u8* const vram = impl->memory.GetPhysicalPointer(Memory::VRAM_PADDR);
    const auto physical = [&](VAddr vaddr) -> PAddr {
        const u8* const host = impl->memory.GetPointer(vaddr);
        if (host >= fcram && host < fcram + Memory::FCRAM_SIZE) {
            return Memory::FCRAM_PADDR + static_cast<u32>(host - fcram);
        }
        if (host >= vram && host < vram + Memory::VRAM_SIZE) {
            return Memory::VRAM_PADDR + static_cast<u32>(host - vram);
        }
        UNREACHABLE_MSG("GPU: DMA from {:#x} to {:#x}: {:#x} is not guest FCRAM or VRAM", src, dst,
                        vaddr);
    };
    std::vector<DmaRun> runs;
    for (u32 done = 0; done < size;) {
        const VAddr s = src + done, d = dst + done;
        const u32 chunk =
            std::min({size - done, Memory::CITRA_PAGE_SIZE - (s & Memory::CITRA_PAGE_MASK),
                      Memory::CITRA_PAGE_SIZE - (d & Memory::CITRA_PAGE_MASK)});
        const PAddr ps = physical(s), pd = physical(d);
        if (!runs.empty() && runs.back().src + runs.back().size == ps &&
            runs.back().dst + runs.back().size == pd) {
            runs.back().size += chunk;
        } else {
            runs.push_back({ps, pd, chunk});
        }
        done += chunk;
    }
    return runs;
}

void GPU::KeepUncached(PAddr addr, u32 size) {
    // Rare: a page moves once and stays until the GPU leaves it unused (ReturnUncached). The
    // move runs here, the guest stopped, with the render thread held after the work queued
    // before it; the GPU's mapping follows in its own order (GuestGpuMemory), without an idle.
    if (impl->renderer->WouldMakeUncached(addr, size)) {
        const auto hold = HoldRender();
        impl->renderer->MakeUncached(addr, size, overwritten);
    }
}

void GPU::RunMemoryTransfer(const Pica::DisplayTransferConfig& config,
                            bool allow_display_transfer) {
    MICROPROFILE_SCOPE(GPU_DisplayTransfer);

    // Notify debugger about the display transfer.

    // Perform memory transfer
    if (config.is_texture_copy) {
        if (!impl->rasterizer->AccelerateTextureCopy(config)) {
            UNREACHABLE_MSG("GPU: the texture copy {} was not done on the GPU", config.DebugName());
        }
    } else if (allow_display_transfer && !impl->rasterizer->AccelerateDisplayTransfer(config)) {
        UNREACHABLE_MSG("GPU: the display transfer {} was not done on the GPU", config.DebugName());
    }
}

void GPU::VBlankCallback(std::uintptr_t user_data, s64 cycles_late) {
    GPU_TRACE("vblank");
    // vprof: a recorded frame starts with the PICA's state.
    const bool frame_starts = SgxdRenderer::Vprof::VBlank();
    SyncCaptureHold();
    if (frame_starts) {
        RunGpuOp(OpKind::RegWrite, [this] {
            // The capture records the lists' state, the render thread's (held here), with the
            // geometry unit's setup as its own (FlushGsMirror).
            impl->render->GiveWholeState();
            impl->pica.FlushGsMirror();
            SgxdRenderer::Vprof::State(impl->pica);
#ifdef VITA_DIAGNOSTICS
            // What the frame's passes can start from: the rasterizer's clears, surfaces and
            // depth stores (RSTA). The render thread is held (SyncCaptureHold).
            impl->render->Rasterizer().VprofFrameStart();
#endif
        });
    }
    // Signal to GSP that GPU interrupt has occurred
    Signal(Service::GSP::InterruptId::PDC0, 0);
    Signal(Service::GSP::InterruptId::PDC1, 0);

    // Moved pages the GPU has not used for a while go back to cached FCRAM first, so that the
    // moves below can reuse their pool pages. Rare, as a move is: the render thread held.
    if (impl->renderer->UncachedFrameEnded()) {
        const auto hold = HoldRender();
        impl->renderer->ReturnUncached();
    }
    // Vertex and index buffers the GPU read from cached FCRAM since the last vblank move to
    // uncached memory once, so later draws from them need no clean.
#ifdef VITA_DIAGNOSTICS
    // sgxd_flags vbcached: they stay cached, each draw cleaning them, for an A/B of the guest's own
    // work on those pages.
    static const bool vb_cached = SgxdRenderer::SgxdFlag("vbcached");
#else
    constexpr bool vb_cached = false;
#endif
    for (const auto& [addr, size] : impl->rasterizer->TakeCachedReads()) {
#ifdef VITA_DIAGNOSTICS
        if (!vb_cached && impl->renderer->WouldMakeUncached(addr, size)) {
            SgxdRenderer::GuestGpuMemory::WatchNote("vertex or index buffers read since the vblank",
                                                    addr, size);
        }
#endif
        if (!vb_cached) {
            KeepUncached(addr, size);
        }
    }
    // The LCD's picture comes from the GPU's present: kept uncached, it needs no clean
    // before each one. The LCD scans 400 columns on the top screen, 320 on the bottom.
    const auto& regs = impl->pica.regs;
    for (u32 i = 0; i < 2; i++) {
        const Pica::FramebufferConfig& f = regs.framebuffer_config[i];
        KeepUncached(f.second_fb_active ? f.address_left2 : f.address_left1,
                     f.stride * (i == 0 ? 400u : 320u));
    }

    // Present renderered frame, as the LCD's registers are now.
    const std::array<Pica::FramebufferConfig, 2> fb{regs.framebuffer_config[0],
                                                    regs.framebuffer_config[1]};
    const std::array<Pica::ColorFill, 2> fill{impl->pica.regs_lcd.color_fill_top,
                                              impl->pica.regs_lcd.color_fill_bottom};
    // On the render thread, after the work before it; it drops one too soon after the last.
    // The rasterizer's frame ends here whether or not the present is shown. Nothing new on the
    // guest's screens and none in the window's overlay: no present (Impl::screens_changed).
    impl->render->EndFrame();
    const bool overlay = impl->renderer->GetRenderWindow().TakeOverlayChanged();
    if (impl->screens_changed || overlay ||
        std::memcmp(&fb, &impl->presented_fb, sizeof(fb)) != 0 ||
        std::memcmp(&fill, &impl->presented_fill, sizeof(fill)) != 0) {
        impl->render->Present(fb, fill);
        impl->screens_changed = false;
        impl->presented_fb = fb;
        impl->presented_fill = fill;
    }
    impl->render->StartFramePageHolds();
    impl->render->ApplyPageHolds();
    // The guest hears of the GPU's work when it is submitted. The vblank does not wait for
    // the GPU; the render thread drops the frames it is two behind on (the late skip,
    // RenderThread's Op::FrameEnd), which keeps it near the guest.
    impl->vblank_ticket = impl->rasterizer->InterruptFence();
    impl->render->FrameTicket(impl->vblank_ticket);
#ifdef VITA_DIAGNOSTICS
    // sgxd_flags `serial`: the vblank waits for the GPU to finish the frame, so that no frame's
    // work overlaps the guest's next one (and the late skip never finds the GPU behind).
    static const bool serial = SgxdRenderer::SgxdFlag("serial");
    if (serial) {
        impl->render->InterruptWait(impl->vblank_ticket);
    }
#endif
    // The frame limiter and the window's events stay on this thread.
    impl->renderer->EndFrame();

    // Reschedule recurrent event
    impl->timing.ScheduleEvent(FRAME_TICKS - cycles_late, impl->vblank_event);
}

template <class Archive>
void GPU::serialize(Archive& ar, const u32 file_version) {
    // The lists' state is the render thread's PicaCore's (RenderThread::SubmitCmdList): saved
    // from it, loaded into it.
    if constexpr (Archive::is_saving::value) {
        impl->render->GiveWholeState();
    }
    ar & impl->pica;
    if constexpr (Archive::is_loading::value) {
        impl->render->TakeWholeState();
    }
    // A loaded state's screens are in the memory it loaded.
    impl->screens_changed = true;
}

SERIALIZE_IMPL(GPU)

} // namespace VideoCore

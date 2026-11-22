// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "common/archives.h"
#include "common/hacks/hack_manager.h"
#include "common/microprofile.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/loader/loader.h"
#include "video_core/gpu.h"
#include "video_core/gpu_debugger.h"
#include "video_core/gpu_impl.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_base.h"
#include "video_core/right_eye_disabler.h"
#include "video_core/video_core.h"
#ifdef ENABLE_GXM
#include "video_core/renderer_sgxd/sgxd_flags.h"
#define GPU_TRACE(...) SgxdRenderer::SgxdTrace(__VA_ARGS__)
#else
#define GPU_TRACE(...) ((void)0)
#endif

namespace VideoCore {

constexpr VAddr VADDR_LCD = 0x1ED02000;
constexpr VAddr VADDR_GPU = 0x1EF00000;

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

    // The PICA raises P3D from inside a command list, as upstream: with its modelled delay,
    // when the list's IRQ request comes, whatever sgxd still has running from it.
    Service::GSP::InterruptHandler pica_interrupt = [this](Service::GSP::InterruptId id,
                                                           u64 delay) { Signal(id, delay); };
    impl->pica.SetInterruptHandler(pica_interrupt);

    // Bind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);
}

GPU::~GPU() = default;

void GPU::RunGpuOp([[maybe_unused]] OpKind kind, const std::function<void()>& op) {
#ifndef VITA_DIAGNOSTICS
    op();
#else
    // The time by kind is `perflog`'s, in the diagnostics build only.
    impl->render_thread->Push([this, kind, op = std::move(op)] {
        const auto start = std::chrono::steady_clock::now();
        const u64 waited = impl->rasterizer->GpuWaitUs();
        GPU_TRACE("op", static_cast<unsigned>(kind));
        op();
        GPU_TRACE("done", static_cast<unsigned>(impl->rasterizer->GpuWaitUs() - waited));
        const auto k = static_cast<std::size_t>(kind);
        impl->kind_wait_us[k].fetch_add(impl->rasterizer->GpuWaitUs() - waited,
                                        std::memory_order_relaxed);
        impl->kind_us[k].fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - start)
                                       .count()),
                                   std::memory_order_relaxed);
        impl->kind_ops[k].fetch_add(1, std::memory_order_relaxed);
        impl->ops_in_flight.fetch_sub(1, std::memory_order_release);
    });
#endif
}

void GPU::PostToEmulation(std::function<void()> fn, RasterizerInterface::GpuFence fence) {
    {
        std::scoped_lock lock{impl->posted_mutex};
        impl->posted.push_back({fence, std::move(fn)});
        if (fence.Any()) {
            impl->fenced.store(true, std::memory_order_release);
        }
    }
    // No delay of its own: the work is done now, and the interrupt's modelled delay travels
    // inside fn to GSP.
    impl->timing.ScheduleEvent(0, impl->posted_event, 0, 0, true);
}

void GPU::PostedEventCallback(uintptr_t, s64) {
    DrainPosted();
}

void GPU::PollPosted() {
    if (impl->fenced.load(std::memory_order_acquire)) {
        DrainPosted();
    }
}

bool GPU::WaitPostedIdle() {
    const auto start = std::chrono::steady_clock::now();
    const u64 waited = impl->rasterizer->GpuWaitUs();
    GPU_TRACE("op", static_cast<unsigned>(kind));
    SgxdRenderer::Vprof::OpBegin(seq);
    op();
    SgxdRenderer::Vprof::OpEnd(seq);
    const u64 wait = impl->rasterizer->GpuWaitUs() - waited;
    GPU_TRACE("done", static_cast<unsigned>(wait));
    impl->kind_wait_us[k].fetch_add(wait, std::memory_order_relaxed);
    impl->kind_us[k].fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now() - start)
                                   .count()),
                               std::memory_order_relaxed);
#endif
    // GPU work the op held back starts now (the last pass's 3D, when a transfer waits on it).
    impl->rasterizer->GpuIdle();
}

void GPU::Signal(Service::GSP::InterruptId id, [[maybe_unused]] u64 delay) {
    if (impl->interrupts_muted) {
        return;
    }
    GPU_TRACE("irq", static_cast<unsigned>(id));
    impl->signal_interrupt(id, delay);
}

void GPU::RunOnRenderThread(const std::function<void()>& fn) {
    impl->render_thread->RunSync(fn);
}

PAddr GPU::VirtualToPhysicalAddress(VAddr addr) {
    if (addr == 0) {
        return 0;
    }

    if (addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END) {
        return addr - Memory::VRAM_VADDR + Memory::VRAM_PADDR;
    }
    if (addr >= Memory::LINEAR_HEAP_VADDR && addr <= Memory::LINEAR_HEAP_VADDR_END) {
        return addr - Memory::LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    if (addr >= Memory::NEW_LINEAR_HEAP_VADDR && addr <= Memory::NEW_LINEAR_HEAP_VADDR_END) {
        return addr - Memory::NEW_LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    PAddr plg_fb_addr;
    if (addr >= Memory::PLUGIN_3GX_FB_VADDR && addr <= Memory::PLUGIN_3GX_FB_VADDR_END &&
        (plg_fb_addr = impl->system.Memory().Plugin3GXFramebufferAddress())) {
        return addr - Memory::PLUGIN_3GX_FB_VADDR + plg_fb_addr;
    }

    LOG_ERROR(HW_Memory, "Unknown virtual address @ 0x{:08X}", addr);
    return addr;
}

void GPU::SetInterruptHandler(Service::GSP::InterruptHandler handler) {
    impl->signal_interrupt = handler;
}

void GPU::FlushRegion(PAddr addr, u32 size) {
    impl->rasterizer->FlushRegion(addr, size);
}

void GPU::InvalidateRegion(PAddr addr, u32 size) {
    impl->rasterizer->InvalidateRegion(addr, size);
}

void GPU::ClearAll(bool flush) {
    impl->rasterizer->ClearAll(flush);
}

void GPU::Execute(const Service::GSP::Command& command) {
    using Service::GSP::CommandId;
    auto& regs = impl->pica.regs;

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
        RunGpuOp(OpKind::Dma, [this, runs = std::move(runs)] {
            for (const DmaRun& run : runs) {
                if (!impl->rasterizer->AccelerateDma(run.src, run.dst, run.size)) {
                    UNREACHABLE_MSG("GPU: DMA {:#x} to {:#x}, {} bytes: not done on the GPU",
                                    run.src, run.dst, run.size);
                }
            }
            PostToEmulation([this] { impl->signal_interrupt(Service::GSP::InterruptId::DMA, 0); },
                            impl->rasterizer->GpuFencePtla());
        });
        break;
    }
    case CommandId::SubmitCmdList: {
        auto& params = command.submit_gpu_cmdlist;
        const PAddr address = VirtualToPhysicalAddress(params.address);
        const u32 size = params.size;
        RunGpuOp(OpKind::CmdList, [this, &regs, address, size] {
            auto& cmdbuffer = regs.internal.pipeline.command_buffer;

            // Write to the command buffer GPU registers
            cmdbuffer.addr[0].Assign(address >> 3);
            cmdbuffer.size[0].Assign(size >> 3);
            cmdbuffer.trigger[0] = 1;
            // As a command list's writes mark them: the render thread's copy takes them.
            auto& dirty = impl->pica.dirty_regs;
            dirty.Set(PICA_REG_INDEX(pipeline.command_buffer.addr[0]));
            dirty.Set(PICA_REG_INDEX(pipeline.command_buffer.size[0]));
            dirty.Set(PICA_REG_INDEX(pipeline.command_buffer.trigger[0]));

            // Trigger processing of the command list
            SubmitCmdList(0);
        });
        break;
    }
    case CommandId::MemoryFill: {
        auto& params = command.memory_fill;
        auto& memfill = regs.memory_fill_config;

        // Write to the memory fill GPU registers.
        // If both buffers are set GSP dispatches PSC0 only.
        const bool has_both_bufs = params.start1 != 0 && params.start2 != 0;
        if (params.start1 != 0) {
            memfill[0].address_start = VirtualToPhysicalAddress(params.start1) >> 3;
            memfill[0].address_end = VirtualToPhysicalAddress(params.end1) >> 3;
            memfill[0].value_32bit = params.value1;
            memfill[0].control = params.control1;
            MemoryFill(0, has_both_bufs ? std::numeric_limits<u32>::max() : 0);
        }
        if (params.start2 != 0) {
            memfill[1].address_start = VirtualToPhysicalAddress(params.start2) >> 3;
            memfill[1].address_end = VirtualToPhysicalAddress(params.end2) >> 3;
            memfill[1].value_32bit = params.value2;
            memfill[1].control = params.control2;
            MemoryFill(1, has_both_bufs ? 0 : 1);
        }
        break;
    }
    case CommandId::DisplayTransfer: {
        auto& params = command.display_transfer;
        auto& display_transfer = regs.display_transfer_config;

        // Write to the transfer engine GPU registers.
        display_transfer.input_address = VirtualToPhysicalAddress(params.in_buffer_address) >> 3;
        display_transfer.output_address = VirtualToPhysicalAddress(params.out_buffer_address) >> 3;
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
        texture_copy.input_address = VirtualToPhysicalAddress(params.in_buffer_address) >> 3;
        texture_copy.output_address = VirtualToPhysicalAddress(params.out_buffer_address) >> 3;
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
    }

    // Notify debugger that a GSP command was processed.
}

void GPU::SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info) {
    const PAddr phys_address_left = VirtualToPhysicalAddress(info.address_left);
    const PAddr phys_address_right = VirtualToPhysicalAddress(info.address_right);

    // Update framebuffer properties.
    auto& framebuffer = impl->pica.regs.framebuffer_config[screen_id];
    if (info.active_fb == 0) {
        framebuffer.address_left1 = phys_address_left;
        framebuffer.address_right1 = phys_address_right;
    } else {
        framebuffer.address_left2 = phys_address_left;
        framebuffer.address_right2 = phys_address_right;
    }

    framebuffer.stride = info.stride;
    framebuffer.format = info.format;
    framebuffer.active_fb = info.shown_fb;

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
        break;
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);

        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        if (index >= GPU_REG_INDEX(internal)) {
            // An op like the GSP's commands, numbered with them in a vprof recording.
            RunGpuOp(OpKind::RegWrite, [this, index, data] {
                impl->pica.regs.reg_array[index] = data;
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
            MemoryFill(0, 0);
            break;
        case GPU_REG_INDEX(memory_fill_config[1].trigger):
            MemoryFill(1, 1);
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

VideoCore::RendererBase& GPU::Renderer() {
    return *impl->renderer;
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
    impl->rasterizer->SetAccurateMul(use_accurate_mul);
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
    impl->pica.ProcessCmdList(addr, size,
                              !right_eye_disabler->ShouldAllowCmdQueueTrigger(addr, size));
    config.trigger[index] = 0;
    impl->pica.dirty_regs.Set(PICA_REG_INDEX(pipeline.command_buffer.trigger[0]) + index);
}

void GPU::MemoryFill(u32 index, u32 intr_index) {
    // Check if a memory fill was triggered.
    auto& config = impl->pica.regs.memory_fill_config[index];
    if (!config.trigger) {
        return;
    }

    // Perform memory fill: the PTLA's, queued behind the renders before it.
    RunGpuOp(OpKind::Fill, [this, &config] {
        if (!impl->rasterizer->AccelerateFill(config)) {
            UNREACHABLE_MSG("GPU: the fill of {:#x}-{:#x} was not done on the GPU",
                            config.GetStartAddress(), config.GetEndAddress());
        }
    });

    // Treat fill as texture transfer from VRAM
    u64 delay = DelayGenerator::CalculateDelayNanoseconds(
        DelayGenerator::GetCopyMode(true, config.IsVRAM()), true,
        config.GetEndAddress() - config.GetStartAddress());

    // It seems that it won't signal interrupt if "address_start" is zero.
    // TODO: hwtest this
    if (config.GetStartAddress() != 0) {
        if (intr_index == 0) {
            Signal(Service::GSP::InterruptId::PSC0, delay);
        } else if (intr_index == 1) {
            Signal(Service::GSP::InterruptId::PSC1, delay);
        }
    }

    // Reset "trigger" flag and set the "finish" flag
    // This was confirmed to happen on hardware even if "address_start" is zero.
    config.trigger.Assign(0);
    config.finished.Assign(1);
}

void GPU::MemoryTransfer() {
    // Check if a transfer was triggered.
    auto& config = impl->pica.regs.display_transfer_config;
    if (!config.trigger.Value()) {
        return;
    }
    const bool allow_display_transfer =
        config.is_texture_copy ||
        right_eye_disabler->ShouldAllowDisplayTransfer(config.GetPhysicalInputAddress(),
                                                       config.input_height, config.output_height);
    // The output's pages move without their contents: only the bytes the transfer writes
    // (OutputBytes, the scaling's). The registers' unscaled size moved a game Y's heap past
    // a halved screen capture's output, uncopied, and the guest read the pool's old pixels as
    // a pointer.
    if (!config.is_texture_copy && allow_display_transfer) {
        KeepUncached(config.GetPhysicalOutputAddress(), config.OutputBytes(), true);
    }
    RunGpuOp(OpKind::Transfer,
             [this, &config, allow_display_transfer] {
                 RunMemoryTransfer(config, allow_display_transfer);
             });

    u64 delay{};
    if (config.is_texture_copy) {
        delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
            config.texture_copy.size);
    } else {
        delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
            config.input_width * config.input_height * BytesPerPixel(config.input_format));
    }

    // Complete transfer.
    config.trigger.Assign(0);
    Signal(Service::GSP::InterruptId::PPF, delay);
}

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

void GPU::KeepUncached(PAddr addr, u32 size, bool overwritten) {
    // Rare: a page moves once and stays until the GPU leaves it unused (ReturnUncached). The
    // move waits for the GPU's work first, so the GPU is idle under it.
    if (impl->renderer->WouldMakeUncached(addr, size)) {
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
    // Signal to GSP that GPU interrupt has occurred
    Signal(Service::GSP::InterruptId::PDC0, 0);
    Signal(Service::GSP::InterruptId::PDC1, 0);

    // Moved pages the GPU has not used for a while go back to cached FCRAM first, so that the
    // moves below can reuse their pool pages. Rare, as a move is: the GPU idle.
    if (impl->renderer->UncachedFrameEnded()) {
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
        KeepUncached(addr, size, false);
    }
    // The LCD's picture comes from the GPU's present: kept uncached, it needs no clean
    // before each one. The LCD scans 400 columns on the top screen, 320 on the bottom.
    const auto& regs = impl->pica.regs;
    for (u32 i = 0; i < 2; i++) {
        const Pica::FramebufferConfig& f = regs.framebuffer_config[i];
        KeepUncached(f.second_fb_active ? f.address_left2 : f.address_left1,
                     f.stride * (i == 0 ? 400u : 320u), false);
    }

    // Present renderered frame.
    // A present has no guest-visible effect: well past the display's rate it is dropped. 12 ms,
    // not a frame's 16.7: at 100% the guest's VBlanks jitter around the frame, and a frame's cap
    // dropped a quarter of them.
    const auto now = std::chrono::steady_clock::now();
    if (now - impl->last_present >= std::chrono::microseconds(12000)) {
        impl->last_present = now;
        const std::array<Pica::FramebufferConfig, 2> fb{regs.framebuffer_config[0],
                                                        regs.framebuffer_config[1]};
        const std::array<Pica::ColorFill, 2> fill{impl->pica.regs_lcd.color_fill_top,
                                                  impl->pica.regs_lcd.color_fill_bottom};
        GPU_TRACE("present");
        impl->renderer->PresentFrame(fb, fill);
        GPU_TRACE("presented");
        impl->rasterizer->GpuIdle();
#ifdef VITA_DIAGNOSTICS
        impl->present_us.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - now)
                                       .count()),
                                   std::memory_order_relaxed);
        impl->presents.fetch_add(1, std::memory_order_relaxed);
#endif
    } else {
        impl->presents_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    impl->renderer->EndFrame();

    // Reschedule recurrent event
    impl->timing.ScheduleEvent(FRAME_TICKS - cycles_late, impl->vblank_event);
}

void GPU::RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window) {
    // Reset the renderer (this will destroy OpenGL resources)
    impl->renderer.reset();

    // Create a new renderer
    impl->renderer =
        VideoCore::CreateRenderer(emu_window, secondary_window, impl->pica, impl->system);
    impl->rasterizer = impl->renderer->Rasterizer();

    // Rebind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);

    // Re-apply per-game configuration and reload disk shader cache
    u64 program_id{};
    impl->system.GetAppLoader().ReadProgramId(program_id);
    ApplyPerProgramSettings(program_id);
    if (Settings::values.use_disk_shader_cache) {
        impl->renderer->Rasterizer()->LoadDefaultDiskResources(false, nullptr);
    }

    // Mark ALL GPU registers as dirty so current state gets uploaded to new renderer
    impl->pica.dirty_regs.SetAllDirty();

    // Also mark shader setups as dirty so uniforms get re-uploaded and
    // stale pointers to the old rasterizer's JIT cache are cleared.
    impl->pica.vs_setup.uniforms_dirty = true;
    impl->pica.vs_setup.cached_shader = nullptr;
    impl->pica.gs_setup.uniforms_dirty = true;
    impl->pica.gs_setup.cached_shader = nullptr;

    // Mark all cached LUT/table state in pica as dirty
    impl->pica.lighting.lut_dirty = impl->pica.lighting.LutAllDirty;
    impl->pica.fog.lut_dirty = true;
    impl->pica.proctex.table_dirty = impl->pica.proctex.TableAllDirty;
}

void GPU::ReleaseRenderer() {
    // Just reset the renderer to release OpenGL resources
    // Don't null out rasterizer pointer as it will become dangling
    impl->renderer.reset();
    LOG_INFO(HW_GPU, "Renderer released for context destroy");
}

template <class Archive>
void GPU::serialize(Archive& ar, const u32 file_version) {
    ar & impl->pica;
}

SERIALIZE_IMPL(GPU)

} // namespace VideoCore

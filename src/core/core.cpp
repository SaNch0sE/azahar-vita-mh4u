// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>
#include <boost/serialization/array.hpp>
#include "audio_core/dsp_interface.h"
#include "audio_core/hle/hle.h"
#ifdef ENABLE_DSP_LLE
#include "audio_core/lle/lle.h"
#endif
#include "common/arch.h"
#include "common/logging/log.h"
#include "common/named_thread.h"
#include "common/thread.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/arm/exclusive_monitor.h"
#include "core/hle/service/cam/cam.h"
#include "core/hle/service/hid/hid.h"
#include "core/hle/service/ir/ir_user.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
#include "core/arm/dynarmic/arm_dynarmic.h"
#endif
#ifdef CITRA_HAS_VITA_NATIVE
#include "core/arm/vita/arm_vita_native.h"
#else
#include "core/arm/dyncom/arm_dyncom.h"
#endif
#ifdef CITRA_HAS_NATIVE_ARM
#include "core/arm/native/arm_native.h"
#endif
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "core/dumping/backend.h"
#include "core/frontend/emu_window.h"
#include "core/file_sys/ncch_container.h"
#include "core/frontend/image_interface.h"
#ifdef ENABLE_GDBSTUB
#include "core/gdbstub/gdbstub.h"
#endif
#include "core/global.h"
#include "core/hle/kernel/address_arbiter.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/service/apt/applet_manager.h"
#include "core/hle/service/apt/apt.h"
#include "core/hle/service/cam/cam.h"
#include "core/hle/service/fs/archive.h"
#include "core/hle/service/gsp/gsp.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/ir/ir_rst.h"
#include "core/hle/service/mic/mic_u.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"
#include "core/hw/aes/key.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/movie.h"
#include "core/savestate.h"
#ifdef ENABLE_SCRIPTING
#include "core/rpc/server.h"
#endif
#include "network/network.h"
#include "video_core/gpu.h"

namespace Core {

u32 host1_tp{};
std::atomic<bool> multi_host{};
std::atomic<bool> host1_parked{};

/*static*/ System System::s_instance;

template <>
Core::System& Global() {
    return System::GetInstance();
}

template <>
Kernel::KernelSystem& Global() {
    return System::GetInstance().Kernel();
}

template <>
Core::Timing& Global() {
    return System::GetInstance().CoreTiming();
}

System::System() : movie{*this}, cheat_engine{*this} {}

System::~System() = default;

System::ResultStatus System::RunLoop(bool tight_loop) {
    status = ResultStatus::Success;

    if (!IsPoweredOn()) {
        return ResultStatus::ErrorNotInitialized;
    }

#ifdef ENABLE_GDBSTUB
    if (GDBStub::IsServerEnabled()) {
        // The break flag is only set if GDB is connected,
        // we can do clearing here safely. If it is ever
        // used outside, move the clearing outside the if.
        for (auto& cpu_core : cpu_cores) {
            cpu_core->ClearBreakFlag();
        }
        GDBStub::HandlePacket(*this);
    }
#endif

    Signal signal{Signal::None};
    u32 param{};
    {
        std::scoped_lock lock{signal_mutex};
        if (current_signal != Signal::None) {
            signal = current_signal;
            param = signal_param;
            current_signal = Signal::None;
        }
    }
    switch (signal) {
    case Signal::Reset: {
        if (app_loader && app_loader->DoingInitialSetup()) {
            // Treat reset as shutdown if we are doing the initial setup
            return ResultStatus::ShutdownRequested;
        }
        Reset();
        return ResultStatus::Success;
    }
    case Signal::Shutdown:
        return ResultStatus::ShutdownRequested;
    case Signal::Load: {
        if (save_state_request_status != SaveStateStatus::NONE) {
            LOG_ERROR(Core, "A pending save state operation has not finished yet");
            status_details = "A pending save state operation has not finished yet";
            return ResultStatus::ErrorSavestate;
        }
        u64 title_id{};
        if (app_loader) {
            app_loader->ReadProgramId(title_id);
        }
        auto info = GetSaveStateInfo(title_id, movie.GetCurrentMovieID(), param);
        if (info.slot == std::numeric_limits<u32>::max()) {
            // Should not happen
            status_details = "Failed to load savestate";
            return ResultStatus::ErrorSavestate;
        }
        if (!load_state_any_build &&
            info.status == Core::SaveStateInfo::ValidationStatus::BuildMismatch) {
            status_details = info.build_name;
            return ResultStatus::ErrorSavestateBuildMismatch;
        }
        save_state_slot = param;
        save_state_request_time = std::chrono::steady_clock::now();
        save_state_request_status = SaveStateStatus::LOADING;
        break;
    }
    case Signal::Save: {
        if (save_state_request_status != SaveStateStatus::NONE) {
            LOG_ERROR(Core, "A pending save state operation has not finished yet");
            status_details = "A pending save state operation has not finished yet";
            return ResultStatus::ErrorSavestate;
        }
        save_state_slot = param;
        save_state_request_time = std::chrono::steady_clock::now();
        save_state_request_status = SaveStateStatus::SAVING;
        break;
    }
    default:
        break;
    }

    if (save_state_request_status == SaveStateStatus::LOADING && kernel.get() &&
        !kernel->AreAsyncOperationsPending()) {
        const u32 slot = save_state_slot;
        save_state_request_status = SaveStateStatus::NONE;
        LOG_INFO(Core, "Begin load of slot {}", slot);
        StopSecondHost();
        try {
            System::LoadState(slot);
            LOG_INFO(Core, "Load completed");
        } catch (const std::exception& e) {
            LOG_ERROR(Core, "Error loading: {}", e.what());
            status_details = e.what();
            return ResultStatus::ErrorSavestate;
        }
        frame_limiter.WaitOnce();
        return ResultStatus::Success;
    } else if (save_state_request_status == SaveStateStatus::SAVING && kernel.get() &&
               !kernel->AreAsyncOperationsPending()) {
        save_state_request_status = SaveStateStatus::NONE;
        const u32 slot = save_state_slot;
        LOG_INFO(Core, "Begin save to slot {}", slot);
        StopSecondHost();
        try {
            System::SaveState(slot);
            LOG_INFO(Core, "Save completed");
        } catch (const std::exception& e) {
            LOG_ERROR(Core, "Error saving: {}", e.what());
            status_details = e.what();
            return ResultStatus::ErrorSavestate;
        }
        frame_limiter.WaitOnce();
        return ResultStatus::Success;
    } else if (save_state_request_status != SaveStateStatus::NONE &&
               (std::chrono::steady_clock::now() - save_state_request_time) >
                   std::chrono::seconds(5)) {
        save_state_request_status = SaveStateStatus::NONE;
        LOG_ERROR(Core, "Cannot perform save state operation due to pending async operations");
        status_details = "Cannot perform save state operation due to pending async operations";
        return ResultStatus::ErrorSavestate;
    }

    // The second emulation thread takes the other cores once the guest's table is in (its view
    // of the guest is made from it); it stops before a savestate (above) and at the end. From
    // then on this loop runs core 0, takes the second thread's messages and its svcs.
    if (!second_host && cpu_cores.size() > 1 && SecondHostCanStart()) {
        StartSecondHost();
    }
    const bool multi = multi_host.load(std::memory_order_relaxed);
#ifdef VITA_DIAGNOSTICS
    if (thread_dump_requested.exchange(false, std::memory_order_relaxed)) {
        DumpGuestThreads();
    }
#endif
    if (multi) {
        if (second_failed.load(std::memory_order_acquire)) {
            status = second_status;
            status_details = second_details;
            return status;
        }
        kernel->ProcessHostMessages();
        ServeForward();
    }

    // The GSP interrupts go to the guest as upstream raises them, when the work is submitted
    // (VideoCore::GPU::Signal); the guest pages that work still reads or writes are held until
    // it is done. The holds whose work is done go between slices.
    gpu->DeliverInterrupts();
    const auto idle = [](ARM_Interface& core) { core.GetTimer().Idle(); };
    const auto cores = HostCores(0);

    // All cores should have executed the same amount of ticks. If this is not the case an event was
    // scheduled with a cycles_into_future smaller then the current downcount.
    // So we have to get those cores to the same global time first
    u64 global_ticks = 0;
    for (const auto& cpu_core : cores) {
        global_ticks = std::max<u64>(global_ticks, cpu_core->GetTimer().GetTicks());
    }
    s64 max_delay = 0;
    ARM_Interface* current_core_to_execute = nullptr;
    for (auto& cpu_core : cores) {
        if (cpu_core->GetTimer().GetTicks() < global_ticks) {
            s64 delay = global_ticks - cpu_core->GetTimer().GetTicks();
            SetRunning(cpu_core.get());
            cpu_core->GetTimer().Advance();
            cpu_core->PrepareReschedule();
            kernel->GetThreadManager(cpu_core->GetID()).Reschedule();
            cpu_core->GetTimer().SetNextSlice(delay);
            if (max_delay < delay) {
                max_delay = delay;
                current_core_to_execute = cpu_core.get();
            }
        }
    }

    // jit sometimes overshoot by a few ticks which might lead to a minimal desync in the cores.
    // This small difference shouldn't make it necessary to sync the cores and would only cost
    // performance. Thus we don't sync delays below min_delay
    static constexpr s64 min_delay = 100;
    if (max_delay > min_delay) {
        LOG_TRACE(Core_ARM11, "Core {} running (delayed) for {} ticks",
                  current_core_to_execute->GetID(),
                  current_core_to_execute->GetTimer().GetDowncount());
        if (running_core[ThisHost()] != current_core_to_execute) {
            SetRunning(current_core_to_execute);
        }
        if (kernel->GetCurrentThreadManager().GetCurrentThread() == nullptr) {
            LOG_TRACE(Core_ARM11, "Core {} idling", current_core_to_execute->GetID());
            SgxdRenderer::SgxdTrace("idle", static_cast<unsigned>(
                                              current_core_to_execute->GetTimer().GetDowncount()));
            idle(*current_core_to_execute);
            PrepareReschedule();
        } else {
            if (tight_loop) {
                current_core_to_execute->Run();
            } else {
                current_core_to_execute->Step();
            }
        }
        Reschedule();
    } else {
        // Now all cores are at the same global time. So we will run them one after the other
        // with a max slice that is the minimum of all max slices of all cores
        // TODO: Make special check for idle since we can easily revert the time of idle cores
        s64 max_slice = Timing::MAX_SLICE_LENGTH;
        for (const auto& cpu_core : cores) {
            SetRunning(cpu_core.get());
            cpu_core->GetTimer().Advance();
            cpu_core->PrepareReschedule();
            kernel->GetThreadManager(cpu_core->GetID()).Reschedule();
            max_slice = std::min(max_slice, cpu_core->GetTimer().GetMaxSliceLength());
        }
        // Each core after the first runs up to where the one before it got: the same span when
        // they start together, less for a core whose idle time has run already (the Vita native
        // core's upcalls run an idle core's timer along with the running one's).
        std::optional<u64> reached;
#ifdef VITA_DIAGNOSTICS
        bool ran = false;
#endif
        for (auto& cpu_core : cores) {
            if (reached) {
                const u64 at = cpu_core->GetTimer().GetTicks();
                max_slice = *reached > at ? static_cast<s64>(*reached - at) : 0;
            }
            cpu_core->GetTimer().SetNextSlice(max_slice);
            auto start_ticks = cpu_core->GetTimer().GetTicks();
            LOG_TRACE(Core_ARM11, "Core {} running for {} ticks", cpu_core->GetID(),
                      cpu_core->GetTimer().GetDowncount());
            SetRunning(cpu_core.get());
            // If we don't have a currently active thread then don't execute instructions,
            // instead advance to the next event and try to yield to the next thread
            if (kernel->GetCurrentThreadManager().GetCurrentThread() == nullptr) {
                LOG_TRACE(Core_ARM11, "Core {} idling", cpu_core->GetID());
                SgxdRenderer::SgxdTrace("idle",
                                      static_cast<unsigned>(cpu_core->GetTimer().GetDowncount()));
                if (multi) {
                    HostIdle(0);
                } else {
                    idle(*cpu_core);
                }
                PrepareReschedule();
            } else {
#ifdef VITA_DIAGNOSTICS
                ran = true;
#endif
                // In the rare case the break flag is set (due to exception thrown)
                // there is probably no need to adjust the timer accordingly.
                if (tight_loop) {
                    cpu_core->Run();
                } else {
                    cpu_core->Step();
                }
            }
            max_slice = cpu_core->GetTimer().GetTicks() - start_ticks;
            reached = cpu_core->GetTimer().GetTicks();
            Reschedule();
        }
#ifdef VITA_DIAGNOSTICS
        // Every core idle in this pass: the guest's clock jumped (GPU::GuestIdle).
        if (!ran) {
            gpu->GuestIdle();
        }
#endif
    }
    if (multi) {
        PublishClock(0);
    }

    return status;
}

bool System::SendSignal(System::Signal signal, u32 param) {
    std::scoped_lock lock{signal_mutex};
    if (current_signal != signal && current_signal != Signal::None) {
        LOG_ERROR(Core, "Unable to {} as {} is ongoing", signal, current_signal);
        return false;
    }
    current_signal = signal;
    signal_param = param;
    return true;
}

System::ResultStatus System::SingleStep() {
    return RunLoop(false);
}

System::ResultStatus System::Load(Frontend::EmuWindow& emu_window, const std::string& filepath,
                                  Frontend::EmuWindow* secondary_window) {
    Settings::ResetTemporaryFrameLimit();
    FileUtil::SetCurrentRomPath(filepath);
    if (early_app_loader) {
        app_loader = std::move(early_app_loader);
    } else {
        app_loader = Loader::GetLoader(filepath);
    }
    if (!app_loader) {
        LOG_CRITICAL(Core, "Failed to obtain loader for {}", filepath);
        return ResultStatus::ErrorGetLoader;
    }

    u64_le program_id = 0;
    app_loader->ReadProgramId(program_id);
    if (restore_plugin_context.has_value() && restore_plugin_context->is_enabled &&
        restore_plugin_context->use_user_load_parameters) {
        if (restore_plugin_context->user_load_parameters.low_title_Id ==
                static_cast<u32_le>(program_id) &&
            restore_plugin_context->user_load_parameters.plugin_memory_strategy ==
                Service::PLGLDR::PLG_LDR::PluginMemoryStrategy::PLG_STRATEGY_MODE3) {
            app_loader->SetKernelMemoryModeOverride(Kernel::MemoryMode::Dev2);
        }
    }

    Kernel::MemoryMode app_mem_mode;
    Kernel::MemoryMode system_mem_mode;
    bool used_default_mem_mode = false;
    Kernel::New3dsHwCapabilities app_n3ds_hw_capabilities;

    if (m_mem_mode) {
        // Use memory mode set by the FIRM launch parameters
        system_mem_mode = static_cast<Kernel::MemoryMode>(m_mem_mode.value());
        m_mem_mode = {};
    } else {
        // Only the Old 3DS is emulated.
        system_mem_mode = Kernel::MemoryMode::Prod;
        used_default_mem_mode = true;
    }

    {
        auto memory_mode = app_loader->LoadKernelMemoryMode();
        if (memory_mode.second != Loader::ResultStatus::Success) {
            LOG_CRITICAL(Core, "Failed to determine system mode (Error {})!",
                         static_cast<int>(memory_mode.second));

            switch (memory_mode.second) {
            case Loader::ResultStatus::ErrorEncrypted:
                return ResultStatus::ErrorLoader_ErrorEncrypted;
            case Loader::ResultStatus::ErrorInvalidFormat:
                return ResultStatus::ErrorLoader_ErrorInvalidFormat;
            case Loader::ResultStatus::ErrorGbaTitle:
                return ResultStatus::ErrorLoader_ErrorGbaTitle;
            case Loader::ResultStatus::ErrorArtic:
                return ResultStatus::ErrorArticDisconnected;
            default:
                return ResultStatus::ErrorSystemMode;
            }
        }

        ASSERT(memory_mode.first);
        app_mem_mode = memory_mode.first.value();
    }

    auto n3ds_hw_caps = app_loader->LoadNew3dsHwCapabilities();
    ASSERT(n3ds_hw_caps.first);
    app_n3ds_hw_capabilities = n3ds_hw_caps.first.value();

    // A New 3DS title runs as on a New 3DS, and with Settings' is_new_3ds every title: the New
    // 3DS's Prod memory mode within the Old 3DS's 128 MB of FCRAM (Kernel::MemoryInit), its extra
    // RAM, and a third core for the threads it puts on core 2 (core 3 is the system's). Every
    // other title runs as on an Old 3DS. An Old 3DS title there (Legacy) keeps its Old 3DS
    // memory limit (the loader's commit limit, KernelSystem::UpdateReportedMemory).
    new_3ds = Settings::values.is_new_3ds.GetValue() || app_loader->IsN3DSExclusive();
    if (new_3ds) {
        const Kernel::New3dsMemoryMode n3ds_mode = app_n3ds_hw_capabilities.memory_mode;
        ASSERT_MSG(n3ds_mode == Kernel::New3dsMemoryMode::NewProd ||
                       n3ds_mode == Kernel::New3dsMemoryMode::Legacy,
                   "a title in New 3DS memory mode {}: only Prod's 124 MB fits in FCRAM",
                   static_cast<u32>(n3ds_mode));
        system_mem_mode = Kernel::MemoryMode::NewProd;
    }

    // If the default mem mode has been used, we do not come from a FIRM launch. On real HW
    // however, the home menu is in charge or setting the proper memory mode when launching
    // applications by doing a FIRM launch. Since we launch the application without going
    // through the home menu, we need to emulate the FIRM launch having happened and set the
    // proper memory mode.
    if (used_default_mem_mode) {

        // If we are on the Old 3DS prod mode and the application memory mode does not match, we
        // need to adjust it. We do not need adjustment if we are on the New 3DS prod mode, as that
        // one overrides all the Old 3DS memory modes.
        if (system_mem_mode == Kernel::MemoryMode::Prod && app_mem_mode != system_mem_mode) {
            system_mem_mode = app_mem_mode;
        }

        // If we are on the New 3DS prod mode, and the application needs the New 3DS extended
        // memory mode (only CTRAging is known to do this), adjust the memory mode.
        else if (system_mem_mode == Kernel::MemoryMode::NewProd &&
                 app_n3ds_hw_capabilities.memory_mode == Kernel::New3dsMemoryMode::NewDev1) {
            system_mem_mode = Kernel::MemoryMode::NewDev1;
        }
    }

    // The Old 3DS has two application cores; a New 3DS title gets core 2 as well.
    const u32 num_cores = new_3ds ? 3 : 2;
    ResultStatus init_result{Init(emu_window, secondary_window, system_mem_mode, num_cores)};
    if (init_result != ResultStatus::Success) {
        LOG_CRITICAL(Core, "Failed to initialize system (Error {})!",
                     static_cast<u32>(init_result));
        System::Shutdown();
        return init_result;
    }

    kernel->UpdateCPUAndMemoryState(program_id, app_mem_mode, app_n3ds_hw_capabilities);

    // Restore any parameters that should be carried through a reset.
    if (auto apt = Service::APT::GetModule(*this)) {
        if (restore_deliver_arg.has_value()) {
            apt->GetAppletManager()->SetDeliverArg(restore_deliver_arg);
            restore_deliver_arg.reset();
        }
        if (restore_sys_menu_arg.has_value()) {
            apt->GetAppletManager()->SetSysMenuArg(restore_sys_menu_arg.value());
            restore_sys_menu_arg.reset();
        }
        apt->SetWirelessRebootInfoBuffer(restore_wireless_reboot_info);
    }

    if (restore_plugin_context.has_value()) {
        if (auto plg_ldr = Service::PLGLDR::GetService(*this)) {
            plg_ldr->SetPluginLoaderContext(restore_plugin_context.value());
        }
        restore_plugin_context.reset();
    }

    if (restore_ipc_recorder) {
        kernel->RestoreIPCRecorder(std::move(restore_ipc_recorder));
    }

    std::shared_ptr<Kernel::Process> process;
    const Loader::ResultStatus load_result{app_loader->Load(process)};
    if (Loader::ResultStatus::Success != load_result) {
        LOG_CRITICAL(Core, "Failed to load ROM (Error {})!", load_result);
        System::Shutdown();

        switch (load_result) {
        case Loader::ResultStatus::ErrorEncrypted:
            return ResultStatus::ErrorLoader_ErrorEncrypted;
        case Loader::ResultStatus::ErrorInvalidFormat:
            return ResultStatus::ErrorLoader_ErrorInvalidFormat;
        case Loader::ResultStatus::ErrorGbaTitle:
            return ResultStatus::ErrorLoader_ErrorGbaTitle;
        case Loader::ResultStatus::ErrorPatches:
            return ResultStatus::ErrorLoader_ErrorPatches;
        case Loader::ResultStatus::ErrorPatchesInvalidTitle:
            return ResultStatus::ErrorLoader_ErrorPatchesInvalidTitle;
        case Loader::ResultStatus::ErrorArtic:
            return ResultStatus::ErrorArticDisconnected;
        default:
            return ResultStatus::ErrorLoader;
        }
    }
    kernel->SetCurrentProcess(process);
    title_id = 0;
    if (app_loader->ReadProgramId(title_id) != Loader::ResultStatus::Success) {
        LOG_ERROR(Core, "Failed to find title id for ROM (Error {})",
                  static_cast<u32>(load_result));
    }

#if defined(__vita__)
    // Cheats are off on the Vita build: every cheat write goes through
    // InvalidateCacheRange, which on the native core is a kernel round trip that cleans the
    // page and drops core 2's icache, once a frame per cheat line.
    LOG_INFO(Core, "cheats are disabled on this build");
#else
    cheat_engine.LoadCheatFile(title_id);
    cheat_engine.Connect(process->process_id);
#endif

    perf_stats = std::make_unique<PerfStats>(title_id);

    status = ResultStatus::Success;
    m_emu_window = &emu_window;
    m_secondary_window = secondary_window;
    m_filepath = filepath;

    // Reset counters and set time origin to current frame
    [[maybe_unused]] const PerfStats::Results result = GetAndResetPerfStats();
    perf_stats->BeginSystemFrame();
    return status;
}

void System::SelectCore(u32 core_id) {
    SetRunning(cpu_cores[core_id].get());
}

void System::SetRunning(ARM_Interface* core) {
    running_core[ThisHost()] = core;
    kernel->SetRunningCPU(core);
}

void System::PrepareReschedule() {
    const u32 host = ThisHost();
    running_core[host]->PrepareReschedule();
    curr_core_reschedule_pending[host] = true;
}

PerfStats::Results System::GetAndResetPerfStats() {
    return (perf_stats && timing) ? perf_stats->GetAndResetStats(timing->GetGlobalTimeUs())
                                  : PerfStats::Results{};
}

PerfStats::Results System::GetLastPerfStats() {
    return perf_stats ? perf_stats->GetLastStats() : PerfStats::Results{};
}

double System::GetStableFrameTimeScale() {
    return perf_stats->GetStableFrameTimeScale();
}

void System::Reschedule() {
    const u32 host = ThisHost();
    if (!curr_core_reschedule_pending[host]) {
        return;
    }

    curr_core_reschedule_pending[host] = false;
    kernel->GetThreadManager(running_core[host]->GetID()).Reschedule();
}

std::span<const std::shared_ptr<ARM_Interface>> System::HostCores(u32 host) const {
    const std::span<const std::shared_ptr<ARM_Interface>> all{cpu_cores};
    if (!multi_host.load(std::memory_order_relaxed)) {
        return host == 0 ? all : all.first(0);
    }
    return host == 0 ? all.first(1) : all.subspan(1);
}

bool System::SecondHostCanStart() const {
#ifdef CITRA_HAS_VITA_NATIVE
    return static_cast<const ARM_VitaNative&>(*cpu_cores[0]).GuestTableInstalled();
#else
    return false;
#endif
}

void System::PublishClock(u32 host) {
    const u64 now = HostCores(host).front()->GetTimer().GetTicks();
    host_clock[host].clock.store(now, std::memory_order_seq_cst);
    // The other one idles until this time reaches its next event (HostIdle).
    const HostClock& other = host_clock[host ^ 1];
    if (other.idle.load(std::memory_order_seq_cst) &&
        now >= other.next.load(std::memory_order_relaxed)) {
#ifdef VITA_DIAGNOSTICS
        host_clock[host].woke_other_us.store(
            static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count()),
            std::memory_order_relaxed);
#endif
        kernel->GetHostInbox(host ^ 1).Wake();
    }
}

bool System::SecondHostQuiet(u64 until) const {
    const HostClock& second = host_clock[1];
    return second.idle.load(std::memory_order_seq_cst) &&
           second.next.load(std::memory_order_relaxed) > until &&
           !kernel->GetHostInbox(1).Pending() && kernel->GetHostInbox(0).Empty() &&
           !forward_pending.load(std::memory_order_acquire);
}

#ifdef VITA_DIAGNOSTICS
u32 System::SecondHostBusyWhy(u64 until) const {
    const HostClock& second = host_clock[1];
    if (!second.idle.load(std::memory_order_seq_cst)) {
        return 0;
    }
    if (second.next.load(std::memory_order_relaxed) <= until) {
        return 1;
    }
    if (kernel->GetHostInbox(1).Pending()) {
        return 2;
    }
    if (!kernel->GetHostInbox(0).Empty()) {
        return 3;
    }
    return forward_pending.load(std::memory_order_acquire) ? 4 : 5;
}
#endif

void System::HostIdle(u32 host) {
    const auto cores = HostCores(host);
    HostClock& me = host_clock[host];
    const HostClock& other = host_clock[host ^ 1];
    Kernel::HostInbox& inbox = kernel->GetHostInbox(host);
    const u64 now = cores.front()->GetTimer().GetTicks();
    s64 slice = Timing::MAX_SLICE_LENGTH;
    for (const auto& core : cores) {
        slice = std::min(slice, core->GetTimer().GetMaxSliceLength());
    }
    u64 next = now + static_cast<u64>(std::max<s64>(slice, 0));
    // The second thread with no guest thread at all: nothing of its timer needs it awake. A thread
    // made for it comes as a message, which wakes it.
    if (host == 1 && std::all_of(cores.begin(), cores.end(), [&](const auto& core) {
            return kernel->GetThreadManager(core->GetID()).GetThreadList().empty();
        })) {
        next = ~0ull;
    }
    me.next.store(next, std::memory_order_relaxed);
    me.clock.store(now, std::memory_order_relaxed);
    // No wake for the other here: of two hosts going idle, one sees the other idle (both store
    // their own flag before reading the other's), goes to the sooner of their next events and wakes
    // the other when its own is due (below). A wake at every idle is a syscall for nothing.
    me.idle.store(true, std::memory_order_seq_cst);
    // Something to do at once: a message, host 1's stop, host 0's forwarded svc.
    const auto work = [&] {
        return !inbox.Empty() || (host == 0 ? forward_pending.load(std::memory_order_acquire)
                                            : second_host_stop.load(std::memory_order_acquire));
    };
    // The other host idle: no thread to run and no message waiting for it (a message runs a
    // thread there, at its clock).
    Kernel::HostInbox& other_inbox = kernel->GetHostInbox(host ^ 1);
    const auto other_idles = [&] {
        return other.idle.load(std::memory_order_seq_cst) && !other_inbox.Pending();
    };
    u64 to = now;
#ifdef VITA_DIAGNOSTICS
    bool slept_any = false, at_work = false, last_other_idle = false;
#endif
    for (;;) {
        const bool other_idle = other_idles();
        const u64 other_clock = other.clock.load(std::memory_order_seq_cst);
#ifdef VITA_DIAGNOSTICS
        last_other_idle = other_idle;
#endif
        if (work()) {
            // Woken: where the other one runs, at most to this one's next event.
            to = other_idle ? now : std::clamp(other_clock, now, next);
#ifdef VITA_DIAGNOSTICS
            at_work = true;
#endif
            break;
        }
        // The clock jumps to the next event only with both hosts idle (the sooner of their next
        // events); while the other runs, this one follows its clock, up to its own next event. A
        // jump past the running one let host 0 run its events before the other's threads answered
        // them: DSP frames written a tick late, ticks in bursts, frames paced wrong. Following
        // exactly makes host 0 sleep for the second thread's clock; a slice of slack each way, or
        // none only before the DSP's tick, let the second thread idle past host 0 and take its
        // interrupts in its past. Most of the sleep is the second thread's own run, not the wake: a
        // spin before the sleep gained nothing.
        to = std::min(next, other_idle ? other.next.load(std::memory_order_seq_cst) : other_clock);
        if (to > now) {
            break;
        }
        // Both idle, and the other's next event due by this clock (it is behind): it moves.
        if (other_idle && now >= other.next.load(std::memory_order_seq_cst)) {
            kernel->GetHostInbox(host ^ 1).Wake();
        }
#ifdef VITA_DIAGNOSTICS
        const auto slept_from = std::chrono::steady_clock::now();
#endif
        inbox.SleepUntil([&] {
            return work() || other_idles() != other_idle ||
                   other.clock.load(std::memory_order_seq_cst) >= next;
        });
#ifdef VITA_DIAGNOSTICS
        slept_any = true;
        const u64 slept = static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                               std::chrono::steady_clock::now() - slept_from)
                                               .count());
        (host == 0 ? second_census.host0_idle_us : second_census.idle_us) += slept;
        // What ended host 0's sleep: a message (or forwarded svc), the other's clock at its next
        // event, the other going idle or running again.
        if (host == 0) {
            const u32 why = work()                                                   ? 0
                            : other.clock.load(std::memory_order_relaxed) >= next ? 1
                            : other_idles() != other_idle                         ? 2
                                                                                  : 3;
            second_census.host0_woken[why]++;
            second_census.host0_woken_us[why] += slept;
            if (why == 1) {
                const u64 from_us = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        slept_from.time_since_epoch())
                        .count());
                const u64 woke_us = other.woke_other_us.load(std::memory_order_relaxed);
                if (woke_us >= from_us) {
                    second_census.clock_wakes_timed++;
                    second_census.clock_wake_latency_us += from_us + slept - woke_us;
                }
                constexpr std::array<u64, 5> limits{10, 25, 50, 100, 250};
                second_census.clock_sleeps_by_length[static_cast<std::size_t>(
                    std::upper_bound(limits.begin(), limits.end(), slept) - limits.begin())]++;
            }
        }
#endif
    }
#ifdef VITA_DIAGNOSTICS
    (host == 0 ? second_census.host0_idles : second_census.idles)++;
    if (host == 0) {
        const u32 how = slept_any ? 4 : at_work ? 0 : to >= next ? (last_other_idle ? 1 : 2) : 3;
        second_census.host0_idle_how[how]++;
        if (!slept_any && to > now) {
            second_census.host0_jump_ticks += to - now;
        }
    }
#endif
    me.idle.store(false, std::memory_order_seq_cst);
    if (to > now) {
        for (const auto& core : cores) {
            Timing::Timer& timer = core->GetTimer();
            const u64 at = timer.GetTicks();
            if (to > at) {
                timer.SetNextSlice(static_cast<s64>(to - at));
                timer.Idle();
            }
        }
        me.clock.store(to, std::memory_order_seq_cst);
        if (other.idle.load(std::memory_order_seq_cst) &&
            to >= other.next.load(std::memory_order_relaxed)) {
            kernel->GetHostInbox(host ^ 1).Wake();
        }
    }
}

void System::MoveWaitsToFirstHost(bool starting) {
    for (u32 core = 1; core < cpu_cores.size(); core++) {
        auto& manager = kernel->GetThreadManager(core);
        for (const auto& thread : manager.GetThreadList()) {
            switch (thread->status) {
            case Kernel::ThreadStatus::WaitSynchAny:
            case Kernel::ThreadStatus::WaitSynchAll:
            case Kernel::ThreadStatus::WaitHleEvent:
            case Kernel::ThreadStatus::WaitIPC:
            case Kernel::ThreadStatus::WaitArb:
                break;
            default:
                continue; // sleeps stay on their own core's timer
            }
            if (starting) {
                // A wait host 0 made: it stays host 0's, its timeout on host 0's timer.
                timing->MoveEvent(manager.WakeupEventType(), thread->thread_id, core, 0);
            } else if (thread->wait_host == 1) {
                // Host 1's event or arbiter wait: into host 0's lists, which host 0 alone wakes
                // now.
                for (const auto& object : thread->wait_objects) {
                    if (object->GetHandleType() == Kernel::HandleType::Event) {
                        static_cast<Kernel::Event&>(*object).MoveWaiter(thread.get(), 1, 0);
                    }
                }
                if (thread->status == Kernel::ThreadStatus::WaitArb &&
                    thread->wait_arbiter != nullptr) {
                    thread->wait_arbiter->MoveWaiterToFirstHost(thread.get());
                }
                thread->wait_host = 0;
            }
        }
    }
}

#ifdef VITA_DIAGNOSTICS
System::SecondHostCensus System::TakeSecondHostCensus() {
    const SecondHostCensus taken = second_census;
    second_census = {};
    return taken;
}

void System::DumpGuestThreads() {
    for (u32 c = 0; c < cpu_cores.size(); c++) {
        auto& manager = kernel->GetThreadManager(c);
        const Kernel::Thread* const current = manager.GetCurrentThread();
        std::string line = fmt::format("threads: core {} (host {}, clock {}) current {}:", c,
                                       HostOfCore(c), cpu_cores[c]->GetTimer().GetTicks(),
                                       current != nullptr ? current->thread_id : 0);
        for (const auto& t : manager.GetThreadList()) {
            fmt::format_to(std::back_inserter(line),
                           " [{} p{} s{} h{}{}{} pc {:08x} lr {:08x} r0 {:08x}", t->thread_id,
                           t->current_priority, static_cast<u32>(t->status), t->wait_host,
                           t->handoff_pending ? " handoff" : "",
                           t->wakeup_queued ? " timed" : "", t->context.cpu_registers[15],
                           t->context.cpu_registers[14], t->context.cpu_registers[0]);
            for (const auto& o : t->wait_objects) {
                fmt::format_to(std::back_inserter(line), " {}:{}", o->GetTypeName(), o->GetName());
                if (o->GetHandleType() == Kernel::HandleType::Event) {
                    const auto st = static_cast<const Kernel::Event&>(*o).DebugState();
                    fmt::format_to(std::back_inserter(line), "({:x} {}/{})", st[0], st[1],
                                   st[2]);
                }
            }
            if (t->status == Kernel::ThreadStatus::WaitArb) {
                fmt::format_to(std::back_inserter(line), " arb {:08x}", t->wait_address);
            }
            line += "]";
        }
        LOG_INFO(Core, "{}", line);
    }
    if (cpu_cores.size() > 1) {
        LOG_INFO(Core, "threads: core 1's CPU limiter: {}",
                 kernel->GetThreadManager(1).GetCpuLimiter()->DebugState());
    }
    LOG_INFO(Core, "threads: hosts idle {}/{} clocks {}/{} next {}/{}, forward {}",
             host_clock[0].idle.load(), host_clock[1].idle.load(), host_clock[0].clock.load(),
             host_clock[1].clock.load(), host_clock[0].next.load(), host_clock[1].next.load(),
             forward_pending.load());
}
#endif

void System::StartSecondHost() {
#ifdef CITRA_HAS_VITA_NATIVE
    static_cast<ARM_VitaNative&>(*cpu_cores[0]).PrepareSecondHost();
    second_host_stop.store(false, std::memory_order_relaxed);
    second_host_attached.store(false, std::memory_order_relaxed);
    second_failed.store(false, std::memory_order_relaxed);
    const u64 now = cpu_cores[0]->GetTimer().GetTicks();
    for (HostClock& c : host_clock) {
        c.clock.store(now, std::memory_order_relaxed);
        c.next.store(now, std::memory_order_relaxed);
        c.idle.store(false, std::memory_order_relaxed);
    }
    // Created on its own core: azaharAttach joins the core it is called on, and the role's pin
    // need not move a thread before its next reschedule. Begun on this core, which waits below,
    // it could attach here and run the other cores' guest in host 0's view (Super Monkey Ball's
    // undefined instruction after a savestate); azaharnative 74 refuses such an attach.
    second_host = std::make_unique<Common::NamedThread>(
        Common::ThreadCfg{"emulation-2", 1024 * 1024,
                          Common::RoleCoreMask(Common::ThreadRole::SecondEmulation)},
        [this] { SecondHostMain(); });
    Kernel::HostInbox& inbox = kernel->GetHostInbox(0);
    while (!second_host_attached.load(std::memory_order_acquire) &&
           !second_failed.load(std::memory_order_acquire)) {
        inbox.SleepUntil([&] {
            return second_host_attached.load(std::memory_order_acquire) ||
                   second_failed.load(std::memory_order_acquire);
        });
    }
    if (second_failed.load(std::memory_order_acquire)) {
        second_host->join();
        second_host.reset();
        return; // RunLoop takes its status
    }
    // Every core is host 0's until here: the waits it made for the others time out on its own
    // timer from now on; then the cores are the second thread's.
    MoveWaitsToFirstHost(true);
    multi_host.store(true, std::memory_order_seq_cst);
    kernel->GetHostInbox(1).Wake();
    LOG_INFO(Core, "second emulation thread: cores 1-{} on their own Vita core",
             cpu_cores.size() - 1);
#endif
}

void System::StopSecondHost() {
    if (!second_host) {
        return;
    }
    second_host_stop.store(true, std::memory_order_seq_cst);
    kernel->GetHostInbox(1).Wake();
    // It may wait for an svc of its own meanwhile.
    Kernel::HostInbox& inbox = kernel->GetHostInbox(0);
    while (second_host_attached.load(std::memory_order_acquire)) {
        ServeForward();
        inbox.SleepUntil([&] {
            return !second_host_attached.load(std::memory_order_acquire) ||
                   forward_pending.load(std::memory_order_acquire);
        });
    }
    second_host->join();
    second_host.reset();
    multi_host.store(false, std::memory_order_seq_cst);
    host1_tp = 0;
    running_core[1] = cpu_cores[0].get();
    kernel->current_cpu[1] = nullptr;
    MoveWaitsToFirstHost(false);
    // What was on its way to the second thread is host 0's now.
    kernel->ProcessHostMessages(1);
    kernel->ProcessHostMessages(0);
    LOG_INFO(Core, "second emulation thread stopped");
}

void System::SecondHostSvc(u32 core_id, Kernel::SVCContext& svc, u32 immediate) {
    if (svc.LocalToSecondHost(immediate)) {
#ifdef VITA_DIAGNOSTICS
        second_census.local_svcs++;
        second_census.local_svcs_by[immediate & 0xFF]++;
#endif
        svc.CallSVCLocal(immediate);
        return;
    }
#ifdef VITA_DIAGNOSTICS
    second_census.forwards++;
    second_census.forward_svcs[immediate & 0xFF]++;
    const auto forward_from = std::chrono::steady_clock::now();
#endif
    forward_kind = ForwardKind::Svc;
    forward_core = core_id;
    forward_immediate = immediate;
    forward_svc = &svc;
    ForwardToFirstHost();
#ifdef VITA_DIAGNOSTICS
    second_census.forward_wait_us += static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                              forward_from)
            .count());
#endif
    // Host 0 rescheduled the core after the svc: its thread may be another.
    PrepareReschedule();
}

bool System::SecondHostHeldPage(u32 core_id) {
#ifdef VITA_DIAGNOSTICS
    second_census.held_pages++;
#endif
    forward_kind = ForwardKind::HeldPage;
    forward_core = core_id;
    ForwardToFirstHost();
    return forward_taken;
}

void System::ForwardToFirstHost() {
    host1_parked.store(true, std::memory_order_seq_cst);
    forward_pending.store(true, std::memory_order_seq_cst);
    kernel->GetHostInbox(0).Wake();
    Kernel::HostInbox& inbox = kernel->GetHostInbox(1);
    while (forward_pending.load(std::memory_order_acquire)) {
        inbox.SleepUntil([&] { return !forward_pending.load(std::memory_order_acquire); });
    }
    host1_parked.store(false, std::memory_order_seq_cst);
}

void System::ServeForward() {
    if (!forward_pending.load(std::memory_order_acquire)) {
        return;
    }
    // The core's register file, thread manager and process are the svc's; the waits it makes
    // time out on host 0's timer (core 0's), whose events are host 0's to run.
    ARM_Interface* const own = running_core[0];
    SetRunning(cpu_cores[forward_core].get());
    timing->SetCurrentTimer(0);
#ifdef CITRA_HAS_VITA_NATIVE
    auto& core = static_cast<ARM_VitaNative&>(*cpu_cores[forward_core]);
    if (forward_kind == ForwardKind::HeldPage) {
        // The second core stores to pages the GPU's queued work holds.
        forward_taken = core.TakeForwardedHeldPage();
#ifdef VITA_DIAGNOSTICS
        second_census.raced_holds += core.forward_raced ? 1 : 0;
#endif
    } else {
        core.CallForwardedSvc(forward_immediate);
        // The core's thread switch, here: a thread the svc put to wait has its registers saved
        // before host 0 may wake it and write its result.
        kernel->GetThreadManager(forward_core).Reschedule();
    }
#else
    forward_svc->CallSVC(forward_immediate);
    kernel->GetThreadManager(forward_core).Reschedule();
#endif
    SetRunning(own);
    forward_pending.store(false, std::memory_order_seq_cst);
    kernel->GetHostInbox(1).Wake();
#ifdef VITA_DIAGNOSTICS
    second_census.served++;
#endif
}

void System::SecondHostMain() {
#ifdef CITRA_HAS_VITA_NATIVE
    Common::SetCurrentThreadRole(Common::ThreadRole::SecondEmulation);
    u32 tp;
    asm volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
    host1_tp = tp; // ThisHost() is 1 on this thread from here on
    Kernel::HostInbox& inbox = kernel->GetHostInbox(1);
    if (!ARM_VitaNative::AttachSecondHost()) {
        SetStatus(ResultStatus::ErrorCoreExceptionRaised,
                  "the second emulation thread's core could not join the guest");
        kernel->GetHostInbox(0).Wake();
        return;
    }
    second_host_attached.store(true, std::memory_order_seq_cst);
    kernel->GetHostInbox(0).Wake();
    while (!multi_host.load(std::memory_order_acquire) &&
           !second_host_stop.load(std::memory_order_acquire)) {
        inbox.SleepUntil([&] {
            return multi_host.load(std::memory_order_acquire) ||
                   second_host_stop.load(std::memory_order_acquire);
        });
    }
    const auto cores = HostCores(1);
    SetRunning(cores.front().get());
    while (!second_host_stop.load(std::memory_order_acquire) &&
           !second_failed.load(std::memory_order_acquire)) {
        kernel->ProcessHostMessages();
        // RunLoop's pass over these cores: each core's events, then each runs up to where the
        // one before it got.
        s64 max_slice = Timing::MAX_SLICE_LENGTH;
        bool any = false;
        for (const auto& core : cores) {
            SetRunning(core.get());
            core->GetTimer().Advance();
            core->PrepareReschedule();
            kernel->GetThreadManager(core->GetID()).Reschedule();
            max_slice = std::min(max_slice, core->GetTimer().GetMaxSliceLength());
            any |= kernel->GetCurrentThreadManager().GetCurrentThread() != nullptr;
        }
#ifdef VITA_DIAGNOSTICS
        second_census.passes++;
#endif
        if (!any) {
            HostIdle(1);
            continue;
        }
        // The cores with a thread run, each up to where the one before it got; then the idle
        // ones follow the furthest, never ahead of it (an idle core first in the pass would run a
        // whole slice ahead each pass and take the clock with it).
        std::optional<u64> reached;
        u32 ran = 0;
        for (const auto& core : cores) {
            SetRunning(core.get());
            if (kernel->GetCurrentThreadManager().GetCurrentThread() == nullptr) {
                continue;
            }
            ran |= 1u << core->GetID();
            if (reached) {
                const u64 at = core->GetTimer().GetTicks();
                max_slice = *reached > at ? static_cast<s64>(*reached - at) : 0;
            }
            core->GetTimer().SetNextSlice(max_slice);
            const u64 start = core->GetTimer().GetTicks();
#ifdef VITA_DIAGNOSTICS
            second_census.runs++;
#endif
            core->Run();
            max_slice = static_cast<s64>(core->GetTimer().GetTicks() - start);
            reached = std::max(reached.value_or(0), core->GetTimer().GetTicks());
            Reschedule();
        }
        for (const auto& core : cores) {
            if ((ran >> core->GetID() & 1) != 0) {
                continue; // its slice is its timer's until the next Advance
            }
            Timing::Timer& timer = core->GetTimer();
            const u64 at = timer.GetTicks();
            if (*reached > at) {
                SetRunning(core.get());
                timer.SetNextSlice(static_cast<s64>(*reached - at));
                timer.Idle();
            }
        }
        PublishClock(1);
#ifdef VITA_DIAGNOSTICS
        second_census.lead = static_cast<s64>(host_clock[1].clock.load(std::memory_order_relaxed) -
                                              host_clock[0].clock.load(std::memory_order_relaxed));
#endif
    }
    ARM_VitaNative::DetachSecondHost();
    second_host_attached.store(false, std::memory_order_seq_cst);
    kernel->GetHostInbox(0).Wake();
#endif
}

System::ResultStatus System::Init(Frontend::EmuWindow& emu_window,
                                  Frontend::EmuWindow* secondary_window,
                                  Kernel::MemoryMode memory_mode, u32 num_cores) {
    LOG_DEBUG(HW_Memory, "initialized OK");
    state_load_partial = false;

    memory = std::make_unique<Memory::MemorySystem>(*this);

    timing = std::make_unique<Timing>(num_cores, Settings::values.cpu_clock_percentage.GetValue(),
                                      movie.GetOverrideBaseTicks());

    kernel = std::make_unique<Kernel::KernelSystem>(
        *memory, *timing, [this] { PrepareReschedule(); }, memory_mode, num_cores,
        movie.GetOverrideInitTime());

    exclusive_monitor = MakeExclusiveMonitor(*memory, num_cores);
    cpu_cores.reserve(num_cores);

#ifdef CITRA_HAS_VITA_NATIVE
    // The PS Vita runs guest code natively and only natively: on a core taken from the Vita's
    // kernel, through the azaharnative module. Without the module there is no CPU to run on.
    const auto map = ARM_VitaNative::CreateMap(*memory);
    if (!map) {
        LOG_CRITICAL(Core, "Vita native execution unavailable: the azaharnative kernel module is "
                           "not loaded, or it could not take a core");
        return ResultStatus::ErrorNotInitialized;
    }
    for (u32 i = 0; i < num_cores; ++i) {
        cpu_cores.push_back(
            std::make_shared<ARM_VitaNative>(*this, *memory, i, timing->GetTimer(i), map));
    }
#else
#ifdef CITRA_HAS_NATIVE_ARM
    // On a 32-bit ARM host the guest's own instructions are host instructions, so they are executed
    // directly rather than interpreted or recompiled. Both emulated cores share one address space
    // and one host thread, so both can run this way against a single address space mirror.
    // Turning the JIT setting off asks for the interpreter here as it does everywhere else, which
    // is how a picture drawn over native execution is compared against one drawn over the
    // interpreter on the same host.
    if (auto mirror = Settings::values.use_cpu_jit ? ARM_Native::CreateMirror(*memory) : nullptr) {
        for (u32 i = 0; i < num_cores; ++i) {
            cpu_cores.push_back(
                std::make_shared<ARM_Native>(*this, *memory, i, timing->GetTimer(i), mirror));
        }
        LOG_INFO(Core, "Executing guest code natively on {} core(s)", num_cores);
    } else {
        LOG_WARNING(Core, "Native execution unavailable; falling back to interpretation");
    }
#endif
    if (cpu_cores.empty()) {
        if (Settings::values.use_cpu_jit) {
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
            for (u32 i = 0; i < num_cores; ++i) {
                cpu_cores.push_back(std::make_shared<ARM_Dynarmic>(
                    *this, *memory, i, timing->GetTimer(i), *exclusive_monitor));
            }
#else
            for (u32 i = 0; i < num_cores; ++i) {
                cpu_cores.push_back(std::make_shared<ARM_DynCom>(*this, *memory, USER32MODE, i,
                                                                 timing->GetTimer(i)));
            }
            LOG_WARNING(Core, "CPU JIT requested, but Dynarmic not available");
#endif
        } else {
            for (u32 i = 0; i < num_cores; ++i) {
                cpu_cores.push_back(std::make_shared<ARM_DynCom>(*this, *memory, USER32MODE, i,
                                                                 timing->GetTimer(i)));
            }
        }
    }
#endif // CITRA_HAS_VITA_NATIVE
    running_core.fill(cpu_cores[0].get());

    kernel->SetCPUs(cpu_cores);
    kernel->SetRunningCPU(cpu_cores[0].get());

    const auto audio_emulation = Settings::values.audio_emulation.GetValue();
#ifdef ENABLE_DSP_LLE
    if (audio_emulation == Settings::AudioEmulation::HLE) {
        dsp_core = std::make_unique<AudioCore::DspHle>(*this);
    } else {
        const bool multithread = audio_emulation == Settings::AudioEmulation::LLEMultithreaded;
        dsp_core = std::make_unique<AudioCore::DspLle>(*this, multithread);
    }
#else
    // Built without the LLE backend (see ENABLE_DSP_LLE): the setting still exists in saved
    // configurations, so honour it by falling back rather than failing to boot.
    if (audio_emulation != Settings::AudioEmulation::HLE) {
        LOG_WARNING(Core, "This build has no LLE DSP; using the HLE DSP instead");
    }
    dsp_core = std::make_unique<AudioCore::DspHle>(*this);
#endif

    dsp_core->SetSink(Settings::values.output_type.GetValue(),
                      Settings::values.output_device.GetValue());
    dsp_core->EnableStretching(Settings::values.enable_audio_stretching.GetValue());

#ifdef ENABLE_SCRIPTING
    if (Settings::values.enable_rpc_server.GetValue()) {
        rpc_server = std::make_unique<RPC::Server>(*this);
    }
#endif

    service_manager = std::make_unique<Service::SM::ServiceManager>(*this);
    archive_manager = std::make_unique<Service::FS::ArchiveManager>(*this);

    u64 loading_title_id = 0;
    app_loader->ReadProgramId(loading_title_id);
    HW::AES::InitKeys();
    Service::Init(*this, loading_title_id, lle_modules, !app_loader->DoingInitialSetup());
#ifdef ENABLE_GDBSTUB
    GDBStub::DeferStart();
#endif

    if (!registered_image_interface) {
        registered_image_interface = std::make_shared<Frontend::ImageInterface>();
    }

    auto gsp = service_manager->GetService<Service::GSP::GSP_GPU>("gsp::Gpu");
    // Free the old one first. Assigning over it constructs the replacement while it is still
    // alive, so both renderers hold their GL resources at once - two vertex rings, two texture
    // caches - and on a modest GPU the second allocation simply fails, in a state load, which
    // re-enters Init.
    gpu.reset();
    gpu = std::make_unique<VideoCore::GPU>(*this, emu_window, secondary_window);
    gpu->SetInterruptHandler([gsp](Service::GSP::InterruptId interrupt_id, u64 wait_delay_ns) {
        gsp->SignalInterrupt(interrupt_id, wait_delay_ns);
    });

    auto plg_ldr = Service::PLGLDR::GetService(*this);
    if (plg_ldr) {
        plg_ldr->SetEnabled(Settings::values.plugin_loader_enabled.GetValue());
        plg_ldr->SetAllowGameChangeState(Settings::values.allow_plugin_loader.GetValue());
    }

    SetInfoLEDColor({});

    LOG_DEBUG(Core, "Initialized OK");

    is_powered_on = true;

    return ResultStatus::Success;
}

System::ResultStatus System::InitGpuOnly(Frontend::EmuWindow& emu_window) {
    memory = std::make_unique<Memory::MemorySystem>(*this);
    timing = std::make_unique<Timing>(1, Settings::values.cpu_clock_percentage.GetValue());
    gpu = std::make_unique<VideoCore::GPU>(*this, emu_window, nullptr);
    LOG_INFO(Core, "memory, timing and the GPU, no application");
    return ResultStatus::Success;
}

void System::ShutdownGpuOnly() {
    gpu.reset();
    timing.reset();
    memory.reset();
}

VideoCore::GPU& System::GPU() {
    return *gpu;
}

Service::SM::ServiceManager& System::ServiceManager() {
    return *service_manager;
}

const Service::SM::ServiceManager& System::ServiceManager() const {
    return *service_manager;
}

Service::FS::ArchiveManager& System::ArchiveManager() {
    return *archive_manager;
}

const Service::FS::ArchiveManager& System::ArchiveManager() const {
    return *archive_manager;
}

Kernel::KernelSystem& System::Kernel() {
    return *kernel;
}

const Kernel::KernelSystem& System::Kernel() const {
    return *kernel;
}

bool System::KernelRunning() {
    return kernel != nullptr;
}

Timing& System::CoreTiming() {
    return *timing;
}

const Timing& System::CoreTiming() const {
    return *timing;
}

Memory::MemorySystem& System::Memory() {
    return *memory;
}

const Memory::MemorySystem& System::Memory() const {
    return *memory;
}

Cheats::CheatEngine& System::CheatEngine() {
    return cheat_engine;
}

const Cheats::CheatEngine& System::CheatEngine() const {
    return cheat_engine;
}

void System::RegisterVideoDumper(std::shared_ptr<VideoDumper::Backend> dumper) {
    video_dumper = std::move(dumper);
}

Core::Movie& System::Movie() {
    return movie;
}

const Core::Movie& System::Movie() const {
    return movie;
}

void System::RegisterMiiSelector(std::shared_ptr<Frontend::MiiSelector> mii_selector) {
    registered_mii_selector = std::move(mii_selector);
}

void System::RegisterSoftwareKeyboard(std::shared_ptr<Frontend::SoftwareKeyboard> swkbd) {
    registered_swkbd = std::move(swkbd);
}

void System::RegisterImageInterface(std::shared_ptr<Frontend::ImageInterface> image_interface) {
    registered_image_interface = std::move(image_interface);
}

// Milestones of a state load, printed at info level: a load that stops without an error
// needs to say where. Cheap: one line per section.
static void SavestateTrace(const char* what) {
    LOG_INFO(Core, "savestate: {}", what);
}

void System::Shutdown(bool is_deserializing) {
    StopSecondHost();

    // Shutdown emulation session
    is_powered_on = false;

    gpu.reset();
    if (is_deserializing) {
        SavestateTrace("gpu down");
    }
    if (!is_deserializing) {
        lle_modules.clear();
#ifdef ENABLE_GDBSTUB
        GDBStub::Shutdown();
#endif
        perf_stats.reset();
        app_loader.reset();
    }
#ifdef ENABLE_SCRIPTING
    rpc_server.reset();
#endif
    archive_manager.reset();
    if (is_deserializing) {
        SavestateTrace("archives down");
    }
    service_manager.reset();
    if (is_deserializing) {
        SavestateTrace("services down");
    }
    dsp_core.reset();
    if (is_deserializing) {
        SavestateTrace("dsp down");
    }
    kernel.reset();
    if (is_deserializing) {
        SavestateTrace("kernel down");
    }
    cpu_cores.clear();
    if (is_deserializing) {
        SavestateTrace("cpu cores down");
    }
    exclusive_monitor.reset();
    timing.reset();
    if (is_deserializing) {
        SavestateTrace("timing down");
    }

    if (video_dumper && video_dumper->IsDumping()) {
        video_dumper->StopDumping();
    }

#ifdef ENABLE_ROOM
    if (auto room_member = Network::GetRoomMember().lock()) {
        Network::GameInfo game_info{};
        room_member->SendGameInfo(game_info);
    }
#endif

    memory.reset();
    if (is_deserializing) {
        SavestateTrace("memory down");
    }

    SetInfoLEDColor({});

    LOG_DEBUG(Core, "Shutdown OK");
}

void System::Reset() {
    // This is NOT a proper reset, but a temporary workaround by shutting down the system and
    // reloading.
    // TODO: Properly implement the reset

    // Save the APT deliver arg and plugin loader context across resets.
    // This is needed as we don't currently support proper app jumping.
    if (auto apt = Service::APT::GetModule(*this)) {
        restore_deliver_arg = apt->GetAppletManager()->ReceiveDeliverArg();
        restore_sys_menu_arg = apt->GetAppletManager()->GetSysMenuArg();
        restore_wireless_reboot_info = apt->GetWirelessRebootInfoBuffer();
    }
    if (auto plg_ldr = Service::PLGLDR::GetService(*this)) {
        restore_plugin_context = plg_ldr->GetPluginLoaderContext();
    }

    restore_ipc_recorder = std::move(kernel->BackupIPCRecorder());

    Shutdown();

    if (!m_chainloadpath.empty()) {
        m_filepath = m_chainloadpath;
        m_chainloadpath.clear();
    }

    // Reload the system with the same setting
    const System::ResultStatus result = Load(*m_emu_window, m_filepath, m_secondary_window);
    if (result == ResultStatus::Success && reset_callback) {
        reset_callback();
    }
}

System::CodeSyncBatch::CodeSyncBatch(System& system_) : system{system_} {
    ASSERT_MSG(system.code_sync_batch == nullptr && ThisHost() == 0,
               "a code sync batch inside another, or off host 0");
    system.code_sync_batch = this;
}

System::CodeSyncBatch::~CodeSyncBatch() {
    system.code_sync_batch = nullptr;
    std::sort(runs.begin(), runs.end());
    for (std::size_t i = 0; i < runs.size();) {
        const u32 first = runs[i].first;
        u32 end = runs[i].second;
        for (++i; i < runs.size() && runs[i].first <= end; ++i) {
            end = std::max(end, runs[i].second);
        }
        system.InvalidateCacheRange(first << Memory::CITRA_PAGE_BITS,
                                    static_cast<std::size_t>(end - first) << Memory::CITRA_PAGE_BITS);
    }
}

void System::CodeSyncBatch::Add(u32 start_address, std::size_t length) {
    if (length == 0) {
        return;
    }
    const u32 first = start_address >> Memory::CITRA_PAGE_BITS;
    const u32 end =
        static_cast<u32>((u64{start_address} + length - 1) >> Memory::CITRA_PAGE_BITS) + 1;
    // Relocations come in a segment's order: most extend the last run.
    if (!runs.empty() && first <= runs.back().second && end >= runs.back().first) {
        runs.back().first = std::min(runs.back().first, first);
        runs.back().second = std::max(runs.back().second, end);
        return;
    }
    runs.emplace_back(first, end);
}

void System::ApplySettings() {
#ifdef ENABLE_GDBSTUB
    if (override_gdb_port != -1) {
        GDBStub::SetServerPort(override_gdb_port);
        GDBStub::ToggleServer(true);
    } else {
        GDBStub::SetServerPort(Settings::values.gdbstub_port.GetValue());
        GDBStub::ToggleServer(Settings::values.use_gdbstub.GetValue());
    }
#endif

    if (gpu) {
        // The renderer's: with the render thread held.
        const auto hold = gpu->HoldRender();
#ifndef ANDROID
        gpu->UpdateFramebufferLayout();
#endif
    }

    if (IsPoweredOn()) {
        CoreTiming().UpdateClockSpeed(Settings::values.cpu_clock_percentage.GetValue());
        dsp_core->SetSink(Settings::values.output_type.GetValue(),
                          Settings::values.output_device.GetValue());
        dsp_core->EnableStretching(Settings::values.enable_audio_stretching.GetValue());

        auto hid = Service::HID::GetModule(*this);
        if (hid) {
            hid->ReloadInputDevices();
        }

        auto apt = Service::APT::GetModule(*this);
        if (apt) {
            apt->GetAppletManager()->ReloadInputDevices();
        }

        auto ir_user = service_manager->GetService<Service::IR::IR_USER>("ir:USER");
        if (ir_user)
            ir_user->ReloadInputDevices();
        auto ir_rst = service_manager->GetService<Service::IR::IR_RST>("ir:rst");
        if (ir_rst)
            ir_rst->ReloadInputDevices();

        auto cam = Service::CAM::GetModule(*this);
        if (cam) {
            cam->ReloadCameraDevices();
        }

        Service::MIC::ReloadMic(*this);
    }

    auto plg_ldr = Service::PLGLDR::GetService(*this);
    if (plg_ldr) {
        plg_ldr->SetEnabled(Settings::values.plugin_loader_enabled.GetValue());
        plg_ldr->SetAllowGameChangeState(Settings::values.allow_plugin_loader.GetValue());
    }
}

void System::RegisterAppLoaderEarly(std::unique_ptr<Loader::AppLoader>& loader) {
    early_app_loader = std::move(loader);
}

void System::InsertCartridge(const std::string& path) {
    FileSys::NCCHContainer cartridge_container(path);
    if (cartridge_container.LoadHeader() == Loader::ResultStatus::Success &&
        cartridge_container.IsNCSD()) {
        inserted_cartridge = path;
    }
}

void System::EjectCartridge() {
    inserted_cartridge.clear();
}

bool System::IsInitialSetup() {
    return app_loader && app_loader->DoingInitialSetup();
}

void System::DebugUnscheduleAllThreadsFromFrontend(bool unschedule) {
    if (!is_powered_on)
        return;

    for (auto proc : kernel->GetProcessList()) {
        if (unschedule) {
            proc->SetUnscheduleMode(Kernel::UnscheduleMode::FRONTEND);
        } else {
            proc->ClearUnscheduleMode(Kernel::UnscheduleMode::FRONTEND);
        }
    }
}

template <class Archive>
void System::serialize(Archive& ar, const unsigned int file_version) {

    if (Archive::is_loading::value) {
        save_state_status = SaveStateStatus::LOADING;
    } else {
        save_state_status = SaveStateStatus::SAVING;
    }

    u32 num_cores;
    if (Archive::is_saving::value) {
        num_cores = this->GetNumCores();
    }
    ar & num_cores;

    // TODO(PabloMK7): Figure out why this is the case
    if (!lle_modules.empty()) {
        throw std::runtime_error("Savestates are not supported with LLE modules enabled");
    }

    ar & lle_modules;
    Kernel::MemoryMode mem_mode{};
    if (!Archive::is_loading::value) {
        mem_mode = kernel->GetMemoryMode();
    }
    ar & mem_mode;

    if (Archive::is_loading::value) {
        // When loading, we want to make sure any lingering state gets cleared out before we begin.
        // Shutdown, but persist a few things between loads...
        SavestateTrace("shutting down for the load");
        Shutdown(true);
        // The console the state was saved on, whatever the title was loaded as.
        new_3ds =
            mem_mode == Kernel::MemoryMode::NewProd || mem_mode == Kernel::MemoryMode::NewDev1;
        SavestateTrace("re-initialising");
        ReportStateProgress(StatePhase::Restarting, 0.0f);

        [[maybe_unused]] const System::ResultStatus result =
            Init(*m_emu_window, m_secondary_window, mem_mode, num_cores);
        SavestateTrace("re-initialised");
        ReportStateProgress(StatePhase::Restarting, 1.0f);
    }

    // Flush on save, don't flush on load - but only where the cache knows what the guest has
    // written since. With page marking off (the render thread runs; the native CPU traps no
    // stores) a dirty region can be a render target the guest has long since reused for its
    // own data, and writing those pixels back over it crashes a game the instant after "Save
    // completed" (a null pointer read out of a written-back surface). The state
    // then holds what guest memory holds; GPU-only content is redrawn or reloaded after a
    // load, as it is after any invalidation.
    const bool should_flush = !Archive::is_loading::value && memory->RasterizerCacheMarkingEnabled();
    // The render thread done with the GPU work queued, and held: guest memory and the PICA are
    // the state's alone until it is saved or loaded.
    const auto render_hold = gpu->HoldRender();
    gpu->ClearAll(should_flush);
    SavestateTrace("cleared");
    ar&* timing.get();
    SavestateTrace("timing");
    for (u32 i = 0; i < num_cores; i++) {
        ar&* cpu_cores[i].get();
    }
    ar&* service_manager.get();
    SavestateTrace("services");
    ar&* archive_manager.get();
    SavestateTrace("archives");

    // NOTE: DSP doesn't like being destroyed and recreated. So instead we do an inline
    // serialization; this means that the DSP Settings need to match for loading to work.
    auto dsp_hle = dynamic_cast<AudioCore::DspHle*>(dsp_core.get());
    if (dsp_hle) {
        ar&* dsp_hle;
    } else {
        throw std::runtime_error("LLE audio not supported for save states");
    }

    ar&* memory.get();
    SavestateTrace("memory");
    ar&* kernel.get();
    SavestateTrace("kernel");
    ar&* gpu.get();
    SavestateTrace("gpu");
    ar & movie;
    SavestateTrace("movie");

    // This needs to be set from somewhere - might as well be here!
    if (Archive::is_loading::value) {
        u32 cheats_pid;
        ar & cheats_pid;
        // A state saved with the engine connected to no process (cheats were off on the Vita
        // build) names none: the running title's, as the boot connects it.
        if (!kernel->GetProcessById(cheats_pid) && kernel->GetCurrentProcess()) {
            cheats_pid = kernel->GetCurrentProcess()->process_id;
        }
        timing->UnlockEventQueue();
        cheat_engine.Connect(cheats_pid);

        // Re-register gpu callback, because gsp service changed after service_manager got
        // serialized
        auto gsp = service_manager->GetService<Service::GSP::GSP_GPU>("gsp::Gpu");
        gpu->SetInterruptHandler([gsp](Service::GSP::InterruptId interrupt_id, u64 wait_delay_ns) {
            gsp->SignalInterrupt(interrupt_id, wait_delay_ns);
        });

        // Apply per program settings and switch the shader cache to the title running when the
        // savestate was created.
        // TODO(PabloMK7): Find better way to obtain the program ID.
        const u32 thread_id = gsp->GetActiveClientThreadId();
        if (thread_id != std::numeric_limits<u32>::max()) {
            const auto thread = kernel->GetThreadByID(thread_id);
            if (thread) {
                const std::shared_ptr<Kernel::Process> process = thread->owner_process.lock();
                if (process) {
                    gpu->ApplyPerProgramSettings(process->codeset->program_id);
                    gpu->SwitchDiskResources(process->codeset->program_id);
                }
            }
        }
    } else {
        u32 cheats_pid = cheat_engine.GetConnectedPID();
        ar & cheats_pid;
    }

    save_state_status = SaveStateStatus::NONE;
}

SERIALIZE_IMPL(System)

} // namespace Core

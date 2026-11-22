// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <numeric>
#include <string>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include <malloc.h>
#include <pthread.h>

#include <fmt/format.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/power.h>
#include <psp2/shellutil.h>
#include <psp2/vshbridge.h>
#include <taihen.h>
#include "citra_vita/crash_report.h"
#include "citra_vita/gxm_present.h"
#include "citra_vita/sgxd_present.h"

#include "citra_vita/emu_window_vita.h"
#include "citra_vita/input_factory_vita.h"

#include "azahar_native.h"
#include "common/heap_check.h"
#include "common/named_thread.h"
#include "core/arm/vita/arm_vita_native.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "sgxd_ptla.h"
#include "video_core/renderer_sgxd/vprof.h"

#include <cstdio>

#include "audio_core/hle/hle.h"
#include "citra_vita/hud_stats.h"
#include "citra_vita/vita_ui.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/hle/service/service.h"
#include "core/movie.h"
#include "video_core/gpu.h"
#include "video_core/renderer_gxm/gxm_flags.h"
#include "video_core/renderer_gxm/render_thread.h"
#include "video_core/renderer_gxm/renderer_gxm.h"

// The default heap is nowhere near enough for a 3DS: FCRAM alone is 128 MB, and the caches and
// the surface storage sit on top of it. Newlib reads this at startup.
extern "C" {
// The newlib heap: guest VRAM and the emulator's working set live in it, guest FCRAM in a block of
// its own (HostSharedMemory::OwnBlock). newlib takes the whole heap as one memblock at startup and
// the kernel commits it, so every megabyte here is one the system cannot have (at 300 MB its own
// services failed to start).
unsigned int _newlib_heap_size_user = 112 * 1024 * 1024;

// Default stack for every pthread, and so for every std::thread: the GPU thread, the two
// rasterizer workers, the service threads. vitasdk's built-in default is 32 KB (the fallback
// constant in libpthread's create.o), which the renderer and the savestate serializer overrun.
// std::thread offers no way to ask for a bigger stack, so this weak global is the only lever;
// the emulation thread sets its own, larger stack through pthread_attr_setstacksize in main().
unsigned int _pthread_stack_default_user = 1024 * 1024;

// newlib's lazy network bring-up (libc/sys/vita/vitanet.c). Not declared in any header, but a
// global symbol, and guarded by its own atomic once-flag, so calling it twice costs nothing.
int _vita_net_init(void);
}

// 5: AZAHAR_PERM_APP_ATTR, guest VRAM in CDRAM mapped with the app's attributes.
constexpr int MinAzaharNativeVersion = 5;

namespace {
/// The process's own arguments, kept for LaunchParameter.
std::vector<std::string> g_arguments;

/// Where the vpk puts the kernel module. The kernel opens the file itself, and app0: is this
/// process's mount rather than a path the kernel resolves, so this is the installed copy.
constexpr char AzaharNativePath[] = "ux0:app/" AZAHAR_VITA_TITLEID "/azaharnative.skprx";
/// Marks the one restart that loading the module costs; see LoadAzaharNative.
constexpr char RelaunchArgument[] = "--azaharnative-loaded";
/// What LoadAzaharNative returned: empty, or the error EmulatorMain shows once it can.
std::string g_native_error;

constexpr char UserDirectory[] = "ux0:/data/azahar/";

void SetupPaths() {
    // Must happen before anything asks for a path: the first request bootstraps the whole table
    // from whatever the platform default is, and on this console there is no such thing.
    FileUtil::SetUserPath(UserDirectory);
}

void MigrateNandLayout() {
    // The NAND's system save data and shared extdata moved up from data/<ID0>/
    // (Service::FS::GetNANDDataRoot, the ten-level limit): data kept there before comes along.
    for (const char* dir : {"sysdata", "extdata"}) {
        const std::string nand = FileUtil::GetUserPath(FileUtil::UserPath::NANDDir);
        const std::string from =
            fmt::format("{}data/00000000000000000000000000000000/{}", nand, dir);
        const std::string to = nand + dir;
        SceIoStat st{};
        if (sceIoGetstat(from.c_str(), &st) >= 0 && sceIoGetstat(to.c_str(), &st) < 0) {
            const int moved = sceIoRename(from.c_str(), to.c_str());
            LOG_INFO(Frontend, "{} moved to {}: {:#010x}", from, to, static_cast<u32>(moved));
        }
    }
}

/// Names the exception behind an abort, in every build: std::terminate goes to newlib's abort(),
/// whose crash dump points at _kill_r, and libstdc++'s handler writes to stderr, which no console
/// shows. std::bad_alloc and a failing pthread call's std::system_error arrive here. The return
/// addresses come from the stack as the throw left it, and the process aborts rather than exits, so
/// that a core file can be taken of it.
void SetupTerminateHandler() {
    std::set_terminate([] {
        const char* type = "";
        const char* what = "no active exception";
        if (std::current_exception()) {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                type = typeid(e).name();
                what = e.what();
            } catch (...) {
                what = "an exception not derived from std::exception";
            }
        }
        // Nothing here allocates: the exception may be std::bad_alloc.
        char reason[320];
        std::snprintf(reason, sizeof(reason), "std::terminate: %s %s", type, what);
        VitaFrontend::CrashReport::WriteStackReport(reason);
        Common::Log::Stop();
        std::abort();
    });
}

/// Brings the network stack up now rather than at the first guest socket call: newlib's lazy
/// _vita_net_init loads SCE_SYSMODULE_NET and about ten system modules with it, in the middle of a
/// title. Here it happens once, before the guest exists.
void SetupNetwork() {
    const int ret = _vita_net_init();
    if (ret < 0) {
        // Not fatal: a console with no network configured still emulates fine, and the guest's
        // network services will fail the same way they would have anyway.
        LOG_WARNING(Frontend, "network stack unavailable ({:#010x}); guest sockets will fail",
                    static_cast<u32>(ret));
    }
}

void SetupLogging() {
#ifdef VITA_DIAGNOSTICS
    Common::Log::Initialize();
    Common::Log::Filter filter;
    // Debug.Emulated: the guest's own svcOutputDebugString lines (homebrew tests such as
    // src/native_mc_test/litmus report through it; retail titles rarely call it).
    filter.ParseFilterString("*:Info Debug.Emulated:Debug");
    Common::Log::SetGlobalFilter(filter);
#endif
    // The release build starts no logging backend at all, so every LOG_* macro reaches a
    // filter that discards it before it formats anything.
}

/// The Vita boots at a conservative clock; an emulated 3DS needs every megahertz. The GPU's
/// clock is set before sgxd takes the GPU and never changes after (docs/sgxd/takeover.md).
void SetupClocks() {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
}

std::string DescribeLoadError(Core::System::ResultStatus status) {
    switch (status) {
    case Core::System::ResultStatus::ErrorGetLoader:
        return "no loader for this file";
    case Core::System::ResultStatus::ErrorSystemMode:
        return "could not read the system mode";
    case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
        return "the title is encrypted";
    case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
        return "unsupported file format";
    case Core::System::ResultStatus::ErrorLoader_ErrorGbaTitle:
        return "GBA titles are not supported";
    case Core::System::ResultStatus::ErrorNotInitialized:
        return "the CPU could not start: azaharnative could not take a core";
    default:
        return "unknown error";
    }
}

/// Releases the GPU's view of guest memory and shuts the system down - in that order, because
/// the unmap must run while guest RAM is still allocated.
void ShutdownSystem(Core::System& system) {
    VitaFrontend::GxmPresent::UnmapAllGuestMemory();
    system.Shutdown();
    // The title's GPU session is over: nothing of it is left for the next one (the game list's
    // banner) to wait on.
    VitaFrontend::SgxdPresent::SessionEnded();
}

#ifdef VITA_DIAGNOSTICS
/// `perflog`'s counters of the render side: the render thread's, sgxd's waits and cache
/// operations, the presents' parts and the page moves. Like every counter behind perflog, the
/// diagnostics build's only.
struct RenderCounters {
    SceUInt64 us; ///< the time since the take before
    SgxdRenderer::RenderThread::Stats thread;
    u64 gpu_wait_us;
    VitaFrontend::PresentTimes present;
    SgxdRenderer::SgxdCacheTimes cache;
    SgxdRenderer::GuestGpuMemory::MoveStats moves;
};

/// The counters the render thread took last, and the request's state: 0 none asked, 1 asked,
/// 2 taken.
RenderCounters render_counters_taken{};
std::atomic<u32> render_counters_state{};

/// Taken on the render thread when it reaches the request, after the work queued before it: sgxd's
/// and the window's counters are plain, written there. perflog does not wait for it: a RunSync
/// would pace the guest as release builds are not. Whether there were counters taken since the
/// last call, in `out`; and the next take asked for.
bool TakeRenderCounters(Core::System& system, RenderCounters& out) {
    u32 state = render_counters_state.load(std::memory_order_acquire);
    const bool taken = state == 2;
    if (taken) {
        out = render_counters_taken;
        state = 0;
    }
    if (state == 0) {
        render_counters_state.store(1, std::memory_order_relaxed);
        // The request runs on the render thread, which may reach it while GPU::~GPU stops the
        // thread (a quit): the GPU's pointer to it is gone by then, the thread is not.
        auto& gpu = system.GPU();
        auto& render = gpu.GetRenderThread();
        render.Run([&gpu, &render] {
            static SceUInt64 last_us = 0;
            const SceUInt64 now_us = sceKernelGetProcessTimeWide();
            RenderCounters& c = render_counters_taken;
            c.us = last_us != 0 ? now_us - last_us : 0;
            last_us = now_us;
            c.thread = render.TakeStats();
            c.gpu_wait_us = sgxd_wait_us_take();
            c.present = VitaFrontend::TakePresentTimes();
            c.cache = SgxdRenderer::SgxdTakeCacheTimes();
            if (auto* gxm = dynamic_cast<GxmRenderer::RendererGxm*>(&gpu.Renderer())) {
                c.moves = gxm->TakeMoveStats();
            }
            render_counters_state.store(2, std::memory_order_release);
        });
    }
    return taken;
}
#endif

/// Runs one title to completion. Returns false when the user wants the application to close
/// rather than to return to the game list.
/**
 */
struct StateArguments {
    int load = -1;
    int save = -1;
    SceUInt64 save_after_us = 0;
    /// `commands`: host0:azahar-commands.txt is read once a second (CommandFile).
    bool commands = false;
};

StateArguments TakeStateArguments() {
    static bool taken = false;
    StateArguments a;
    if (taken) {
        return a;
    }
    taken = true;
    for (std::size_t i = 1; i < g_arguments.size(); i++) {
        const std::string& argument = g_arguments[i];
        if (argument.starts_with("load=")) {
            a.load = std::atoi(argument.c_str() + 5);
        } else if (argument == "commands") {
            a.commands = true;
        } else if (argument.starts_with("scale=")) {
            // The top screen's scale: 1, 1.25 or 1.5.
            SgxdRenderer::RasterizerSgxd::SetTopScale(
                static_cast<u32>(std::lround(std::atof(argument.c_str() + 6) * 4)));
        } else if (argument == "layout=overlay") {
            Settings::values.layout_option = Settings::LayoutOption::SingleScreen;
            VitaFrontend::EmuWindowVita::SetOverlay(true);
        } else if (argument.starts_with("layout=")) {
            Settings::values.layout_option =
                static_cast<Settings::LayoutOption>(std::atoi(argument.c_str() + 7));
        } else if (argument == "wide") {
            VitaFrontend::EmuWindowVita::SetWidescreen(true);
        } else if (argument.starts_with("opacity=")) {
            // The overlay layout's other screen, in percent (10 to 100).
            VitaFrontend::EmuWindowVita::SetOverlayOpacity(
                static_cast<u32>(std::clamp(std::atoi(argument.c_str() + 8), 10, 100)));
        } else if (argument.starts_with("vsync=")) {
            Settings::values.use_vsync = std::atoi(argument.c_str() + 6) != 0;
        } else if (argument == "sched=0") {
            // The module's in-kernel scheduling off: every SleepThread exits to the HLE kernel.
            Core::SetVitaInKernelScheduling(false);
        } else if (argument.starts_with("save=")) {
            a.save = std::atoi(argument.c_str() + 5);
            if (const std::size_t at = argument.find('@'); at != std::string::npos) {
                a.save_after_us = std::strtoull(argument.c_str() + at + 1, nullptr, 10) * 1000000;
            }
        } else {
            sceClibPrintf("[azahar] launch argument %s not known\n", argument.c_str());
        }
    }
    return a;
}

/**
 * The command channel (the `commands` launch argument): host0:azahar-commands.txt,
 */
class CommandFile {
public:
    std::string Poll() {
        const SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now - last_read_us < 1000000) {
            return {};
        }
        last_read_us = now;
        FILE* f = std::fopen("host0:azahar-commands.txt", "rb");
        if (f == nullptr) {
            return {};
        }
        char line[128];
        std::string due;
        while (due.empty() && std::fgets(line, sizeof(line), f) != nullptr) {
            char* rest = nullptr;
            const unsigned long seq = std::strtoul(line, &rest, 10);
            if (rest == line || seq <= last_seq) {
                continue;
            }
            last_seq = seq;
            due = rest;
            due.erase(0, due.find_first_not_of(' '));
            due.erase(due.find_last_not_of(" \r\n") + 1);
        }
        std::fclose(f);
        return due;
    }

private:
    SceUInt64 last_read_us = 0;
    unsigned long last_seq = 0;
};

/**
 * Restores a savestate slot before the title runs. The signal executes at a quiescent point
 * inside RunLoop, so it gets a few slices. A state from another build is taken: the archive
 * layout is what has to match, not the revision (a scene saved once is loaded into the builds
 * after it).
 */
void RestoreStateAtStart(Core::System& system, u32 slot) {
    system.SetLoadStateAnyBuild(true);
    if (!system.SendSignal(Core::System::Signal::Load, slot)) {
        sceClibPrintf("[azahar] savestate: could not queue a load of slot %u\n", slot);
        return;
    }
    sceClibPrintf("[azahar] savestate: restoring slot %u\n", slot);
    // The load runs inside RunLoop once the kernel has no asynchronous HLE request in flight; at
    // this point in the boot that is immediate. Requests run inline while the load is pending so a
    // title that streams from its ROM cannot keep one in flight, and a failed load is reported
    // rather than played over.
    const bool async_before = Settings::values.deterministic_async_operations.GetValue();
    Settings::values.deterministic_async_operations = true;
    // The signal becomes a pending request inside the first RunLoop, so the loop always runs
    // at least once.
    for (int i = 0; i < 200000; i++) {
        const auto status = system.RunLoop();
        if (status != Core::System::ResultStatus::Success) {
            sceClibPrintf("[azahar] savestate: load of slot %u failed: %d (%s)\n", slot,
                          static_cast<int>(status), system.GetStatusDetails().c_str());
            break;
        }
        if (!system.IsSaveStateRequestPending()) {
            sceClibPrintf("[azahar] savestate: slot %u restored\n", slot);
            VitaFrontend::CrashReport::NoteStateLoaded(slot);
            break;
        }
    }
    Settings::values.deterministic_async_operations = async_before;
    if (system.IsSaveStateRequestPending()) {
        sceClibPrintf("[azahar] savestate: slot %u never became loadable\n", slot);
    }
}

/// What copy runs, for a crash report: the name, the product code and version (an update's or the
/// cartridge's), the file's type and size, and its 3GX plugin. At a title's start and after a jump
/// to another title, which the core makes alone.
void NoteRunningTitle(Core::System& system) {
    const std::string& path = system.GetFilePath();
    auto& loader = system.GetAppLoader();
    u64 program_id = 0;
    loader.ReadProgramId(program_id);
    std::string name;
    loader.ReadTitle(name);
    VitaFrontend::CrashReport::NoteGame(
        path, program_id,
        fmt::format("\"{}\", {}, {}, {} MiB", name, loader.ProductSummary(),
                    Loader::GetFileTypeString(loader.GetFileType(), loader.IsFileCompressed()),
                    FileUtil::GetSize(path) >> 20));
    std::string plugin;
    if (const auto plg_ldr = Service::PLGLDR::GetService(system)) {
        const auto& context = plg_ldr->GetPluginLoaderContext();
        if (context.plugin_loaded) {
            plugin = fmt::format("\"{}\" by {}, {}", context.plugin_title, context.plugin_author,
                                 context.plugin_path);
        }
    }
    VitaFrontend::CrashReport::NotePlugin(plugin);
}

bool RunTitle(Core::System& system, VitaFrontend::EmuWindowVita& window, VitaFrontend::Ui& ui,
              const std::string& path) {
    window.UpdateCurrentFramebufferLayout();

    ui.SetStatusMessage("Loading...");
    ui.Update();
    window.Present(&ui);

    window.SetLoading(true);
    const auto load_result = system.Load(window, path);
    window.SetLoading(false);
    if (load_result != Core::System::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Could not load {}: {}", path, DescribeLoadError(load_result));
        ui.SetStatusMessage("Could not load this title: " + DescribeLoadError(load_result));
        for (int i = 0; i < 240; i++) {
            window.PollEvents();
            ui.Update();
            window.Present(&ui);
            sceDisplayWaitVblankStart();
        }
        ui.ClearStatusMessage();
        return true;
    }
    ui.ClearStatusMessage();
    StateArguments state_arguments = TakeStateArguments();
    window.UpdateCurrentFramebufferLayout(); // a layout=N argument's

#ifdef VITA_DIAGNOSTICS
    SgxdRenderer::Vprof::Info("rom", path.c_str());
    if (const auto& process = system.Kernel().GetCurrentProcess(); process && process->codeset) {
        SgxdRenderer::Vprof::Info("guest_name", process->codeset->GetName().c_str());
    }

    // ux0:data/azahar/autoload.txt names a savestate slot to restore before the title runs,
    // when the launch arguments name none.
    if (state_arguments.load < 0) {
        if (FILE* f = std::fopen("ux0:data/azahar/autoload.txt", "rb")) {
            char buf[16]{};
            std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            state_arguments.load = std::atoi(buf);
        }
    }
#endif
    if (state_arguments.load >= 0) {
        window.SetLoading(true);
        RestoreStateAtStart(system, static_cast<u32>(state_arguments.load));
        window.SetLoading(false);
        if (system.IsStateLoadPartial()) {
            sceClibPrintf("[azahar] savestate: the load stopped partway; the title is stopped\n");
            window.RequestQuit(false);
        }
    }
    // `save=N@T`: once the title has run T seconds (real time, from here).
    const SceUInt64 title_start_us = sceKernelGetProcessTimeWide();
    bool state_save_sent = false;
    // A save or load sent (the argument's or the menu's) whose result is still to print.
    bool state_watch = false;
    bool state_watch_is_load = false;
    CommandFile commands;

    ui.EnterGame();
    window.SetUi(&ui);

    // The pause menu is the render thread's: it polls the pad, opens the menu on the PS button
    // and runs it, while this loop waits at the top. What only this thread can do (savestates,
    // quitting) the menu leaves here.
    window.SetPauseMenu(&ui);
    while (!window.IsQuitRequested()) {
        window.WaitWhilePaused();
        const auto action = static_cast<VitaFrontend::Ui::Action>(window.TakeUiAction());
        switch (action) {
        case VitaFrontend::Ui::Action::QuitGame:
        case VitaFrontend::Ui::Action::ExitApplication:
            window.SetPauseMenu(nullptr);
            window.SetInputToGuest(true);
            window.SetLoading(false);
            window.SetUi(nullptr);
            ShutdownSystem(system);
            return action == VitaFrontend::Ui::Action::QuitGame;
        case VitaFrontend::Ui::Action::SaveState:
            state_watch_is_load = false;
            state_watch = system.SendSignal(Core::System::Signal::Save, ui.SelectedSlot());
            sceClibPrintf("[azahar] savestate: saving slot %u from the menu\n", ui.SelectedSlot());
            break;
        case VitaFrontend::Ui::Action::LoadState:
            state_watch_is_load = true;
            state_watch = system.SendSignal(Core::System::Signal::Load, ui.SelectedSlot());
            sceClibPrintf("[azahar] savestate: loading slot %u from the menu\n", ui.SelectedSlot());
            break;
        default:
            break;
        }

        if (state_arguments.save >= 0 && !state_save_sent &&
            sceKernelGetProcessTimeWide() - title_start_us >= state_arguments.save_after_us) {
            state_save_sent = true;
            state_watch_is_load = false;
            state_watch = system.SendSignal(Core::System::Signal::Save,
                                            static_cast<u32>(state_arguments.save));
            sceClibPrintf("[azahar] savestate: saving slot %d %s\n", state_arguments.save,
                          state_watch ? "queued" : "refused");
        }
        if (state_arguments.commands) {
            if (const std::string command = commands.Poll(); !command.empty()) {
                int slot = -1;
                const bool save = std::sscanf(command.c_str(), "save %d", &slot) == 1;
                const bool load = !save && std::sscanf(command.c_str(), "load %d", &slot) == 1;
                if ((save || load) && slot >= 0) {
                    system.SetLoadStateAnyBuild(true);
                    state_watch_is_load = load;
                    state_watch = system.SendSignal(save ? Core::System::Signal::Save
                                                         : Core::System::Signal::Load,
                                                    static_cast<u32>(slot));
                    sceClibPrintf("[azahar] savestate: %s slot %d (command) %s\n",
                                  save ? "saving" : "loading", slot,
                                  state_watch ? "queued" : "refused");
                } else {
                    sceClibPrintf("[azahar] command \"%s\" not known\n", command.c_str());
                }
            }
        }
        // A save or load sent and not done yet: the volume keys do nothing meanwhile.
        window.SetLoading(state_watch);
        const auto status = system.RunLoop();
        // The signal becomes a request inside RunLoop, which runs it once no HLE request is
        // in flight: the same call or a later one.
        if (state_watch && !system.IsSaveStateRequestPending()) {
            state_watch = false;
            window.SetLoading(false);
            const bool ok = status == Core::System::ResultStatus::Success;
            sceClibPrintf("[azahar] savestate: %s%s\n",
                          ok ? "done" : "failed: ", ok ? "" : system.GetStatusDetails().c_str());
            if (ok && state_watch_is_load) {
                VitaFrontend::CrashReport::NoteStateLoaded(progress_slot);
            }
        }
        if (status == Core::System::ResultStatus::ErrorSavestate && system.IsStateLoadPartial()) {
            // A load that failed partway through the file left a state the title cannot run on
            // (part the old one, part the file's): back to the game list.
            sceClibPrintf("[azahar] savestate: the load stopped partway; the title is stopped\n");
            LOG_CRITICAL(Frontend, "Savestate load stopped partway: {}", system.GetStatusDetails());
            window.RequestQuit(false);
        } else if (status == Core::System::ResultStatus::ErrorSavestate) {
            // A refused save or load is not fatal; the title keeps running.
            LOG_ERROR(Frontend, "Savestate operation failed");
        } else if (status != Core::System::ResultStatus::Success) {
            LOG_CRITICAL(Frontend, "Emulation stopped: {}", static_cast<int>(status));
            break;
        }

        {
            static SceUInt64 last_perf_us = 0;
            const SceUInt64 now_us = sceKernelGetProcessTimeWide();
            if (now_us - last_perf_us >= 1000000ull) {
#ifdef VITA_DIAGNOSTICS
                const SceUInt64 interval_us = now_us - last_perf_us;
#endif
                last_perf_us = now_us;
                const auto perf = system.GetAndResetPerfStats();
                using namespace VitaFrontend::HudStats;
                speed_percent.store(
                    static_cast<u32>(std::max(perf.emulation_speed * 100.0 + 0.5, 0.0)),
                    std::memory_order_relaxed);
                game_fps10.store(static_cast<u32>(std::max(perf.game_fps * 10.0 + 0.5, 0.0)),
                                 std::memory_order_relaxed);
                shown_fps.store(static_cast<u32>(std::max(perf.system_fps + 0.5, 0.0)),
                                std::memory_order_relaxed);
                valid.store(true, std::memory_order_release);
                // Presented with the guest's screens unchanged too (GPU's vblank).
                window.MarkOverlayChanged();
#ifdef VITA_DIAGNOSTICS
                static const bool perflog = SgxdRenderer::SgxdFlag("perflog");
                if (perflog) {
                    // The render thread's counters (sgxd's, the presents', the page moves'):
                    // the last ones it took, over their own interval (zero when it has not
                    // reached the last request).
                    RenderCounters render_counters{};
                    const bool render_taken = TakeRenderCounters(system, render_counters);
                    // The vblank interval split as core/perf_stats.h measures it, in ms.
                    LOG_INFO(Frontend,
                             "perf: speed {:.1f}% game {:.1f} fps shown {:.1f} fps; vblank "
                             "{:.2f} ms: svc {:.2f} ipc {:.2f} gpu {:.2f} swap {:.2f} rest {:.2f}",
                             perf.emulation_speed * 100.0, perf.game_fps, perf.system_fps,
                             perf.time_vblank_interval * 1e3, perf.time_hle_svc * 1e3,
                             perf.time_hle_ipc * 1e3, perf.time_gpu * 1e3, perf.time_swap * 1e3,
                             perf.time_remaining * 1e3);
                    // The native core's slices: time in the guest and in azaharRun (ms), how
                    // the slices ended, the svcs answered in the guest, and the svcs that left
                    // it, most frequent first.
                    const auto n = Core::TakeVitaNativeCensus();
                    // The store permits made by what their slot held before, and the sites of
                    // stores the module left to the app on a live permit (pc:instruction count).
                    std::string live_sites;
                    for (const auto& site : n.permits_live_sites) {
                        if (site[2] != 0) {
                            fmt::format_to(std::back_inserter(live_sites), " {:08x}:{:08x} {}",
                                           site[0], site[1], site[2]);
                        }
                    }
                    auto store_pages = n.permitted_store_pages;
                    std::sort(store_pages.begin(), store_pages.end(),
                              [](const auto& a, const auto& b) { return a[1] > b[1]; });
                    for (const auto& page : store_pages) {
                        if (page[1] != 0) {
                            fmt::format_to(std::back_inserter(live_sites), " page {:#010x} {}",
                                           page[0] << 12, page[1]);
                        }
                    }
                    LOG_INFO(Frontend,
                             "perf: store permits made after none {}, another page's {}, the "
                             "page's used up {}, the page's live {}; all cleared {} times;{}",
                             n.permits_after_none, n.permits_after_other, n.permits_after_used,
                             n.permits_after_live, n.permit_clears, live_sites);
                    const double tick_ms = BASE_CLOCK_RATE_ARM11 / 1e3;
                    LOG_INFO(
                        Frontend,
                        "perf: guest {:.1f} ms, azaharRun {:.1f} ms, {} slices: svc {}, "
                        "timer {}, faults {} (VFP short vectors {} in {}, loads past held "
                        "bytes {}, stores {} (permits {}, {} stores made by them in the "
                        "vector), {} past the budget left to wait; sites patched to "
                        "a thunk {}, to the svc {}); svc sites patched to a thunk {}; "
                        "upcall sites {} (Thumb {}) runs {} redirected {} rescheduled {} "
                        "switched {} idled {} ({} events, {} exits, {} for core 1, {} for the "
                        "vblank), exits for the slice's end {} and no next thread {} "
                        "syscalls {} "
                        "preempted {} ({} in the vector), app megabytes taken {}, upcall "
                        "stack {} bytes, the app's level-1 entries the guest's table lacks "
                        "{} and holds stale {}; svcs "
                        "answered in the guest: "
                        "tick {}, cache {}, thread id {}, process id {}, VFP {}",
                        n.guest_ticks / tick_ms,
                        n.kernel_cycles / (scePowerGetArmClockFrequency() * 1e3), n.slices,
                        n.reasons[AZAHAR_EXIT_SVC], n.reasons[AZAHAR_EXIT_TIMER],
                        n.slices - n.reasons[AZAHAR_EXIT_SVC] - n.reasons[AZAHAR_EXIT_TIMER],
                        n.vfp_insns, n.vfp_exits, n.hold_loads, n.hold_stores, n.hold_thumb,
                        n.store_permits, n.permitted_stores, n.hold_load_waits, n.vfp_thunks, n.vfp_svcs,
                        n.svc_thunks,
                        n.upcall_sites, n.upcall_thumb_sites, n.upcall_runs, n.upcall_redirects,
                        n.upcall_reschedules, n.upcall_switches, n.upcall_idles,
                        n.upcall_idle_events, n.upcall_idle_exits, n.upcall_idle_exits_other_busy,
                        n.upcall_idle_exits_vblank, n.upcall_exits_slice_up,
                        n.upcall_exits_no_next, n.upcall_syscalls, n.upcall_preempted,
                        n.upcall_rearms, n.upcall_synced, n.upcall_stack_used,
                        n.upcall_drift < 0 ? n.upcall_drift : n.upcall_drift & 0xFFFF,
                        n.upcall_drift < 0 ? 0 : n.upcall_drift >> 16, n.fast[AZAHAR_FAST_TICK],
                        n.fast[AZAHAR_FAST_CACHE], n.fast[AZAHAR_FAST_THREAD_ID],
                        n.fast[AZAHAR_FAST_PROCESS_ID], n.fast[AZAHAR_FAST_VFP]);
                    LOG_INFO(Frontend,
                             "perf: idle loops: {} callers sent to the skip; {} skips, {:.1f} ms "
                             "of guest time skipped",
                             n.idle_waits, n.idle_skips, n.idle_skip_ticks / tick_ms);
                    LOG_INFO(Frontend,
                             "perf: idle exits to RunLoop: host 1 running {}, its next event "
                             "soon {}, a message for it {}, one for host 0 {}, a forward {}, "
                             "quiet {}",
                             n.upcall_idle_exit_why[0], n.upcall_idle_exit_why[1],
                             n.upcall_idle_exit_why[2], n.upcall_idle_exit_why[3],
                             n.upcall_idle_exit_why[4], n.upcall_idle_exit_why[5]);
                    std::string sleeps;
                    for (const auto& s : n.sleeps) {
                        if (s.count != 0) {
                            fmt::format_to(std::back_inserter(sleeps), " {:08x} {} ({:.1f} us)",
                                           s.lr, s.count, s.ns / 1e3 / s.count);
                        }
                    }
                    LOG_INFO(Frontend, "perf: SleepThread by caller:{}; other {}, 0 ns {}", sleeps,
                             n.sleep_other, n.sleep_zero);
                    std::string hold_top;
                    for (const auto& [page, count, pc] : n.hold_top) {
                        if (count != 0) {
                            hold_top += fmt::format(" {:#010x} {} (pc {:#010x})", page, count, pc);
                        }
                    }
                    LOG_INFO(Frontend, "perf: accesses past holds: {} pages;{}", n.hold_pages,
                             hold_top);
                    std::vector<std::pair<u32, u32>> upcall_svcs;
                    for (u32 s = 0; s < n.upcall_svcs.size(); s++) {
                        if (n.upcall_svcs[s] != 0) {
                            upcall_svcs.emplace_back(n.upcall_svcs[s], s);
                        }
                    }
                    std::sort(upcall_svcs.rbegin(), upcall_svcs.rend());
                    std::string upcall_top;
                    for (std::size_t i = 0; i < std::min<std::size_t>(upcall_svcs.size(), 10); i++) {
                        upcall_top += fmt::format(" {:#04x} {}", upcall_svcs[i].second,
                                                  upcall_svcs[i].first);
                    }
                    LOG_INFO(Frontend, "perf: upcalls by svc:{}", upcall_top);
                    std::vector<u32> order;
                    for (u32 i = 0; i < n.svcs.size(); i++) {
                        if (n.svcs[i] != 0) {
                            order.push_back(i);
                        }
                    }
                    std::sort(order.begin(), order.end(),
                              [&n](u32 a, u32 b) { return n.svcs[a] > n.svcs[b]; });
                    std::string svcs;
                    for (const u32 i : order) {
                        svcs += fmt::format("{}{:02x} {}", svcs.empty() ? "" : ", ", i, n.svcs[i]);
                    }
                    LOG_INFO(Frontend, "perf: svc exits: {}", svcs.empty() ? "none" : svcs);
                    std::string forwards;
                    for (const auto& f : n.forwards) {
                        if (f.count != 0) {
                            forwards += fmt::format(
                                "{}{:08x}{} #{:x} {}", forwards.empty() ? "" : ", ", f.lr,
                                f.app ? fmt::format("<{:08x}", f.app) : std::string{}, f.number,
                                f.count);
                        }
                    }
                    if (!forwards.empty()) {
                        LOG_INFO(Frontend, "perf: Sony syscalls forwarded by caller: {}; {} more",
                                 forwards, n.forward_other);
                    }
                    // Room for the lists' scan off the emulation thread (GPU::GuestIdle).
                    const auto room = system.GPU().TakeScanRoom();
                    LOG_INFO(Frontend,
                             "perf: scan room: {} lists, scan {:.1f} ms, hideable {:.1f} ms; scan "
                             "end to the guest's idle {} under 0.5 ms, {} under 2, {} under 8, {} "
                             "under 16, {} longer; {} not followed by an idle yet",
                             room.lists, room.scan_us / 1e3, room.hidden_us / 1e3, room.gaps[0],
                             room.gaps[1], room.gaps[2], room.gaps[3], room.gaps[4], room.pending);
                    if (render_taken) {
                        LOG_INFO(Frontend, "perf: render side's counters below over {:.1f} ms",
                                 render_counters.us / 1e3);
                    } else {
                        LOG_INFO(Frontend, "perf: render side's counters below none: the render "
                                           "thread has not reached the last request");
                    }
                    const auto& t = render_counters.thread;
                    using K = SgxdRenderer::RenderThread::Stats;
                    LOG_INFO(Frontend,
                             "perf: render thread busy {:.1f} ms, idle {:.1f} ms ({} GPU wakes, "
                             "{} timed out), GPU waits {:.1f} ms; records: state {}, draw {}, "
                             "fill {}, "
                             "transfer {}, dma {}, present {}, other {}; present {} ({} "
                             "dropped behind, {} too soon; gaps to 20 ms {}, 37 {}, 54 {}, "
                             "longer {}); holds {} waiting {:.1f} ms",
                             t.busy_us / 1e3, t.idle_us / 1e3, t.gpu_wakes, t.timeouts,
                             render_counters.gpu_wait_us / 1e3, t.records[K::State],
                             t.records[K::Draw], t.records[K::Fill], t.records[K::Transfer],
                             t.records[K::Dma], t.records[K::Present], t.records[K::Other],
                             t.presents, t.presents_behind, t.presents_soon, t.present_gaps[0],
                             t.present_gaps[1], t.present_gaps[2], t.present_gaps[3], t.holds,
                             t.hold_us / 1e3);
                    LOG_INFO(
                        Frontend,
                        "perf: late skip: {} frames, {} draws dropped, "
                        "{} kept for the other screen; screens presented from another buffer {}; "
                        "drawn whole with the GPU late {}, the vblank waiting {:.1f} ms",
                        t.late_frames, t.late_draws, t.late_kept, t.late_partial, t.late_waits,
                        t.late_wait_us / 1e3);
                    if (t.lead_samples != 0) {
                        LOG_INFO(Frontend,
                                 "perf: frames queued ahead of the render thread at the vblanks: "
                                 "{:.1f} mean, {} most; ring {} KiB most",
                                 static_cast<double>(t.lead_frames_sum) / t.lead_samples,
                                 t.lead_frames_max, t.lead_bytes_max / 1024);
                    }
                    if (t.gpu_samples != 0) {
                        LOG_INFO(Frontend,
                                 "perf: GPU busy {:.0f}% of {} samples, idle with a 3D held for "
                                 "the next list {:.0f}%",
                                 100.0 * t.gpu_busy / t.gpu_samples, t.gpu_samples,
                                 100.0 * t.gpu_held / t.gpu_samples);
                        // Where the microkernel's streams are: no job, a job's code, or the
                        // wait they stopped at (3D stream / TA stream).
                        using RT = SgxdRenderer::RenderThread;
                        static constexpr const char* states[] = {
                            "idle", "code", "TA", "3D", "2D", "event", "count", "TA idle", "pages"};
                        static_assert(std::size(states) == RT::GpuAtStates);
                        const auto pct = [&](u32 n) { return 100.0 * n / t.gpu_samples; };
                        std::string at;
                        for (u32 c = 0; c < RT::GpuAtCells; c++) {
                            if (t.gpu_at[c] * 200 >= t.gpu_samples) {
                                at += fmt::format("{}3D {} / TA {} {:.1f}%", at.empty() ? "" : ", ",
                                                  states[c / RT::GpuAtStates],
                                                  states[c % RT::GpuAtStates], pct(t.gpu_at[c]));
                            }
                        }
                        LOG_INFO(Frontend, "perf: GPU at: {}", at);
                        // Whose 3D and TA the samples found running, by the render job's
                        // sender, with the jobs it sent; most first.
                        const auto by_sender = [&](const std::array<u32, RT::GpuLabels>& by) {
                            std::array<u32, RT::GpuLabels> order;
                            std::iota(order.begin(), order.end(), 0u);
                            std::ranges::sort(order, [&](u32 a, u32 b) { return by[a] > by[b]; });
                            std::string out;
                            for (const u32 l : order) {
                                if (by[l] * 200 < t.gpu_samples) {
                                    break;
                                }
                                u32 key = 0;
                                const char* what = sgxd_mkq_label_what(static_cast<u8>(l), &key);
                                out += fmt::format("{}{} {:#x} {:.1f}% ({} jobs)",
                                                   out.empty() ? "" : ", ", what, key, pct(by[l]),
                                                   t.jobs_by[l]);
                            }
                            return out;
                        };
                        LOG_INFO(Frontend, "perf: GPU 3D running by sender: {}",
                                 by_sender(t.gpu_3d_by));
                        LOG_INFO(Frontend, "perf: GPU TA running by sender: {}",
                                 by_sender(t.gpu_ta_by));
                    }
                    // This thread, the emulation thread: its run time by the kernel's count
                    // over the interval, and its waits for the render thread by kind (sleeps,
                    // the 1 ms timeouts among them). The holds are above, the DSP's waits below.
                    SceKernelThreadInfo emu_info{};
                    emu_info.size = sizeof(emu_info);
                    const int info_result =
                        sceKernelGetThreadInfo(sceKernelGetThreadId(), &emu_info);
                    ASSERT_MSG(info_result >= 0, "perflog: thread info {:#010x}",
                               static_cast<u32>(info_result));
                    using W = SgxdRenderer::RenderThread;
                    static SceKernelSysClock last_run = 0;
                    const SceKernelSysClock run_us = emu_info.runClocks - last_run;
                    last_run = emu_info.runClocks;
                    LOG_INFO(Frontend,
                             "perf: emulation thread ran {:.1f} ms of {:.1f}; waits for the "
                             "render thread: tickets {:.1f} ms in {} ({} sleeps, {} timed out), "
                             "ring space {:.1f} ms in {} ({}, {}), sync {:.1f} ms in {} ({}, {})",
                             run_us / 1e3, interval_us / 1e3, t.emu_wait_us[W::EmuTicket] / 1e3,
                             t.emu_waits[W::EmuTicket], t.emu_sleeps[W::EmuTicket],
                             t.emu_timeouts[W::EmuTicket], t.emu_wait_us[W::EmuSpace] / 1e3,
                             t.emu_waits[W::EmuSpace], t.emu_sleeps[W::EmuSpace],
                             t.emu_timeouts[W::EmuSpace], t.emu_wait_us[W::EmuSync] / 1e3,
                             t.emu_waits[W::EmuSync], t.emu_sleeps[W::EmuSync],
                             t.emu_timeouts[W::EmuSync]);
                    // The GPU's page holds (page_holds.h): the module's calls and the pages
                    // they held and let go of, the guest's aborts on held pages (those that
                    // waited for GPU work, and how long), the HLE's waits, and the vblank's
                    // waits for the last vblank's work.
                    const auto [tex_faults, tex_hle] =
                        system.GPU().GetRenderThread().TakeTextureWrites();
                    LOG_INFO(Frontend,
                             "perf: texture tracking: {} texture pages written by the guest, {} "
                             "by the HLE",
                             tex_faults, tex_hle);
                    const auto hs = system.GPU().GetRenderThread().TakeHoldStats();
                    LOG_INFO(Frontend,
                             "perf: page holds: azaharHold {} calls, {} runs, {} descriptors; "
                             "{} pages held, {} let go; guest aborts {} ({} versioned, {} "
                             "waited, {:.1f} ms; {} geometry pages moved), HLE waits {} ({:.1f} "
                             "ms), vblank waits {} ({:.1f} ms)",
                             hs.holds.calls, hs.holds.runs, hs.holds.descriptors,
                             hs.holds.held_pages, hs.holds.released_pages, hs.faults,
                             hs.versioned_faults, hs.fault_waits, hs.fault_wait_us / 1e3,
                             hs.geometry_moves,
                             hs.host_waits, hs.host_wait_us / 1e3, hs.vblank_waits,
                             hs.vblank_wait_us / 1e3);
                    // Page versions: azaharFlip's calls, the pages that got a second slot,
                    // the swaps, the writes in place (with queued work reading the guest's slot,
                    // and with none), the remaps at frame starts and before a same-frame use,
                    // the pages gone home, and the pool's slots in use.
                    LOG_INFO(Frontend,
                             "perf: page versions: azaharFlip {} calls; {} versioned, {} swaps, "
                             "{} written in place under queued reads, {} in place; remaps {} at "
                             "frame starts, {} early; {} home; {} slots in use",
                             hs.holds.flip_calls, hs.holds.versions, hs.holds.swaps,
                             hs.holds.in_place_read, hs.holds.in_place, hs.holds.frame_remaps,
                             hs.holds.early_remaps, hs.holds.homes, hs.holds.slots_used);
                    std::string by_kind;
                    for (u32 k = 0; k < SgxdRenderer::PageHolds::Kinds; k++) {
                        if (hs.fault_kinds[k] != 0) {
                            fmt::format_to(std::back_inserter(by_kind), "{}{} {} ({:.1f} ms)",
                                           by_kind.empty() ? "" : ", ",
                                           SgxdRenderer::PageHolds::KindName(
                                               static_cast<SgxdRenderer::PageHolds::Kind>(k)),
                                           hs.fault_kinds[k], hs.fault_kind_us[k] / 1e3);
                        }
                    }
                    LOG_INFO(Frontend, "perf: aborts that waited by hold: {}",
                             by_kind.empty() ? "none" : by_kind);
                    if (hs.site_count != 0) {
                        std::string sites;
                        for (u32 i = 0; i < hs.site_count; i++) {
                            const auto& site = hs.sites[i];
                            fmt::format_to(std::back_inserter(sites),
                                           "{}pc {:#010x} {} {} ({:.1f} ms, page {:#010x})",
                                           i == 0 ? "" : ", ", site.pc,
                                           SgxdRenderer::PageHolds::KindName(site.kind),
                                           site.at.count, site.at.us / 1e3, site.at.page);
                        }
                        LOG_INFO(Frontend, "perf: abort sites: {}", sites);
                    }
                    const auto& p = render_counters.present;
                    LOG_INFO(Frontend,
                             "perf: present ui update {:.1f} ms, screens {:.1f} ms, ui draw "
                             "{:.1f} ms, end frame {:.1f} ms; {} waits for a display buffer",
                             p.ui_update / 1e3, p.screens / 1e3, p.ui_draw / 1e3, p.end_frame / 1e3,
                             p.display_waits);
                    const auto& c = render_counters.cache;
                    LOG_INFO(Frontend, "perf: cache ops {:.1f} ms in {} for {} KiB", c.us / 1e3,
                             c.calls, c.bytes / 1024);
                    // By site: done (time, calls, KiB), and not needed because the range is
                    // uncached (calls, KiB).
                    static constexpr const char* site_names[] = {
                        "vertices",   "screens",  "dma source", "dma destination",
                        "textures",   "other"};
                    std::string sites;
                    for (u32 i = 0; i < static_cast<u32>(SgxdRenderer::CacheSite::Count); i++) {
                        const auto& s = c.sites[i];
                        if (s.calls == 0 && s.skipped_calls == 0) {
                            continue;
                        }
                        sites += fmt::format("{}{} {:.1f} ms in {} for {} KiB, {} skipped for {} KiB",
                                             sites.empty() ? "" : "; ", site_names[i],
                                             s.us / 1e3, s.calls, s.bytes / 1024, s.skipped_calls,
                                             s.skipped_bytes / 1024);
                    }
                    LOG_INFO(Frontend, "perf: cache sites: {}", sites.empty() ? "none" : sites);
                    const auto& m = render_counters.moves;
                    LOG_INFO(Frontend,
                             "perf: FCRAM to CDRAM: {} move(s), {} page(s), {} copied; {} "
                             "return(s), {} page(s); {:.1f} ms; pool {} of {} pages",
                             m.calls, m.pages, m.copied_pages, m.returns, m.returned_pages,
                             m.us / 1e3, m.pool_pages_used, m.pool_pages);
                    // The RomFS reads: a cached read's miss reads its line on the emulation
                    // thread, the larger reads go to an async thread.
                    {
                        const auto r = FileSys::TakeRomFSReadStats();
                        LOG_INFO(Frontend,
                                 "perf: romfs: {} cached reads ({} KiB), {} misses ({:.1f} ms on "
                                 "the reading thread); {} large reads ({} KiB, {:.1f} ms)",
                                 r.reads, r.read_bytes >> 10, r.misses, r.miss_us / 1e3, r.large,
                                 r.large_bytes >> 10, r.large_us / 1e3);
                    }
                    // The HLE DSP's thread: the frames it generated, the emulation thread's
                    // waits for one at its interrupt, the application's semaphore signals.
                    if (auto* dsp = dynamic_cast<AudioCore::DspHle*>(&system.DSP())) {
                        const auto d = dsp->TakeStats();
                        LOG_INFO(Frontend,
                                 "perf: dsp: {} frames, {:.1f} ms on its thread ({:.2f} ms a "
                                 "frame); {} waits at the interrupt, {:.2f} ms; {} semaphore "
                                 "signals, {:.0f} us after the interrupt (longest {}); {} ticks "
                                 "with no new frame from the application, {} buffer changes "
                                 "reported in them",
                                 d.frames, d.work_us / 1e3,
                                 d.frames ? d.work_us / 1e3 / d.frames : 0.0, d.waits,
                                 d.wait_us / 1e3, d.semaphores,
                                 d.semaphores ? static_cast<double>(d.answer_us) / d.semaphores
                                              : 0.0,
                                 d.answer_longest_us, d.late_ticks, d.late_buffer_changes);
                    }
                    // sgxd_flags threadtimes: each placed thread's run time in the second and
                    // the cores it may take; psp2prof's sampler holds core 3, so its share shows
                    // here only.
                    static const bool thread_times = SgxdRenderer::SgxdFlag("threadtimes");
                    if (thread_times) {
                        static std::unordered_map<int, u64> thread_runs;
                        std::string runs;
                        for (const Common::RoleThread& r : Common::RoleThreads()) {
                            SceKernelThreadInfo info{};
                            info.size = sizeof(info);
                            if (sceKernelGetThreadInfo(r.id, &info) < 0) {
                                continue;
                            }
                            const u64 now = info.runClocks;
                            const auto [it, fresh] = thread_runs.try_emplace(r.id, now);
                            const u64 delta = fresh ? 0 : now - it->second;
                            it->second = now;
                            std::string cores;
                            for (int core = 0; core < 4; core++) {
                                if (r.cores >> core & 1) {
                                    cores += static_cast<char>('0' + core);
                                }
                            }
                            runs += fmt::format("{}{} [{}] {:.1f}", runs.empty() ? "" : ", ",
                                                info.name, cores, delta / 1e3);
                        }
                        LOG_INFO(Frontend, "perf: threads' run time (ms): {}", runs);
                    }
                }
#endif
            }
        }
        // Nothing to present from here: the renderer presents each frame it finishes, inside
        // RunLoop (EmuWindowVita::SwapBuffers).
    }

    window.SetPauseMenu(nullptr);
    window.SetLoading(false);
    window.SetUi(nullptr);
    ShutdownSystem(system);
    return true;
}

#ifdef VITA_DIAGNOSTICS
std::string ReadWholeFile(const char* path) {
    std::string text;
    if (std::FILE* f = std::fopen(path, "rb")) {
        char buf[4096];
        while (const std::size_t n = std::fread(buf, 1, sizeof(buf), f)) {
            text.append(buf, n);
        }
        std::fclose(f);
    }
    return text;
}

bool IsCapture(std::string_view path) {
    return path.size() > 6 && path.substr(path.size() - 6) == ".vprof";
}

/**
 * The replay player: a vprof capture's frame on the GPU with no title at all
 * (tools/frame_replay). Only memory,
 * timing and the GPU exist (Core::System::InitReplay); the frame's commands and memory come
 * from the capture. The first requests are the vpk's app0:replay.txt when host0 has none,
 * later ones host0:vprof-replay.txt's, and the results go to host0 (vprof_replay.cpp), so the
 * player needs host0.
 */
void RunReplay(Core::System& system, VitaFrontend::EmuWindowVita& window,
               const std::string& capture) {
    window.UpdateCurrentFramebufferLayout();
    std::string first;
    SceIoStat st{};
    if (sceIoGetstat("host0:vprof-replay.txt", &st) < 0) {
        first = ReadWholeFile("app0:replay.txt");
        if (first.empty()) {
            first = "results\nprofile 30\nquit\n";
        }
    }
    std::string shown = first.empty() ? std::string{"host0:vprof-replay.txt's"} : first;
    std::replace(shown.begin(), shown.end(), '\n', ';');
    LOG_INFO(Frontend, "replay player: {}, first requests {}", capture, shown);
    SgxdRenderer::Vprof::Info("replay", capture.c_str());
    if (system.InitReplay(window) != Core::System::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "replay player: the GPU could not be brought up");
        return;
    }
    system.GPU().VprofReplay(capture, first);
    system.ShutdownReplay();
    LOG_INFO(Frontend, "replay player: done");
}
#endif

/**
 * The title to start without going through the list, if the launcher named one.
 *
 * Two ways in, because there are two launchers. A LiveArea bubble or another application
 * starts this one with a `psgm:play?titleid=...&param=<path>` string, which arrives through
 * sceAppMgrGetAppParam. The Vita3K emulator passes its --app-args as the process's own
 * arguments instead. Empty when neither did: then the list comes up as always.
 */
std::string LaunchParameter() {
    if (!g_arguments.empty()) {
        return g_arguments.front();
    }
    char param[1024]{};
    if (sceAppMgrGetAppParam(param) != 0) {
        return {};
    }
    const std::string_view text{param};
    if (text.find("psgm:play") == std::string_view::npos) {
        return {};
    }
    const std::size_t at = text.find("&param=");
    if (at == std::string_view::npos) {
        return {};
    }
    return std::string{text.substr(at + 7)};
}

bool module_installed(const std::string& name) {
    SceUID search_unk[2] = {0};
    return _vshKernelSearchModuleByName(name.c_str(), search_unk) >= 0;
}

int AppMgrOptionValues(std::string_view option);

/**
 * Finds the marker LoadAzaharNative's restart carries. The arguments after it, when there are
 * any, are the launch parameter the first process was started with and the ones after it
 * (load=N, save=N@T), which the restart would otherwise lose; SceAppMgr's options among them
 * are skipped as main does. The scan starts at argv[0] because nothing says whether the
 * restarted process gets its own path there.
 */
bool TakeRelaunchArguments(int argc, char* argv[]) {
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == nullptr || std::strcmp(argv[i], RelaunchArgument) != 0) {
            continue;
        }
        for (int j = i + 1; j < argc && argv[j] != nullptr && argv[j][0] != '\0'; ++j) {
            if (argv[j][0] == '-') {
                j += AppMgrOptionValues(argv[j]);
                continue;
            }
            g_arguments.emplace_back(argv[j]);
        }
        return true;
    }
    return false;
}

/**
 * Makes sure the kernel module is loaded: the one already there, or the one the vpk carries,
 * which takes the place of a loaded one that can be unloaded (UnloadAzaharNative).
 *
 * The app imports the module's calls through the weak stub, so it starts without the module.
 * The loader binds a process's imports when the process loads, and nothing here counts on a
 * library that arrives later being bound into a process that is already running. So the first
 * start after a boot loads the module and starts the app again, and the second process binds
 * to the module as it loads.
 *
 * Returns an empty string, or the error to show once a dialog can be shown. Does not return
 * when it restarts the app.
 */
/// Stops and unloads the loaded azaharnative (`version`, AZAHAR_NATIVE_UNLOADABLE on): for this
/// vpk's to take its place, or at the exit. True once it is gone. taiHEN takes the module by a UID
/// of this process, which the module gives (the one its load gave may have been an earlier
/// process's). This process is bound to it and makes no call into it after this; a restart binds
/// to the module loaded then.
bool UnloadAzaharNative(int version) {
    const SceUID module = azaharModuleUid();
    int stop = 0;
    const int r =
        module < 0 ? module : taiStopUnloadKernelModule(module, 0, nullptr, 0, nullptr, &stop);
    sceClibPrintf("[azahar] azaharnative %d unloaded: 0x%08x, its stop %d\n", version,
                  static_cast<u32>(r), stop);
    return r >= 0 && !module_installed("azaharnative");
}

std::string LoadAzaharNative(bool relaunched) {
    if (module_installed("azaharnative")) {
        // A module stays loaded until it is unloaded or the console reboots. One that can be
        // unloaded (AZAHAR_NATIVE_UNLOADABLE on) and is not this vpk's, an earlier or a later
        // Azahar's, is replaced below; any other stops the start until a reboot.
        const int loaded = azaharVersion();
        const bool replaced = loaded != AZAHAR_NATIVE_VERSION &&
                              loaded >= AZAHAR_NATIVE_UNLOADABLE && UnloadAzaharNative(loaded);
        if (!replaced) {
            if (loaded != AZAHAR_NATIVE_VERSION) {
                // Version 1 is the first release's module, which taiHEN's config.txt loaded at
                // boot; every later one comes with the vpk and is loaded here, at the launch.
                if (loaded <= 1) {
                    return "Remove the azaharnative.skprx line from your taiHEN config.txt "
                           "(ux0:tai/config.txt or ur0:tai/config.txt), then reboot your "
                           "console.";
                }
                // Retail firmware keeps a module that exports calls to user mode (a syscall
                // export, as this one's are) loaded outside development mode: SceKernelModulemgr's
                // stop answers 0x8002d002. This vpk's module is
                // loaded by the next start after a reboot.
                sceClibPrintf("[azahar] azaharnative %d stays loaded, this vpk's is %d: a reboot "
                              "finishes the update\n",
                              loaded, AZAHAR_NATIVE_VERSION);
                return "Azahar was updated. Reboot your console to finish the update.";
            }
            // The kernel's printf text from here on reaches this app's TTY (klog.c).
            SgxdRenderer::SgxdDrainKernelLog();
            // sgxd's spins time out on the calling thread's cycle counter (azaharCycles): every
            // thread of this process may read it, those that exist now and those created later.
            if (const int cycles = azaharCycleCounter(); cycles < 0) {
                return fmt::format("azaharCycleCounter failed ({:#010x}).",
                                   static_cast<u32>(cycles));
            }
            return {};
        }
        // sgxd's spins time out on the calling thread's cycle counter (azaharCycles): every
        // thread of this process may read it, those that exist now and those created later.
        if (const int cycles = azaharCycleCounter(); cycles < 0) {
            return fmt::format("azaharCycleCounter failed ({:#010x}).", static_cast<u32>(cycles));
        }
        return {};
    }
    const SceUID module = taiLoadStartKernelModule(AzaharNativePath, 0, nullptr, 0);
    if (module < 0) {
        return fmt::format("{} could not be loaded ({:#010x}). Reinstall the vpk.",
                           AzaharNativePath, static_cast<u32>(module));
    }
    // Restarting a second time would not bind anything the first restart did not.
    if (relaunched) {
        return "azaharnative.skprx was loaded twice without the restarted app seeing it.";
    }

    // The restart carries the launch parameter and the arguments after it (load=N, save=N@T).
    std::string marker{RelaunchArgument};
    std::vector<std::string> carried = g_arguments;
    if (carried.empty()) {
        if (std::string launch = LaunchParameter(); !launch.empty()) {
            carried.push_back(std::move(launch));
        }
    }
    std::vector<char*> args{marker.data()};
    for (std::string& argument : carried) {
        args.push_back(argument.data());
    }
    args.push_back(nullptr);
    const int exec = sceAppMgrLoadExec("app0:eboot.bin", args.data(), nullptr);
    if (exec >= 0) {
        // The call returns once the restart is queued, and the system ends this process soon
        // after. Wait for that rather than start an emulator whose calls are not bound.
        for (int waited = 0; waited < 50; ++waited) {
            sceKernelDelayThread(100 * 1000);
        }
    }
    return fmt::format("azaharnative.skprx is loaded, but Azahar could not restart to use it "
                       "({:#010x}). Start Azahar again.",
                       static_cast<u32>(exec));
}

/// The system's message dialog, over GXM: only before sgxd has the GPU, or when it could not
/// take it.
void show_error_dialog(const std::string& message) {
    if (!VitaFrontend::GxmPresent::Initialize()) {
        LOG_CRITICAL(Frontend, "Could not bring up GXM for: {}", message);
        return;
    }
    SceMsgDialogUserMessageParam msg_param{};
    msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
    msg_param.msg = reinterpret_cast<const SceChar8*>(message.c_str());

    SceMsgDialogParam param{};
    sceMsgDialogParamInit(&param);
    param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
    param.userMsgParam = &msg_param;

    sceMsgDialogInit(&param);

    while (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED) {
        VitaFrontend::GxmPresent::SwapCommonDialog();
    }
    sceMsgDialogTerm();
    VitaFrontend::GxmPresent::Shutdown();
}

/// The emulator proper. Runs on a thread of its own; see main.
int EmulatorMain() {
    // This thread drives the guest (which itself runs on the detached core 2, outside the
    // system scheduler) and runs the GPU; RoleCores in common/thread.cpp lays out
    // which of the three schedulable cores each host thread gets.
    Common::SetCurrentThreadRole(Common::ThreadRole::Emulation);

    SetupPaths();
    SetupLogging();
    MigrateNandLayout();
#ifdef VITA_DIAGNOSTICS
    // What SetupClocks got: the set calls' results are not checked.
    LOG_INFO(Frontend, "clocks: ARM {} MHz, bus {} MHz, GPU {} MHz, GPU xbar {} MHz",
             scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
             scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
#endif
    SetupNetwork();

    // What stops the start comes first, in the menus' language: before anything else is set up
    // and before the startup notice. Closing it exits the app.
    {
        SceCommonDialogConfigParam dialog_config_param{};
        sceCommonDialogSetConfigParam(&dialog_config_param);

        if (!g_native_error.empty()) {
            show_error_dialog(g_native_error);
            return -1;
        }

        if (!module_installed("CapUnlocker")) {
            show_error_dialog("CapUnlocker.skprx not installed. Install it from "
                              "https://github.com/GrapheneCt/CapUnlocker");
            return -1;
        }
    }

    SetupClocks();
#ifdef VITA_DIAGNOSTICS
    // What SetupClocks got: the set calls' results are not checked.
    LOG_INFO(Frontend, "clocks: ARM {} MHz, bus {} MHz, GPU {} MHz, GPU xbar {} MHz",
             scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
             scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
#endif
    MigrateNandLayout();
    SetupNetwork();
    VitaFrontend::Multiplayer::Init();

    // The GPU is sgxd's from here to the end of the session (docs/sgxd/takeover.md). No GXM call
    // may follow: SceGpu's callers would wait at its gate.
    std::string sgxd_error;
    if (!VitaFrontend::SgxdPresent::Initialize(&sgxd_error)) {
        LOG_CRITICAL(Frontend, "{}", sgxd_error);
        sceClibPrintf("[azahar] %s\n", sgxd_error.c_str());
        show_error_dialog(sgxd_error);
        return -1;
    }

    // SceShell draws nothing while sgxd owns the GPU: its GPU calls sleep at SceGpu's gate
    // (docs/sgxd/takeover.md), and azaharnative keeps SceAppMgr's
    // watchdog from taking it for hung. So every shell overlay that can be locked is, for the
    // whole session: the PS button (its presses reach the pad data instead, and the pause menu
    // opens on them), the quick menu, the power menu, and the USB, memory card and music player
    // notices. The volume keys are this app's (azaharnative).
    const auto shell_locks = static_cast<SceShellUtilLockType>(
        SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN | SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2 |
        SCE_SHELL_UTIL_LOCK_TYPE_QUICK_MENU | SCE_SHELL_UTIL_LOCK_TYPE_POWEROFF_MENU |
        SCE_SHELL_UTIL_LOCK_TYPE_USB_CONNECTION | SCE_SHELL_UTIL_LOCK_TYPE_MC_INSERTED |
        SCE_SHELL_UTIL_LOCK_TYPE_MC_REMOVED | SCE_SHELL_UTIL_LOCK_TYPE_MUSIC_PLAYER);
    sceShellUtilInitEvents(0);
    sceShellUtilLock(shell_locks);

    auto& system = Core::System::GetInstance();

    // The Vita has neither the memory nor the drivers for the LLE service modules. GXM is the
    // only renderer in this build: the software renderer was too slow on this
    // CPU to be worth carrying and is no longer compiled for the console.
    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }
    Settings::values.graphics_api = Settings::GraphicsAPI::Gxm;
    // Vertex shading happens on the GPU or not at all: with this off, the PICA code would send
    // every draw down its CPU vertex path, which the sgxd rasterizer does not have.
    Settings::values.use_hw_shader = true;
    Settings::values.disable_right_eye_render = true;
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

    VitaFrontend::EmuWindowVita window(system);
    // Not InputCommon::Init(): every factory it registers is one this frontend does not use (the
    // buttons, circle pad and touch come from the "vita" engine and EmuWindow's own factory).
    VitaFrontend::InitInput(window);

    VitaFrontend::Ui ui(system, window);

    VitaFrontend::CrashReport::EnableRecovery(restart,
                                              CommandChannel() ? command_file.LastSeq() : nullptr);
    bool running = true;
#ifdef VITA_DIAGNOSTICS
    // The replay player's vpk carries its capture; the emulator's replays one it is launched
    // with.
    std::string replay = LaunchParameter();
#ifdef AZAHAR_REPLAY_PLAYER
    if (!IsCapture(replay)) {
        replay = "app0:replay.vprof";
    }
#endif
    if (IsCapture(replay)) {
        RunReplay(system, window, replay);
        running = false;
    } else
#endif
    if (const std::string launch = LaunchParameter(); !launch.empty()) {
        LOG_INFO(Frontend, "launch parameter names {}", launch);
        running = RunTitle(system, window, ui, launch);
    }
    while (running && !window.IsQuitRequested()) {
        window.PollEvents();
        const auto action = ui.Update();
        window.Present(&ui);
        // The present waits for no vblank: a frame a refresh.
        sceDisplayWaitVblankStart();

        switch (action) {
        case VitaFrontend::Ui::Action::LaunchGame: {
            running = RunTitle(system, window, ui, ui.SelectedGamePath());
            break;
        }
        case VitaFrontend::Ui::Action::ExitApplication:
            running = false;
            break;
        default:
            break;
        }
    }

    VitaFrontend::ShutdownInput();
    Common::Log::Stop();

    sceShellUtilUnlock(shell_locks);
    VitaFrontend::SgxdPresent::Shutdown();
    return 0;
}
} // Anonymous namespace

int main(int argc, char* argv[]) {
    const bool relaunched = TakeRelaunchArguments(argc, argv);
#ifdef VITA_DIAGNOSTICS
    // The extended-memory mode (ATTRIBUTE2=12, ~365 MB) is only granted with every system app
    // closed; without it the 240 MB heap overruns the plain partition and newlib silently has
    // no heap at all.
    {
        void* probe = std::malloc(64);
        if (probe == nullptr) {
            sceClibPrintf("[azahar] the %u MB heap could not be allocated. The extended memory "
                          "mode (ATTRIBUTE2=12, 365 MB) is granted only with every system app "
                          "closed: browser, store, mail, content manager, a PSP bubble. Close "
                          "them and start again.\n",
                          static_cast<unsigned>(_newlib_heap_size_user / (1024 * 1024)));
            sceKernelExitProcess(0);
        }
        std::free(probe);
    }
    // A restart's arguments are its marker and the launch parameter, taken above.
    for (int i = 1; i < argc && !relaunched; ++i) {
        if (argv[i] != nullptr && argv[i][0] != '\0') {
            g_arguments.emplace_back(argv[i]);
        }
    }
    SetupTerminateHandler();
    VitaFrontend::CrashReport::PrintLast();
    // Before anything is brought up, because the first start after a boot restarts from here.
    g_native_error = LoadAzaharNative(relaunched);
    // Without the module (g_native_error) its calls are not bound: the report lists no plugins.
    VitaFrontend::CrashReport::NoteSystem(g_native_error ? 0 : azaharVersion(), g_cores_unlocked);
    Common::SetCurrentThreadRole(Common::ThreadRole::Other);
    // The process's own thread has a homebrew launcher's small stack, and the global meant to
    // change that does not work: everything runs on this thread instead (service handling and the
    // savestate serializer go deep). Its stack comes from the game partition, mostly the newlib
    // heap's.
    constexpr std::size_t EmulationStackSize = 4 * 1024 * 1024;

    {
        Common::NamedThread emu_thread{Common::ThreadCfg{"emulation", EmulationStackSize},
                                       EmulatorMain};
        emu_thread.join();
    }
    // Nothing of Azahar's stays in the kernel after an exit: the session gave everything back (the
    // GPU at the frontend's shutdown, the core and the moved pages at the title's end), so the
    // module stops, and the next launch loads it again. One that cannot be unloaded stays, and so
    // does this one if its stop finds anything still held (the process's exit takes that back).
    if (g_native_error.empty() && azaharVersion() >= AZAHAR_NATIVE_UNLOADABLE) {
        UnloadAzaharNative(azaharVersion());
    }
    return 0;
}

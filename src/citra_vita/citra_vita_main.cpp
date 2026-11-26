// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>
#include <malloc.h>
#include <pthread.h>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <psp2/appmgr.h>
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/error.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/shellutil.h>
#include <psp2/vshbridge.h>
#include <taihen.h>
#include "citra_vita/adhoc.h"
#include "citra_vita/banner_player.h"
#include "citra_vita/config.h"
#include "citra_vita/crash_report.h"
#include "citra_vita/sgxd_present.h"

#include "citra_vita/emu_window_vita.h"
#include "citra_vita/input_factory_vita.h"
#include "citra_vita/multiplayer.h"

#include "azahar_native.h"
#include "common/heap_check.h"
#include "common/named_thread.h"
#include "core/arm/vita/arm_vita_native.h"
#include "core/file_sys/romfs_reader.h"
#include "core/hle/kernel/hle_ipc.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/wait_object.h"
#include "core/memory.h"
#include "sgxd_mkq.h"
#include "sgxd_ptla.h"
#include "sgxd_session.h"
#include "video_core/renderer_sgxd/vprof.h"

#include <cstdio>

#include "audio_core/hle/hle.h"
#include "citra_vita/hud_stats.h"
#include "citra_vita/i18n.h"
#include "citra_vita/vita_ui.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "core/loader/title_patches.h"
#include "core/movie.h"
#include "video_core/gpu.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/render_thread.h"
#include "video_core/renderer_sgxd/renderer_sgxd.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"
#include "video_core/right_eye_disabler.h"

#ifdef AZAHAR_PGO_GENERATE
// LLVM's profile runtime (tools/pgo): the profile serialised into a buffer of the caller's.
extern "C" uint64_t __llvm_profile_get_size_for_buffer(void);
extern "C" int __llvm_profile_write_buffer(char* buffer);
#endif

// The default heap is nowhere near enough for a 3DS: FCRAM alone is 128 MB, and the caches and
// the surface storage sit on top of it. Newlib reads this at startup.
extern "C" {
// The newlib heap: the emulator's working set, 21-32 MiB in play on players' consoles; guest
// memory and the large transients have blocks of their own (HostSharedMemory). newlib takes the
// whole heap as one memblock at startup and the kernel commits it, so every megabyte here is one
// the textures pool's overflow cannot have: a retail Vita leaves a game about 28 MiB besides. The
// replay player reads whole captures into it.
#ifdef AZAHAR_REPLAY_PLAYER
unsigned int _newlib_heap_size_user = 112 * 1024 * 1024;
#else
unsigned int _newlib_heap_size_user = 64 * 1024 * 1024;
#endif

// Default stack for every pthread, and so for every std::thread, which cannot ask for one.
// vitasdk's default is 32 KB, which the renderer and the savestate serializer overrun.
unsigned int _pthread_stack_default_user = 1024 * 1024;

// newlib's lazy network bring-up (libc/sys/vita/vitanet.c). Not declared in any header, but a
// global symbol, and guarded by its own atomic once-flag, so calling it twice costs nothing.
int _vita_net_init(void);
}

// The app runs with its module's own version only: another one loaded stops the start
// (LoadAzaharNative).

namespace {

using VitaFrontend::I18n::Tr;
using VitaFrontend::I18n::Translatable;
using VitaFrontend::I18n::TrFormat;
std::vector<std::string> g_arguments;
/// Whether the process was launched by its SELF's path (LOADP, SceAppMgr's
/// LaunchAppByPath), not from the LiveArea; LoadAzaharNative's restart carries it on.
bool g_launched_by_path = false;

/// Where the vpk puts the kernel module. The kernel opens the file itself, and app0: is this
/// process's mount rather than a path the kernel resolves, so this is the installed copy.
constexpr char AzaharNativePath[] = "ux0:app/" AZAHAR_VITA_TITLEID "/azaharnative.skprx";
/// Marks the one restart that loading the module costs; see LoadAzaharNative.
constexpr char RelaunchArgument[] = "--azaharnative-loaded";
/// What stops the start, composed when it is shown: by then the settings have chosen the menus'
/// language, which they have not when the error is found.
using StartError = std::function<std::string()>;
/// What LoadAzaharNative returned: none, or the error EmulatorMain shows once it can.
StartError g_native_error;
/// azaharUnlockCores': core 3 for this app's threads, CapUnlocker's or the module's own; 0 not
/// asked (no module), negative refused.
int g_cores_unlocked = 0;
/// A crash's notice for the game list to show, the player closing it: a restart's after
/// CrashReport::Recover (crashed=PATH), or a title's that crashed (RunTitle).
std::string g_crash_notice;

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

/// The log goes to the debug channel (Common::Log's VitaBackend). What prints there itself
/// (sceClibPrintf) cannot wait for the log's lock or allocate: main's lines before this, a crash's,
/// the power callbacks' and the heap walks'.
Common::Log::Filter log_filter;

void SetupLogging() {
    Common::Log::Initialize();
    Common::Log::Filter& filter = log_filter;
#ifdef VITA_DIAGNOSTICS
    // The guest's own svcOutputDebugString lines (homebrew tests report through it).
    filter.ParseFilterString("*:Info Debug.Emulated:Debug");
#else
    // What a title's calls log (services, kernel, hardware) only when critical: a title calls a
    // stub or an unimplemented path up to hundreds of times a second, a line each. The
    // emulator's own classes stay, for the crash report's console tail.
    filter.ParseFilterString("*:Critical Common:Info Core:Info Config:Info Savestate:Info "
                             "Frontend:Info Render:Info Loader:Info");
#endif
    Common::Log::SetGlobalFilter(filter);
}

/// The Vita boots at a conservative clock; an emulated 3DS needs every megahertz. The GPU's
/// clock is set before sgxd takes the GPU and never changes after (docs/sgxd/takeover.md).
void SetupClocks() {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
}

const char* DescribeLoadError(Core::System::ResultStatus status) {
    switch (status) {
    case Core::System::ResultStatus::ErrorGetLoader:
        return Translatable("unknown file type");
    case Core::System::ResultStatus::ErrorSystemMode:
        return Translatable("could not read the system mode");
    case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
        return Translatable("it is encrypted. Decrypt it, then try again.");
    case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
        return Translatable("unsupported file format");
    case Core::System::ResultStatus::ErrorLoader_ErrorGbaTitle:
        return Translatable("GBA titles are not supported");
    case Core::System::ResultStatus::ErrorNotInitialized:
        return Translatable("the CPU could not start: azaharnative could not take a core");
    default:
        return Translatable("unknown error");
    }
}

/// Shuts the system down. `threads_before`: the threads running before the title
/// (RunningNamedThreads), all that may be left after it.
void ShutdownSystem(Core::System& system, std::size_t threads_before) {
    // A title's process alive after its shutdown keeps its kernel objects and its page table
    // (5 MiB) on the heap for good. Reported with the HLE async sections in flight.
    const std::weak_ptr<Kernel::Process> process = system.Kernel().GetCurrentProcess();
    const std::size_t in_flight = Kernel::AsyncSectionsInFlight();
    // Every thread of the title ends before the title does: the HLE requests' background work
    // finishes and its threads join here, while everything it may touch is still there; the
    // shutdown joins the rest (the render thread, the DSP's, the services' workers).
    Kernel::JoinAsyncSections();
    system.Shutdown();
    // The title's GPU session is over: nothing of it is left for the next one (the game list's
    // banner) to wait on.
    VitaFrontend::SgxdPresent::SessionEnded();
    if (const std::shared_ptr<Kernel::Process> alive = process.lock()) {
        LOG_INFO(Frontend,
                 "shutdown: the title's process outlived it ({} references); HLE async sections in "
                 "flight: {} at the quit, {} now",
                 static_cast<long>(alive.use_count() - 1), static_cast<unsigned>(in_flight),
                 static_cast<unsigned>(Kernel::AsyncSectionsInFlight()));
    }
    if (const std::size_t running = Common::RunningNamedThreads(); running != threads_before) {
        LOG_INFO(Frontend, "shutdown: {} threads running, {} before the title",
                 static_cast<unsigned>(running), static_cast<unsigned>(threads_before));
    }
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
            c.moves = gpu.Renderer().TakeMoveStats();
            render_counters_state.store(2, std::memory_order_release);
        });
    }
    return taken;
}
#endif

/// Runs one title to completion. Returns false when the user wants the application to close
/// rather than to return to the game list.
#ifdef VITA_DEBUG_TOOLS
/**
 * The savestate arguments after the title's path: `load=N` restores slot N before the title runs,
 * `save=N@T` saves slot N once it has run T seconds. A scripted launch passes them at the launch to
 * jump to a scene a run saved. Taken once: a title started from the list later gets none.
 * `scale=1.5`, `layout=N` (`layout=overlay`), `wide` (`wide=0`, `wide=stretch`), `opacity=N`,
 * `vsync=0`, `limit=N`, `skips=N` (`skips=none`), `gpuwait=0`, `motion=0`, `rightstick=lr`
 * (`cstick`, `dpad`) and `stats=0` set the internal resolution, the screen layout, widescreen,
 * the overlay's opacity, vsync, the speed limit, the late skip's cap, the wait for GPU results,
 * the motion controls, what the right stick drives and the stats overlay as the settings would,
 * for the session: they pin the option over the settings file's values (Config::Pin), so that a
 * run is what its arguments say whatever a player chose on the console, and are never written to
 * the file. `clock=N` pins the emulated CPU's clock in percent the same way (at 50, a millisecond
 * the guest runs is two of emulated time). `commands` turns on the command channel
 * (CommandChannel) for the whole run, and `notice` shows the start-up notice on a launch by path,
 * which skips it otherwise (EmulatorMain).
 */
struct StateArguments {
    int load = -1;
    int save = -1;
    SceUInt64 save_after_us = 0;
};
#endif

namespace Config = VitaFrontend::Config;
using Config::Option;

#ifdef VITA_DEBUG_TOOLS
/// `layout=N`: N is a Settings::LayoutOption; the menu's layouts
/// are the ones it offers. Empty for any other argument, or a layout the menu does not offer.
std::optional<Config::Layout> LayoutArgument(const std::string& argument) {
    if (!argument.starts_with("layout=")) {
        return std::nullopt;
    }
    switch (static_cast<Settings::LayoutOption>(std::atoi(argument.c_str() + 7))) {
    case Settings::LayoutOption::Default:
        return Config::Layout::Default;
    case Settings::LayoutOption::SingleScreen:
        return Config::Layout::SingleScreen;
    case Settings::LayoutOption::LargeScreen:
        return Config::Layout::LargeScreen;
    case Settings::LayoutOption::SideScreen:
        return Config::Layout::SideBySide;
    case Settings::LayoutOption::HybridScreen:
        return Config::Layout::Hybrid;
    default:
        return std::nullopt;
    }
}

/// Launch argument statslog=1: the stats overlay's numbers on the console once a second,
/// release builds too.
bool stats_log = false;
/// Launch argument stuck=0: no Multicore stuck question (RunTitle), for test homebrews that put
/// no frame on the screen; release builds too.
bool stuck_check = true;

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
        } else if (argument == "commands" || argument == "crashhold" || argument == "notice" ||
                   argument.starts_with("seq=") || argument.starts_with("crashed=") ||
                   argument.starts_with("changed=")) {
            // CommandChannel's, a crashed title's hold (RunTitle), EmulatorMain's start-up
            // notice, and a crash's restart's (main).
        } else if (argument.starts_with("scale=")) {
            // The top screen's scale: 1, 1.25, 1.5, 1.75 or 2 (Config::TopScaleValue).
            Config::Pin(Option::TopScale,
                        Config::TopScaleValue(static_cast<float>(std::atof(argument.c_str() + 6))));
        } else if (argument == "layout=overlay") {
            Config::Pin(Option::ScreenLayout, static_cast<int>(Config::Layout::Overlay));
        } else if (argument == "layout=largeoverlay") {
            Config::Pin(Option::ScreenLayout, static_cast<int>(Config::Layout::LargeOverlay));
        } else if (const auto layout = LayoutArgument(argument); layout) {
            Config::Pin(Option::ScreenLayout, static_cast<int>(*layout));
        } else if (argument == "wide") {
            Config::Pin(Option::Widescreen, 1);
        } else if (argument == "wide=0") {
            Config::Pin(Option::Widescreen, 0);
        } else if (argument == "wide=stretch") {
            Config::Pin(Option::Widescreen, 2);
        } else if (argument.starts_with("msaa=")) {
            Config::Pin(Option::Msaa, std::atoi(argument.c_str() + 5) != 0 ? 1 : 0);
        } else if (argument.starts_with("stats=")) {
            Config::Pin(Option::StatsOverlay, std::atoi(argument.c_str() + 6) != 0 ? 1 : 0);
        } else if (argument.starts_with("statslog=")) {
            stats_log = std::atoi(argument.c_str() + 9) != 0;
        } else if (argument == "stuck=0") {
            stuck_check = false;
        } else if (argument.starts_with("log=")) {
            // A log filter rule on top of SetupLogging's: log=Service.FS:Debug, the ROM's reads.
            log_filter.ParseFilterString(argument.substr(4));
            Common::Log::SetGlobalFilter(log_filter);
        } else if (argument.starts_with("motion=")) {
            Config::Pin(Option::Motion, std::atoi(argument.c_str() + 7) != 0 ? 1 : 0);
        } else if (argument == "rightstick=cstick" || argument == "rightstick=lr" ||
                   argument == "rightstick=dpad") {
            using Mode = Config::RightStickMode;
            const Mode mode = argument.ends_with("lr")     ? Mode::ShoulderButtons
                              : argument.ends_with("dpad") ? Mode::DPad
                                                           : Mode::CStick;
            Config::Pin(Option::RightStick, static_cast<int>(mode));
        } else if (argument.starts_with("opacity=")) {
            // The overlay layout's other screen, in percent (10 to 100).
            Config::Pin(Option::OverlayOpacity, std::atoi(argument.c_str() + 8));
        } else if (argument.starts_with("vsync=")) {
            Config::Pin(Option::Vsync, std::atoi(argument.c_str() + 6) != 0 ? 1 : 0);
        } else if (argument.starts_with("clock=")) {
            Config::Pin(Option::CpuClock, std::atoi(argument.c_str() + 6));
        } else if (argument.starts_with("limit=")) {
            Config::Pin(Option::SpeedLimit, std::atoi(argument.c_str() + 6));
        } else if (argument == "skips=none") {
            Config::Pin(Option::FrameSkipLimit, Config::NoFrameSkipLimit);
        } else if (argument.starts_with("skips=") && std::atoi(argument.c_str() + 6) >= 0 &&
                   std::atoi(argument.c_str() + 6) < Config::NoFrameSkipLimit) {
            // The late skip's cap in frames, 0 to 4 as the settings offer them.
            Config::Pin(Option::FrameSkipLimit, std::atoi(argument.c_str() + 6));
        } else if (argument.starts_with("gpuwait=")) {
            Config::Pin(Option::WaitForGpu, std::atoi(argument.c_str() + 8) != 0 ? 1 : 0);
        } else if (argument.starts_with("righteye=")) {
            Config::Pin(Option::SkipRightEye, std::atoi(argument.c_str() + 9) != 0 ? 1 : 0);
        } else if (argument.starts_with("gputimings=")) {
            Config::Pin(Option::SimulateGpuTimings,
                        std::atoi(argument.c_str() + 11) != 0 ? 1 : 0);
        } else if (argument.starts_with("multicore=")) {
            Config::Pin(Option::Multicore, std::atoi(argument.c_str() + 10) != 0 ? 1 : 0);
        } else if (argument.starts_with("new3ds=")) {
            Config::Pin(Option::ForceNew3ds, std::atoi(argument.c_str() + 7) != 0 ? 1 : 0);
        } else if (argument == "diskcache=0") {
            // Profiling runs: every program the first tier's, none from an earlier run's file.
            Settings::values.use_disk_shader_cache = false;
        } else if (argument.starts_with("save=")) {
            a.save = std::atoi(argument.c_str() + 5);
            if (const std::size_t at = argument.find('@'); at != std::string::npos) {
                a.save_after_us = std::strtoull(argument.c_str() + at + 1, nullptr, 10) * 1000000;
            }
        } else {
            LOG_INFO(Frontend, "launch argument {} not known", argument);
        }
    }
    return a;
}

/**
 * The command channel (the `commands` launch argument): host0:azahar-commands.txt,
 * which a session on the host appends lines to. In a title: "SEQ save N", "SEQ load N",
 * (diagnostics) "SEQ dump PADDR SIZE", hexadecimal, into host0:azahar-dump-PADDR.bin, "SEQ vdump
 * VADDR SIZE", the title's memory at a virtual address, into host0:azahar-vdump-VADDR.bin, "SEQ
 * threads", every guest thread to the console, "SEQ quit" (to the game list) or "SEQ exit", as
 * the pause menu quits, and "SEQ menu quit", the pause menu itself opened and its "Quit game"
 * chosen a second later. On the game list: "SEQ launch PATH", "SEQ select TEXT" (the first
 * title whose name or path holds TEXT selected, as the pad would) and "SEQ exit". Both: "SEQ limit
 * N" (the speed limit, percent, 0 none), "SEQ vsync N", "SEQ name TEXT" (the console name, kept
 * as the setting is, for the next title started), "SEQ heap" (the heap's use), "SEQ rm PATH"
 * and "SEQ copy SRC|DST" (a file on the console removed or copied). It is
 * read once a second; a line runs once, when its SEQ is past the last one run, so the host never
 * has to remove the file (a new launch starts from SEQ 0 again, so the session empties it at each
 * launch). Only the argument turns this on.
 */
class CommandFile {
public:
    std::string Poll() {
        const SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now - last_read_us < 1000000) {
            return {};
        }
        last_read_us = now;
        VitaFrontend::CrashReport::StallWatchBeat();
        // `quiet N`: no host0 request for N seconds, the loop still beating the stall watch.
        if (now < quiet_until_us) {
            return {};
        }
        // One request a second while nothing changes: each host0 request is a round trip over the
        // debug link, whose lost answers stop the asking thread for good.
        SceIoStat stat{};
        if (sceIoGetstat(Path, &stat) < 0) {
            return {};
        }
        if (stat.st_size == last_size && std::memcmp(&stat.st_mtime, &last_mtime,
                                                     sizeof(last_mtime)) == 0 && !more) {
            return {};
        }
        last_size = stat.st_size;
        last_mtime = stat.st_mtime;
        const SceUID fd = sceIoOpen(Path, SCE_O_RDONLY, 0);
        if (fd < 0) {
            return {};
        }
        static char text[16 * 1024];
        const int got = sceIoRead(fd, text, sizeof(text) - 1);
        sceIoClose(fd);
        text[got > 0 ? got : 0] = '\0';
        std::string due;
        more = false;
        for (char* line = text; *line != '\0';) {
            char* const end = std::strchr(line, '\n');
            if (end != nullptr) {
                *end = '\0';
            }
            char* rest = nullptr;
            const unsigned long seq = std::strtoul(line, &rest, 10);
            if (rest != line && seq > last_seq) {
                if (!due.empty()) {
                    // Another one after it: read again at the next second.
                    more = true;
                    break;
                }
                last_seq = seq;
                due = rest;
                due.erase(0, due.find_first_not_of(' '));
                due.erase(due.find_last_not_of(" \r") + 1);
            }
            if (end == nullptr) {
                break;
            }
            line = end + 1;
        }
        // `quiet N` (the debug link loses a host0 answer now and then while upcalls run,
        // which stops the asking thread for good): this channel leaves host0 alone for N
        // seconds, so that a title's boot runs with no request on the link.
        if (due.rfind("quiet ", 0) == 0) {
            const unsigned long seconds = std::strtoul(due.c_str() + 6, nullptr, 10);
            quiet_until_us = now + std::min(seconds, 600ul) * 1000000;
            LOG_INFO(Frontend, "quiet: no host0 requests for {} s (command)", seconds);
            return {};
        }
        return due;
    }

    /// The last line run, for a crash's restart (seq=N), which goes on after it rather than run
    /// the file's lines again (the crash's among them).
    [[nodiscard]] const unsigned long* LastSeq() const {
        return &last_seq;
    }
    void StartAfter(unsigned long seq) {
        last_seq = seq;
    }

private:
    // On the memory card, where a host without a debug link (one that reaches the console's
    // files over the network) can write it; such a host cannot pass arguments either
    // (launch.txt), and has no other way to end a run that owns the GPU.
    static constexpr const char* Path = "ux0:data/azahar/commands.txt";
    SceUInt64 quiet_until_us = 0;
    SceUInt64 last_read_us = 0;
    unsigned long last_seq = 0;
    SceOff last_size = -1;
    SceDateTime last_mtime{};
    bool more = false;
};

/// One for the run: the SEQ count goes on across the titles it launches.
CommandFile command_file;

bool CommandChannel() {
    static const bool on =
        g_arguments.size() > 1 &&
        std::find(g_arguments.begin() + 1, g_arguments.end(), "commands") != g_arguments.end();
    return on;
}

/// newlib's malloc state (mallocr.c): where its arena starts, and its bins, the third of which
/// is the top chunk (the arena's free end).
extern "C" char* __malloc_sbrk_base;
extern "C" void* __malloc_av_[];
extern "C" void __malloc_lock(struct _reent*);
extern "C" void __malloc_unlock(struct _reent*);

#ifdef VITA_DIAGNOSTICS
/// `heapmap`: the chunks the heap holds, walked from the arena's start to its top: the in-use ones
/// past the arena's first 2 MiB (what is left there after a quit splits the heap's free space), by
/// offset, size and first two words (a vtable or a string names it), and the largest free run.
void PrintHeapMap() {
    __malloc_lock(_REENT);
    const uintptr_t base = (reinterpret_cast<uintptr_t>(__malloc_sbrk_base) + 7) & ~uintptr_t{7};
    const uintptr_t top = reinterpret_cast<uintptr_t>(__malloc_av_[2]);
    struct Used {
        u32 offset, size, word0, word1;
    };
    static std::array<Used, 64> used;
    u32 used_count = 0, used_total = 0, used_past = 0, largest_free = 0, free_run = 0;
    for (uintptr_t p = base; p < top;) {
        const u32 size = *reinterpret_cast<const u32*>(p + 4) & ~3u;
        if (size < 16 || p + size > top) {
            sceClibPrintf("[azahar] heapmap: a chunk of %u bytes at +%x ends the walk\n", size,
                          static_cast<unsigned>(p - base));
            break;
        }
        const bool in_use = (*reinterpret_cast<const u32*>(p + size + 4) & 1u) != 0;
        if (in_use) {
            used_total += size;
            largest_free = std::max(largest_free, free_run);
            free_run = 0;
            if (p - base >= (2u << 20)) {
                used_past += size;
                if (used_count < used.size()) {
                    const u32* const words = reinterpret_cast<const u32*>(p + 8);
                    used[used_count] = {static_cast<u32>(p - base), size, words[0], words[1]};
                }
                used_count++;
            }
        } else {
            free_run += size;
        }
        p += size;
    }
    largest_free = std::max(largest_free, free_run);
    const u32 top_size = *reinterpret_cast<const u32*>(top + 4) & ~3u;
    __malloc_unlock(_REENT);
    sceClibPrintf("[azahar] heapmap: arena %u KiB, %u KiB in use, %u KiB of it in %u chunks past "
                  "2 MiB; largest free run %u KiB, top %u KiB (command)\n",
                  static_cast<unsigned>((top - base) >> 10), used_total >> 10, used_past >> 10,
                  used_count, largest_free >> 10, top_size >> 10);
    for (u32 i = 0; i < std::min<u32>(used_count, used.size()); i++) {
        sceClibPrintf("[azahar] heapmap: +%08x %u bytes: %08x %08x\n", used[i].offset,
                      used[i].size, used[i].word0, used[i].word1);
    }
}

/// newlib's per-thread reentrancy slots (sys/vita/threading.c's reent_list, 256 of them, the main
/// thread's first): {thread, reclaim pending, TLS data, pthread data, the reentrancy state}.
struct NewlibReentSlot {
    int thread_id;
    int needs_reclaim;
    void* tls_data_ext;
    void* pthread_data_ext;
    struct _reent reent;
};
NewlibReentSlot* newlib_reent_slots;
extern "C" struct _reent* __getreent(void); ///< the calling thread's (`_REENT` is the global one here)

/// `reents`: the slots in use or holding dtoa state: owner (a dead one's name is gone), reclaim
/// pending, the dtoa result, its powers of five and its Bigint lists. A slot freed by its thread's
/// end keeps all of it until a new thread takes the slot.
void PrintReentSlots() {
    u32 used = 0;
    for (u32 i = 0; i < 256; i++) {
        NewlibReentSlot& slot = newlib_reent_slots[i];
        struct _reent* const r = &slot.reent;
        if (slot.thread_id == 0 && _REENT_MP_RESULT(r) == nullptr && _REENT_MP_P5S(r) == nullptr &&
            _REENT_MP_FREELIST(r) == nullptr) {
            continue;
        }
        used++;
        SceKernelThreadInfo info{};
        info.size = sizeof(info);
        const bool alive = slot.thread_id != 0 && sceKernelGetThreadInfo(slot.thread_id, &info) >= 0;
        sceClibPrintf("[azahar] reents: slot %u thread %08x %s%s result %p p5s %p freelist %p\n", i,
                      static_cast<unsigned>(slot.thread_id), alive ? info.name : "(gone)",
                      slot.needs_reclaim ? " reclaim pending" : "",
                      static_cast<void*>(_REENT_MP_RESULT(r)), static_cast<void*>(_REENT_MP_P5S(r)),
                      static_cast<void*>(_REENT_MP_FREELIST(r)));
    }
    sceClibPrintf("[azahar] reents: %u slots shown (command)\n", used);
}
#endif

/// The commands the title's loop and the game list's both take: `limit N`, `vsync N` and
/// `heap`, the newlib heap's use (a title quit and launched again leaves what it did not free)
/// and the largest block it can still give (what scattered leftovers cut it into), and the
/// system's free memory beside it.
/// `layout N` (the screen layout's value: the menu's choices, Config::Layout's), `swap 0|1` and
/// `wide 0|1|2` (off, the hack, stretched): pinned as the launch arguments pin them, and the
/// screens placed again at the window's next poll.
bool RunLayoutCommand(VitaFrontend::EmuWindowVita& window, const std::string& command) {
    char name[8] = {};
    int value = 0;
    if (std::sscanf(command.c_str(), "%7s %d", name, &value) != 2) {
        return false;
    }
    const std::string_view word{name};
    const Option option = word == "layout" ? Option::ScreenLayout
                          : word == "swap" ? Option::SwapScreens
                          : word == "wide" ? Option::Widescreen
                                           : Option::Count;
    if (option == Option::Count) {
        return false;
    }
    Config::Pin(option, value);
    window.RequestLayoutUpdate();
    LOG_INFO(Frontend, "{} {} (command)", name, Config::Effective(option));
    return true;
}

bool RunSettingCommand(const std::string& command) {
    if (command == "suspend") {
        // The power button's short press, for a console with none to press remotely; its
        // resume is the CP's power-on.
        LOG_INFO(Frontend, "suspend (command): 0x{:08x}",
                 static_cast<unsigned>(scePowerRequestSuspend()));
        return true;
    }
#ifdef VITA_DIAGNOSTICS
    if (command == "heap") {
        const struct mallinfo m = mallinfo();
        u32 low = 0, high = _newlib_heap_size_user;
        while (high - low > 64 * 1024) {
            const u32 size = low + (high - low) / 2;
            // Through a volatile: a malloc only freed again is otherwise removed by the
            // compiler, as if it had succeeded.
            void* volatile p = std::malloc(size);
            if (p != nullptr) {
                std::free(p);
                low = size;
            } else {
                high = size;
            }
        }
        // Outside the heap: thread stacks and memory blocks.
        SceKernelFreeMemorySizeInfo free{};
        free.size = sizeof(free);
        sceKernelGetFreeMemorySize(&free);
        LOG_INFO(
            Frontend,
            "heap: {} KiB in use, {} KiB free inside the {} KiB taken, of {} KiB; largest block {} "
            "KiB; free memory {} KiB, CDRAM {} KiB, phycont {} KiB; {} threads (command)",
            static_cast<unsigned>(m.uordblks >> 10), static_cast<unsigned>(m.fordblks >> 10),
            static_cast<unsigned>(m.arena >> 10), _newlib_heap_size_user >> 10, low >> 10,
            static_cast<unsigned>(free.size_user) >> 10,
            static_cast<unsigned>(free.size_cdram) >> 10,
            static_cast<unsigned>(free.size_phycont) >> 10,
            static_cast<unsigned>(Common::RunningNamedThreads()));
        return true;
    }
    if (command == "heapmap") {
        PrintHeapMap();
        return true;
    }
    if (command == "heapsites") {
        VitaFrontend::CrashReport::PrintHeapSites(false);
        return true;
    }
#endif
    if (command.rfind("rm ", 0) == 0) {
        // `rm PATH`: a file on the console removed (a title's savestate slots, its .cst files).
        const std::string path = command.substr(3);
        LOG_INFO(Frontend, "rm {}: {:08x} (command)", path,
                 static_cast<unsigned>(sceIoRemove(path.c_str())));
        return true;
    }
    if (command.rfind("copy ", 0) == 0 && command.find('|') != std::string::npos) {
        // `copy SRC|DST`: a file copied on the console, its directories made (a player's game
        // save from host0 into the title's save data; paths past ten levels fold as the
        // emulator's own do). '|' parts them: save data paths hold spaces ("Nintendo 3DS").
        const std::size_t bar = command.find('|');
        const std::string from = command.substr(5, bar - 5);
        const std::string to = command.substr(bar + 1);
        // In pieces of 256 KiB with the stall watch beaten between them: a large copy from host0
        // outlasts the watch's limit.
        bool ok = FileUtil::CreateFullPath(to);
        if (ok) {
            FileUtil::IOFile in(from, "rb"), out(to, "wb");
            ok = in.IsOpen() && out.IsOpen();
            std::vector<u8> piece(256 * 1024);
            u64 copied = 0;
            while (ok) {
                const std::size_t got = in.ReadBytes(piece.data(), piece.size());
                if (got == 0) {
                    break;
                }
                ok = out.WriteBytes(piece.data(), got) == got;
                copied += got;
                VitaFrontend::CrashReport::StallWatchBeat();
            }
            ok = ok && copied == in.GetSize();
        }
        LOG_INFO(Frontend, "copy {} -> {}: {} (command)", from, to, ok ? "done" : "failed");
        return true;
    }
#ifdef AZAHAR_PGO_GENERATE
    if (command.rfind("pgo ", 0) == 0) {
        // `pgo PATH`: the profile's counts so far (AZAHAR_PGO=GENERATE) written to PATH, a .profraw
        // for llvm-profdata; the counting goes on. The runtime serialises it into a memory block of
        // its own (megabytes: not the heap's), written out in 1 MiB pieces, each checked.
        const std::string path = command.substr(4);
        const u64 size = __llvm_profile_get_size_for_buffer();
        const SceUID block = sceKernelAllocMemBlock(
            "azahar-pgo", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
            static_cast<SceSize>((size + 0xfff) & ~u64{0xfff}), nullptr);
        void* base = nullptr;
        if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0 ||
            __llvm_profile_write_buffer(static_cast<char*>(base)) != 0) {
            LOG_INFO(Frontend, "pgo: {} bytes not serialised (block {:08x}) (command)",
                     static_cast<unsigned long long>(size), static_cast<unsigned>(block));
            if (block >= 0) {
                sceKernelFreeMemBlock(block);
            }
            return true;
        }
        const SceUID fd = sceIoOpen(path.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        u64 written = 0;
        while (fd >= 0 && written < size) {
            const SceSize piece = static_cast<SceSize>(std::min<u64>(size - written, 1u << 20));
            const int put = sceIoWrite(fd, static_cast<const u8*>(base) + written, piece);
            if (put <= 0) {
                LOG_INFO(Frontend, "pgo: a write at {} failed: {:08x} (command)",
                         static_cast<unsigned long long>(written), static_cast<unsigned>(put));
                break;
            }
            written += static_cast<u64>(put);
        }
        if (fd >= 0) {
            sceIoClose(fd);
        }
        sceKernelFreeMemBlock(block);
        LOG_INFO(Frontend, "pgo: {}, {} of {} bytes (command)", path,
                 static_cast<unsigned long long>(written), static_cast<unsigned long long>(size));
        return true;
    }
#endif
    if (command.rfind("copy ", 0) == 0) {
        // `copy SRC DST`: a file copied on the console, in 1 MiB pieces (a ROM from host0 onto
        // the memory card, so that its title's reads do not cross the debug link).
        char src[256] = "", dst[256] = "";
        std::sscanf(command.c_str(), "copy %255s %255s", src, dst);
        for (char* slash = std::strchr(dst, '/'); slash != nullptr;
             slash = std::strchr(slash + 1, '/')) {
            *slash = '\0';
            sceIoMkdir(dst, 0777);
            *slash = '/';
        }
        const SceUID in = sceIoOpen(src, SCE_O_RDONLY, 0);
        const SceUID out =
            in < 0 ? -1 : sceIoOpen(dst, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
        if (in < 0 || out < 0) {
            LOG_INFO(Frontend, "copy: {} {:08x}, {} {:08x} (command)", src,
                     static_cast<unsigned>(in), dst, static_cast<unsigned>(out));
            if (in >= 0) {
                sceIoClose(in);
            }
            return true;
        }
        static std::array<u8, 1024 * 1024> piece;
        u64 copied = 0;
        int got;
        while ((got = sceIoRead(in, piece.data(), piece.size())) > 0) {
            if (const int put = sceIoWrite(out, piece.data(), got); put != got) {
                // 0x8001001c: ENOSPC, the card full.
                LOG_INFO(Frontend, "copy: a write to {} failed at {}: {:08x} (command)", dst,
                         static_cast<unsigned long long>(copied), static_cast<unsigned>(put));
                break;
            }
            copied += static_cast<u64>(got);
            VitaFrontend::CrashReport::StallWatchBeat();
            if (copied % (64u << 20) == 0) {
                LOG_INFO(Frontend, "copy: {} MiB (command)",
                         static_cast<unsigned long long>(copied >> 20));
            }
        }
        sceIoClose(out);
        sceIoClose(in);
        LOG_INFO(Frontend, "copy: {} to {}, {} bytes{} (command)", src, dst,
                 static_cast<unsigned long long>(copied), got < 0 ? ", a read failed" : "");
        return true;
    }
#ifdef VITA_DIAGNOSTICS
    if (command.rfind("tone ", 0) == 0) {
        // `tone HZ SECONDS`: a sine on an audio port of its own (a second BGM port beside the
        // title's), not through the emulator: what a recording has it as checks the
        // capture's rate against the console's.
        int hz = 0, seconds = 0;
        std::sscanf(command.c_str(), "tone %d %d", &hz, &seconds);
        if (hz <= 0 || hz > 20000 || seconds <= 0 || seconds > 30) {
            LOG_INFO(Frontend,
                     "tone: wants HZ (1-20000) SECONDS (1-30, under the stall watch) (command)");
            return true;
        }
        constexpr int Rate = 48000;
        constexpr int Frames = 1024;
        const int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, Frames, Rate,
                                             SCE_AUDIO_OUT_MODE_STEREO);
        if (port < 0) {
            LOG_INFO(Frontend, "tone: no port ({:08x}) (command)", static_cast<unsigned>(port));
            return true;
        }
        int volume[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
        sceAudioOutSetVolume(port, static_cast<SceAudioOutChannelFlag>(
                                       SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH),
                             volume);
        LOG_INFO(Frontend, "tone: {} Hz for {} s at {} Hz (command)", hz, seconds, Rate);
        static std::array<s16, Frames * 2> buffer;
        double phase = 0.0;
        const double step = 2.0 * M_PI * hz / Rate;
        for (int n = 0; n < seconds * Rate / Frames; n++) {
            for (int i = 0; i < Frames; i++) {
                const s16 v = static_cast<s16>(8000.0 * std::sin(phase));
                buffer[i * 2] = v;
                buffer[i * 2 + 1] = v;
                phase += step;
            }
            phase = std::fmod(phase, 2.0 * M_PI);
            sceAudioOutOutput(port, buffer.data());
        }
        sceAudioOutOutput(port, nullptr);
        sceAudioOutReleasePort(port);
        LOG_INFO(Frontend, "tone: done (command)");
        return true;
    }
    if (command == "cores") {
        // What each core runs now (azaharCoreSample, the stall report's for a stopped core).
        for (u32 core = 0; core < 4; core++) {
            AzaharCoreSample cs{};
            const int got = azaharCoreSample(core, &cs);
            cs.thread_name[sizeof(cs.thread_name) - 1] = '\0';
            cs.module_name[sizeof(cs.module_name) - 1] = '\0';
            LOG_INFO(Frontend,
                     "cores: core {}: {:08x}, pid {:08x} thread {:08x} \"{}\"{}, PC in {}+{:x}, "
                     "PCs {:08x} {:08x} {:08x} {:08x} (command)",
                     static_cast<unsigned>(core), static_cast<unsigned>(got),
                     static_cast<unsigned>(cs.pid), static_cast<unsigned>(cs.thid), cs.thread_name,
                     cs.idle ? " (idle)" : "", cs.module_name[0] ? cs.module_name : "?",
                     static_cast<unsigned>((cs.pcs[0] & ~1u) - cs.module_text),
                     static_cast<unsigned>(cs.pcs[0]), static_cast<unsigned>(cs.pcs[1]),
                     static_cast<unsigned>(cs.pcs[2]), static_cast<unsigned>(cs.pcs[3]));
        }
        VitaFrontend::CrashReport::PrintBusErrorDump(false);
        VitaFrontend::CrashReport::PrintTableScan(false);
        return true;
    }
#endif
    if (command == "stalltest") {
        // The stall watch's whole path on purpose: the loop that reads this channel stops for a
        // minute, past the watch's limit (its listing, the core dump, the game list again).
        sceClibPrintf("[azahar] stalltest: the frontend's loop stops for 60 s (command)\n");
        sceKernelDelayThread(60 * 1000 * 1000);
        return true;
    }
    if (command == "crashtest assert" || command == "crashtest abort") {
        // A crash on purpose, as a failed assert or an abort ends: the report, the core dump,
        // the game list again (CrashReport::Recover).
        sceClibPrintf("[azahar] %s (command)\n", command.c_str());
        if (command == "crashtest abort") {
            std::abort();
        }
        UNREACHABLE_MSG("crashtest (command)");
    }
    if (command == "crashtest render") {
        // A failed assert on the render thread with a title's frames on the GPU: where a GPU
        // stop ends (sgxd's Fault).
        auto& system = Core::System::GetInstance();
        if (!system.IsPoweredOn()) {
            LOG_INFO(Frontend, "crashtest render: no title runs (command)");
            return true;
        }
        sceClibPrintf("[azahar] %s (command)\n", command.c_str());
        system.GPU().GetRenderThread().Run([] { UNREACHABLE_MSG("crashtest render (command)"); });
        return true;
    }
    if (command == "straytest" || command == "straytest park") {
        // A data abort in the module's own code in the next slice (azaharStrayTest): the slice
        // ends and the title with it; `park` parks its core instead, for the stall watch.
        const int set = azaharStrayTest(command == "straytest" ? 1u : 2u);
        sceClibPrintf("[azahar] %s: %08x (command)\n", command.c_str(), static_cast<unsigned>(set));
        return true;
    }
    if (command == "hangtest") {
        // A GPU hang on purpose with a title's frames on the GPU: the next pass's 3D never ends
        // (SgxdBackend::DrawHang), then the stop, the fault's report and the recovery from it.
        if (!Core::System::GetInstance().IsPoweredOn()) {
            LOG_INFO(Frontend, "hangtest: no title runs (command)");
            return true;
        }
        sceClibPrintf("[azahar] hangtest (command)\n");
        SgxdRenderer::SgxdBackend::hang_test = true;
        return true;
    }
    if (command == "coredumptest") {
        // A core dump of the running process (azaharCoredump), timed: what a caught crash
        // writes before the app goes back to its game list. The loop goes on after it.
        char path[256] = {};
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        const int result = VitaFrontend::CrashReport::Coredump(path, sizeof(path));
        sceClibPrintf("[azahar] coredumptest: %08x after %llu ms, \"%s\" (command)\n",
                      static_cast<unsigned>(result),
                      (sceKernelGetProcessTimeWide() - start) / 1000, path);
        return true;
    }
#ifdef VITA_DIAGNOSTICS
    if (command == "hostthreads") {
        VitaFrontend::CrashReport::PrintThreadStates("asked (command)");
        return true;
    }
    if (command == "reents") {
        PrintReentSlots();
        return true;
    }
    if (command == "heapcheck" || command == "heapcheck full") {
        // The heap checked at the DSP's and the audio's steps and each guest frame
        // (Common::HeapCheck), until the first damage stops the process.
        Common::HeapCheck::full.store(command == "heapcheck full", std::memory_order_relaxed);
        Common::HeapCheck::enabled.store(true, std::memory_order_relaxed);
        LOG_INFO(Frontend, "heapcheck: on{} (command)",
                 command == "heapcheck full" ? ", every chunk" : ", the bins");
        return true;
    }
    if (command == "heapcheck stats") {
        Common::HeapCheck::PrintStats();
        return true;
    }
    if (command == "leaktrack") {
        VitaFrontend::CrashReport::StartLeakTracking();
        return true;
    }
    if (command == "leaks" || command == "leaks all") {
        VitaFrontend::CrashReport::PrintLeaks(command == "leaks all");
        return true;
    }
#endif
    // `name TEXT`: the console name, as the settings set it (kept, the file written at once); a
    // title takes it as it starts, so the next title launched reads it. `name` alone clears it
    // (the console's own).
    if (command == "name" || command.starts_with("name ")) {
        Config::SetConsoleName(command.size() > 5 ? command.substr(5) : std::string{});
        Config::Save();
        LOG_INFO(Frontend, "console name \"{}\" (command)", Config::ConsoleName());
        return true;
    }
    // As the launch arguments, pins over the settings file's values (Config::Pin).
    int value = 0;
    if (std::sscanf(command.c_str(), "limit %d", &value) == 1) {
        Config::Pin(Option::SpeedLimit, value);
        LOG_INFO(Frontend, "speed limit {}% (command)", value);
        return true;
    }
    if (std::sscanf(command.c_str(), "vsync %d", &value) == 1) {
        Config::Pin(Option::Vsync, value != 0 ? 1 : 0);
        LOG_INFO(Frontend, "vsync {} (command)", value != 0 ? "on" : "off");
        return true;
    }
    // The top screen's scale, as `scale=` at launch: 1, 1.25, 1.5, 1.75 or 2.
    if (float scale = 0.0f; std::sscanf(command.c_str(), "scale %f", &scale) == 1) {
        Config::Pin(Option::TopScale, Config::TopScaleValue(scale));
        LOG_INFO(Frontend, "top screen scale {:.2f}x (command)", static_cast<double>(scale));
        return true;
    }
    return false;
}
#endif // VITA_DEBUG_TOOLS

#ifdef VITA_DIAGNOSTICS
/// `memfind V` or `memfind LO-HI`: the running title's writable memory searched for V, or for a
/// value from LO to HI (one that changes as it is read: a cheat's address): the first search
/// lists every aligned u16, u32 and f32 that matches, each search after keeps those that match
/// the new one; `memfind reset` starts over. On the console, page by page, so that no copy of
/// guest memory crosses the debug link.
struct MemfindHit {
    VAddr addr;
    u8 kind; // 0 u16, 1 u32, 2 f32
};
std::vector<MemfindHit> memfind_hits;
bool memfind_listed = false;

void Memfind(Core::System& system, const std::string& argument) {
    if (argument == "reset") {
        memfind_hits.clear();
        memfind_listed = false;
        LOG_INFO(Frontend, "memfind: reset");
        return;
    }
    const auto process = system.Kernel().GetCurrentProcess();
    if (!process) {
        return;
    }
    char* end = nullptr;
    const double lo = std::strtod(argument.c_str(), &end);
    const double hi = *end == '-' ? std::strtod(end + 1, nullptr) : lo;
    const auto matches = [lo, hi](const u8* at, u8 kind) {
        double v;
        if (kind == 0) {
            u16 x;
            std::memcpy(&x, at, 2);
            v = x;
        } else if (kind == 1) {
            u32 x;
            std::memcpy(&x, at, 4);
            v = x;
        } else {
            float x;
            std::memcpy(&x, at, 4);
            // One value: the float nearest it (96.1 is no float).
            if (lo == hi) {
                return x == static_cast<float>(lo);
            }
            v = x;
        }
        return v >= lo && v <= hi;
    };
    constexpr std::size_t Most = 1 << 20;
    std::array<u8, Memory::CITRA_PAGE_SIZE> page{};
    if (!memfind_listed) {
        u32 scanned = 0;
        for (const auto& [base, vma] : process->vm_manager.vma_map) {
            if (vma.type == Kernel::VMAType::Free ||
                (static_cast<u8>(vma.permissions) & static_cast<u8>(Kernel::VMAPermission::Write)) ==
                    0) {
                continue;
            }
            for (VAddr at = base; at < base + vma.size && memfind_hits.size() < Most;
                 at += Memory::CITRA_PAGE_SIZE) {
                system.Memory().ReadBlock(*process, at, page.data(), page.size());
                scanned++;
                for (u32 o = 0; o < page.size(); o += 2) {
                    if (matches(page.data() + o, 0)) {
                        memfind_hits.push_back({at + o, 0});
                    }
                    if (o % 4 == 0) {
                        for (u8 kind = 1; kind < 3; kind++) {
                            if (matches(page.data() + o, kind)) {
                                memfind_hits.push_back({at + o, kind});
                            }
                        }
                    }
                }
            }
        }
        memfind_listed = true;
        LOG_INFO(Frontend, "memfind {}: {} pages, {} hits{}", argument, scanned,
                 static_cast<unsigned>(memfind_hits.size()),
                 memfind_hits.size() >= Most ? " (the most kept)" : "");
    } else {
        std::erase_if(memfind_hits, [&](const MemfindHit& hit) {
            u8 bytes[4]{};
            system.Memory().ReadBlock(*process, hit.addr, bytes, hit.kind == 0 ? 2 : 4);
            return !matches(bytes, hit.kind);
        });
        LOG_INFO(Frontend, "memfind {}: {} hits left", argument,
                 static_cast<unsigned>(memfind_hits.size()));
    }
    static constexpr const char* Kinds[] = {"u16", "u32", "f32"};
    for (std::size_t i = 0; i < memfind_hits.size() && i < 24; i++) {
        LOG_INFO(Frontend, "memfind:   {:08x} {}", static_cast<unsigned>(memfind_hits[i].addr),
                 Kinds[memfind_hits[i].kind]);
    }
}

/// The `threads` command: every guest thread's state, registers and what it waits on (a hang
/// with every thread blocked names its waits).
void LogGuestThreads(Core::System& system) {
    auto& kernel = system.Kernel();
    for (u32 core = 0; core < system.GetNumCores(); core++) {
        for (const auto& thread : kernel.GetThreadManager(core).GetThreadList()) {
            const auto& r = thread->context.cpu_registers;
            std::string waits;
            for (const auto& object : thread->wait_objects) {
                waits += fmt::format(" {} {} \"{}\"", object->GetTypeName(), object->GetObjectId(),
                                     object->GetName());
            }
            // An IPC wait's request header, from the thread's command buffer.
            u32 header = 0;
            if (thread->status == Kernel::ThreadStatus::WaitIPC &&
                thread->owner_process.lock() == kernel.GetCurrentProcess()) {
                header = system.Memory().Read32(thread->GetCommandBufferAddress());
            }
            LOG_INFO(Frontend,
                     "thread {} \"{}\" core {}: status {}, priority {}, pc {:08x} lr {:08x} sp "
                     "{:08x} r0 {:08x}, arbiter {:08x}, ipc header {:08x}, timeout {:d};{}",
                     thread->thread_id, thread->name, core, static_cast<int>(thread->status),
                     thread->current_priority, r[15], r[14], r[13], r[0], thread->wait_address,
                     header, thread->wakeup_queued, waits);
            LOG_INFO(Frontend,
                     "thread {}: r1 {:08x} r2 {:08x} r3 {:08x} r4 {:08x} r5 {:08x} r6 {:08x} r7 "
                     "{:08x} r8 {:08x} r9 {:08x} r10 {:08x} r11 {:08x} r12 {:08x} cpsr {:08x}",
                     thread->thread_id, r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10],
                     r[11], r[12], thread->context.cpsr);
        }
    }
}
#endif

/// A savestate's save or load refused or failed with the title still running: what to do next.
std::string StateErrorText(bool load, u32 slot) {
    if (!load) {
        return TrFormat("Slot {} was not saved. Make sure the memory card has free space, then save "
                        "again.",
                        slot);
    }
    if (VitaFrontend::Multiplayer::InRoom()) {
        return TrFormat("Slot {} can't be loaded in a multiplayer room. Leave the room, then load "
                        "it again.",
                        slot);
    }
    return TrFormat("Slot {} could not be loaded. Save to it again, or load another slot.", slot);
}

/// A load that stopped partway, which stops the title: the game list's notice.
std::string PartialLoadNotice(u32 slot) {
    return TrFormat("Slot {} could not be loaded, so the game was stopped. Start the game again, "
                    "and save to that slot again before you load it.",
                    slot) +
           "\n\n" + Tr("Press X or O to continue.");
}

#ifdef VITA_DEBUG_TOOLS
/**
 * Restores a savestate slot before the title runs. The signal executes at a quiescent point
 * inside RunLoop, so it gets a few slices. A state from another build is taken: the archive
 * layout is what has to match, not the revision.
 */
void RestoreStateAtStart(Core::System& system, u32 slot) {
    system.SetLoadStateAnyBuild(true);
    if (!system.SendSignal(Core::System::Signal::Load, slot)) {
        LOG_INFO(Frontend, "savestate: could not queue a load of slot {}", slot);
        return;
    }
    LOG_INFO(Frontend, "savestate: restoring slot {}", slot);
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
            LOG_INFO(Frontend, "savestate: load of slot {} failed: {} ({})", slot,
                     static_cast<int>(status), system.GetStatusDetails());
            break;
        }
        if (!system.IsSaveStateRequestPending()) {
            LOG_INFO(Frontend, "savestate: slot {} restored", slot);
            VitaFrontend::CrashReport::NoteStateLoaded(slot);
            break;
        }
    }
    Settings::values.deterministic_async_operations = async_before;
    if (system.IsSaveStateRequestPending()) {
        LOG_INFO(Frontend, "savestate: slot {} never became loadable", slot);
    }
}
#endif

/**
 * A suspend while sgxd holds the GPU (docs/sgxd/takeover.md). The title is paused, the GPU's
 * queue stopped and waited for, and the GPU given back (SuspendGpu); every thread's sgxd call
 * waits from then on. A standby asks for it and waits (StandbyThread); the power callback asks
 * too, for a display turned off with no standby after it. A retail console turns its display
 * off first (the bit the SDK names APP_SUSPEND), also after a while without input; the display
 * coming on again is APP_RESUMING. From the suspend to the resume the GPU is the system's (the
 * lock screen draws with it); it is taken again once SceAppMgr no longer reports the system's
 * UI over the app (PowerThread).
 */
std::atomic<bool> power_suspended{false}, power_resumed{false};
/// The window whose pause menu a suspend opens over a running title (EmulatorMain's).
std::atomic<VitaFrontend::EmuWindowVita*> power_window{nullptr};

constexpr int PowerSuspend = SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_APP_SUSPEND;
constexpr int PowerResume =
    SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_APP_RESUMING;

/// One hand-back or take at a time: the standby's, the power callback's and the take after the
/// resume.
Common::CoreCondition& PowerLock() {
    static Common::CoreCondition lock;
    return lock;
}

/// The GPU given back for a suspend, once. A running title is paused first, as its pause menu
/// leaves it: the run loop parked and only the menu drawing. Not once the system's suspend has
/// taken the GPU (the app late for the standby), when the menu could not draw.
void SuspendGpu() {
    std::scoped_lock lk{PowerLock()};
    if (power_suspended || !VitaFrontend::SgxdPresent::IsInitialized()) {
        return;
    }
    VitaFrontend::EmuWindowVita* const window = power_window.load();
    if (window != nullptr && VitaFrontend::SgxdPresent::Owned()) {
        const bool paused = window->PauseForSuspend(std::chrono::seconds(1));
        sceClibPrintf("[azahar] suspend: the title %s\n",
                      paused ? "paused" : "not paused (none running, or the timeout)");
    }
    VitaFrontend::SgxdPresent::Suspend();
    power_suspended = true;
}

int OnPower(int, int, int arg, void*) {
#ifdef VITA_DIAGNOSTICS
    // The callbacks' bits as they change, with the process time: which a power press sends, in
    // what order, and whether the standby waits for this callback.
    static int last_arg = -1;
    if (arg != last_arg) {
        sceClibPrintf("[azahar] power callback %08x at %llu us\n", static_cast<unsigned>(arg),
                      static_cast<unsigned long long>(sceKernelGetProcessTimeWide()));
        last_arg = arg;
    }
#endif
    if (arg & PowerSuspend) {
        SuspendGpu();
        power_resumed = false;
    }
    if ((arg & PowerResume) && power_suspended) {
        power_resumed = true;
        VitaFrontend::CrashReport::NoteResume();
    }
    return 0;
}

// The frontend's loop waits at sgxd's gate from the suspend to the take after it: the stall
// watch is told it lives (it would take the wait for a hang and end the process).
//
// After the resume the take waits until SceAppMgr no longer reports the system's UI over the app
// (isSystemUiOverlaid: its impose state or power save mode). In power
// save mode SceAppMgr gives the pad and the touch panel to the system and leaves the app's display
// plane without an owner; SceShell turns the mode on as the display goes off and off once its lock
// screen is gone. SCE_APPMGR_SYSTEMEVENT_ON_RESUME is no sign of either.
// The flag clears a little before the shell blanks its display plane, whose memory would show
// the app's data: so the take waits for that plane as well.
int PowerThread(SceSize, void*) {
    const SceUID callback = sceKernelCreateCallback("AzaharPower", 0, OnPower, nullptr);
    ASSERT_MSG(callback >= 0 && scePowerRegisterCallback(callback) >= 0,
               "the power callback was not registered");
    bool plane_waited = false;
    for (;;) {
        sceKernelDelayThreadCB(100 * 1000);
#ifdef VITA_DIAGNOSTICS
        // SceAppMgr's view of the app as it changes, and its system events: what tells, without
        // a press, that the app is in front again after a resume (the lock screen gone).
        {
            static SceAppMgrAppState last{};
            static int last_got = 1;
            SceAppMgrAppState state{};
            const int got = _sceAppMgrGetAppState(&state, sizeof(state), 0);
            if (got != last_got || state.isSystemUiOverlaid != last.isSystemUiOverlaid ||
                state.systemEventNum != last.systemEventNum ||
                state.appEventNum != last.appEventNum) {
                sceClibPrintf("[azahar] app state %08x: overlaid %d, system events %u, app events "
                              "%u (suspended %d, resumed %d) at %llu us\n",
                              static_cast<unsigned>(got), state.isSystemUiOverlaid,
                              state.systemEventNum, state.appEventNum, power_suspended.load(),
                              power_resumed.load(),
                              static_cast<unsigned long long>(sceKernelGetProcessTimeWide()));
                last = state;
                last_got = got;
            }
            if (got >= 0 && state.systemEventNum != 0) {
                SceAppMgrSystemEvent event{};
                while (sceAppMgrReceiveSystemEvent(&event) >= 0) {
                    sceClibPrintf("[azahar] system event %08x at %llu us\n",
                                  static_cast<unsigned>(event.systemEvent),
                                  static_cast<unsigned long long>(sceKernelGetProcessTimeWide()));
                    event = {};
                }
            }
        }
#endif
        if (power_suspended) {
            VitaFrontend::CrashReport::StallWatchBeat();
        }
        if (!power_resumed) {
            continue;
        }
        SceAppMgrAppState state{};
        const int got = _sceAppMgrGetAppState(&state, sizeof(state), 0);
        ASSERT_MSG(got == 0, "SceAppMgr's app state was not read after the resume ({:#010x})",
                   static_cast<u32>(got));
        if (state.isSystemUiOverlaid) {
            continue;
        }
        if (VitaFrontend::ShellPlaneShown()) {
            if (!plane_waited) {
                sceClibPrintf("[azahar] after the resume: the system's UI off the app, its plane "
                              "still up\n");
                plane_waited = true;
            }
            continue;
        }
        plane_waited = false;
        sceClibPrintf("[azahar] after the resume: the system's UI off the app, the GPU taken "
                      "again\n");
        std::scoped_lock lk{PowerLock()};
        std::string error;
        const bool resumed = VitaFrontend::SgxdPresent::Resume(&error);
        ASSERT_MSG(resumed, "sgxd: after the resume: {}", error);
        sceClibPrintf("[azahar] the GPU is ours again after the resume\n");
        power_suspended = false;
        power_resumed = false;
    }
    return 0;
}

// A standby's ask for the GPU (sgxd's handler of (0, 0x20f), before SceAppMgr stops the app): the
// system's suspend waits until it is given back, 3 s at most. At the emulation and render threads'
// priority (the highest an application's thread has), on any core: the default one waits behind
// them.
int StandbyThread(SceSize, void* arg) {
    const SceUID flag = *static_cast<const SceUID*>(arg);
    for (;;) {
        u32 bits = 0;
        const int waited =
            sceKernelWaitEventFlag(flag, 1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR, &bits, nullptr);
        ASSERT_MSG(waited == 0, "the standby's flag wait failed ({:#010x})",
                   static_cast<u32>(waited));
        const SceUInt64 t0 = sceKernelGetProcessTimeWide();
        sceClibPrintf("[azahar] standby: the GPU asked for\n");
        SuspendGpu();
        sceClibPrintf("[azahar] standby: answered in %llu ms\n",
                      static_cast<unsigned long long>((sceKernelGetProcessTimeWide() - t0) / 1000));
    }
    return 0;
}

/// A crash's notice (g_crash_notice): what happened, and the files a report wants.
std::string CrashNotice(std::string_view what, std::string_view report, std::string_view dump) {
    std::string text{what};
    text.append("\n\n")
        .append(dump.empty() ? Tr("To report it, send this file:")
                             : Tr("To report it, send these files:"))
        .append("\n")
        .append(report);
    if (!dump.empty()) {
        text.append("\n").append(dump);
    }
    return text.append("\n\n").append(Tr("Press X or O to continue."));
}

/// A crash's first line, and with the title's settings not all at their defaults
/// (Config::WriteChangedFromDefaults's `keys`), the advice to try it with them first, naming them,
/// their defaults and their screen as the settings do.
std::string CrashWhat(const VitaFrontend::Ui& ui, const char* what, std::string_view keys) {
    std::string text{what};
    std::string names;
    for (const auto& [option, own] : Config::OptionsOfKeys(keys)) {
        const std::string name = ui.SettingTitle(option);
        const std::string value = ui.SettingDefault(option);
        names.append(names.empty() ? "" : "; ")
            .append(own ? TrFormat("{} to {}, in Game settings", name, value)
                        : TrFormat("{} to {}, in Settings", name, value));
    }
    if (!names.empty()) {
        text.append("\n\n").append(
            TrFormat("This game does not use the default settings. Before you report the crash, "
                     "set these back to their defaults and try again: {}",
                     names));
    }
    return text;
}

/// The console name the player chose (Config::ConsoleName) into the config save, as the 3DS's own
/// settings write it: its user name, which games read through CFG. After the load made CFG's
/// module, before the title's first instruction; the save is written only where the name differs.
void ApplyConsoleName(Core::System& system) {
    // The name set, or with none the console's own: the name its config save held before the
    // first one written (Config::ConsoleOwnName), brought back once the setting is cleared.
    // Never set, the save is left as it is.
    const auto cfg = Service::CFG::GetModule(system);
    ASSERT_MSG(cfg != nullptr, "console name: no CFG module after the load");
    const std::string set = Config::ConsoleName();
    const std::string own = Config::ConsoleOwnName();
    std::string name;
    if (!set.empty()) {
        name = set;
        if (own.empty() && cfg->GetUsername() != Common::UTF8ToUTF16(set)) {
            const std::string current = Common::UTF16ToUTF8(cfg->GetUsername());
            Config::SetConsoleOwnName(current.empty() ? std::string{"AZAHAR"} : current);
            Config::Save();
        }
    } else if (!own.empty()) {
        name = own;
        Config::SetConsoleOwnName({});
        Config::Save();
    } else {
        return;
    }
    const std::u16string wanted = Common::UTF8ToUTF16(name);
    if (cfg->GetUsername() == wanted) {
        return;
    }
    cfg->SetUsername(wanted);
    const Result written = cfg->UpdateConfigNANDSavegame();
    LOG_INFO(Frontend, "console name {}: {}", name,
             written.IsSuccess() ? "written" : "not written to the config save");
}

/// The system language the player chose (Config::Option::SystemLanguage) into the config save, as
/// the 3DS's own settings write it. Like ApplyConsoleName, before the title's first instruction,
/// and before anything reads the language: with the region on auto the first read picks the
/// region from it (CFG's UpdatePreferredRegionCode). The save is written only where it differs.
void ApplySystemLanguage(Core::System& system) {
    const auto cfg = Service::CFG::GetModule(system);
    ASSERT_MSG(cfg != nullptr, "system language: no CFG module after the load");
    const auto wanted = static_cast<Service::CFG::SystemLanguage>(
        Config::Effective(Config::Option::SystemLanguage));
    if (cfg->GetRawSystemLanguage() == wanted) {
        return;
    }
    cfg->SetSystemLanguage(wanted);
    const Result written = cfg->UpdateConfigNANDSavegame();
    LOG_INFO(Frontend, "system language {}: {}", static_cast<int>(wanted),
             written.IsSuccess() ? "written" : "not written to the config save");
}

/// A crash's files waiting for their archive and notice (ArchivedCrashNotice): Recover's restart
/// sets them (crashed=PATH), for the game list to pack where it can show the progress.
struct CrashFiles {
    std::string what, report, dump;
};
std::optional<CrashFiles> g_crash_files;

/// The crash's report and dump (none: the report alone) packed into one archive
/// (CrashReport::ArchiveCrash), its progress on the status panel (`present` draws a frame), and the
/// notice naming the archive: or the files themselves where it could not be written.
std::string ArchivedCrashNotice(VitaFrontend::Ui& ui, const std::function<void()>& present,
                                const CrashFiles& files) {
    // This run's own report (a game's crash), packed away or named by the notice: the next
    // crash's report starts afresh, with its own preamble.
    const bool live = files.report == Common::Log::CrashReportPath;
    ui.SetStatusMessage(Tr("Packing the crash report..."));
    ui.SetProgress(0.0f);
    present();
    u64 shown = 0;
    const std::string archive = VitaFrontend::CrashReport::ArchiveCrash(
        files.report, files.dump, [&](u64 done, u64 total) {
            // Alive: a full dump takes about a minute to pack, past the stall watch's limit.
            VitaFrontend::CrashReport::StallWatchBeat();
            const u64 now = sceKernelGetProcessTimeWide();
            if (now - shown < 100'000 || total == 0) {
                return;
            }
            shown = now;
            ui.SetProgress(static_cast<float>(done) / static_cast<float>(total));
            present();
        });
    ui.ClearStatusMessage();
    if (live) {
        Common::Log::EndCrashReport(!archive.empty());
    }
    return archive.empty() ? CrashNotice(files.what, files.report, files.dump)
                           : CrashNotice(files.what, archive, "");
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

#ifdef VITA_DEBUG_TOOLS
/// The commands that only read the title's state: `dump PADDR SIZE` and `vdump VADDR SIZE` (its
/// memory to host0), `memfind`, `tables VADDR`, `pageinfo VADDR` (`tables` and the GPU's holds of
/// the page), `versions` (the versioned pages) and `threads`. The title's loop takes them, and so
/// does a crashed title's hold (launch argument crashhold), while the state is still there.
bool RunInspectCommand(Core::System& system, const std::string& command) {
    if (unsigned paddr = 0, size = 0;
        std::sscanf(command.c_str(), "dump %x %x", &paddr, &size) == 2) {
#ifdef VITA_DIAGNOSTICS
        // Guest memory as the host sees it (moved pages included), to
        // host0:azahar-dump-PADDR.bin.
        const u8* const at = system.Memory().GetPhysicalPointer(paddr);
        const auto name = fmt::format("host0:azahar-dump-{:08x}.bin", paddr);
        FILE* const out = at != nullptr ? std::fopen(name.c_str(), "wb") : nullptr;
        const std::size_t written = out ? std::fwrite(at, 1, size, out) : 0;
        if (out) {
            std::fclose(out);
        }
        LOG_INFO(Frontend, "dump {:08x}+{:x}: {} bytes to {}", paddr, size,
                 static_cast<unsigned>(written), name);
#else
        LOG_INFO(Frontend, "dump: a diagnostics build's command");
#endif
        return true;
    }
    if (u32 vaddr = 0, size = 0; std::sscanf(command.c_str(), "vdump %x %x", &vaddr, &size) == 2) {
#ifdef VITA_DIAGNOSTICS
        // The title's memory at a virtual address, to host0:azahar-vdump-VADDR.bin.
        const auto process = system.Kernel().GetCurrentProcess();
        std::vector<u8> bytes(process ? size : 0);
        if (process) {
            system.Memory().ReadBlock(*process, vaddr, bytes.data(), bytes.size());
        }
        const auto name = fmt::format("host0:azahar-vdump-{:08x}.bin", vaddr);
        FILE* const out = process ? std::fopen(name.c_str(), "wb") : nullptr;
        const std::size_t written = out ? std::fwrite(bytes.data(), 1, bytes.size(), out) : 0;
        if (out) {
            std::fclose(out);
        }
        LOG_INFO(Frontend, "vdump {:08x}+{:x}: {} bytes to {}", vaddr, size,
                 static_cast<unsigned>(written), name);
#else
        LOG_INFO(Frontend, "vdump: a diagnostics build's command");
#endif
        return true;
    }
    if (command.starts_with("memfind ")) {
#ifdef VITA_DIAGNOSTICS
        Memfind(system, command.substr(8));
#else
        LOG_INFO(Frontend, "memfind: a diagnostics build's command");
#endif
        return true;
    }
    if (command.starts_with("tables ")) {
        // What a guest fault's report says of an address (Core::ModuleTablesAt), from the
        // emulation thread's core (host 0's).
        const u32 va = static_cast<u32>(std::strtoul(command.c_str() + 7, nullptr, 16));
        for (const std::string& line : Core::ModuleTablesAt(va)) {
            LOG_INFO(Frontend, "{} (command)", line);
        }
        return true;
    }
    if (u32 va = 0; std::sscanf(command.c_str(), "pageinfo %x", &va) == 1) {
#ifdef VITA_DIAGNOSTICS
        // A guest address's page as the module's tables (the shared one and each core's view) and
        // the GPU's holds (its access, tickets and versions) have it.
        for (const std::string& line : Core::ModuleTablesAt(va)) {
            LOG_INFO(Frontend, "pageinfo {:08x}: {} (command)", va, line);
        }
        const u8* const host = system.Memory().GetPointer(va);
        LOG_INFO(Frontend, "pageinfo {:08x}: app view {}; {} (command)", va, fmt::ptr(host),
                 host != nullptr ? system.GPU().GetRenderThread().HoldState(host)
                                 : std::string{"not mapped"});
#else
        LOG_INFO(Frontend, "pageinfo: a diagnostics build's command");
#endif
        return true;
    }
    if (command == "versions") {
#ifdef VITA_DIAGNOSTICS
        LOG_INFO(Frontend, "versions: {} (command)", system.GPU().GetRenderThread().VersionList());
#else
        LOG_INFO(Frontend, "versions: a diagnostics build's command");
#endif
        return true;
    }
    if (command == "threads") {
#ifdef VITA_DIAGNOSTICS
        LogGuestThreads(system);
#else
        LOG_INFO(Frontend, "threads: a diagnostics build's command");
#endif
        return true;
    }
    return false;
}
#endif

bool RunTitle(Core::System& system, VitaFrontend::EmuWindowVita& window, VitaFrontend::Ui& ui,
              const std::string& path) {
    window.UpdateCurrentFramebufferLayout();

    ui.SetStatusMessage(Tr("Loading..."));
    ui.Update();
    window.Present(&ui);

    const std::size_t threads_before = Common::RunningNamedThreads();
    window.SetLoading(true);
    Config::ApplyForLoad(Config::TitleKeyOf(path));
    const auto load_result = system.Load(window, path);
    window.SetLoading(false);
    if (load_result != Core::System::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Could not load {}: {}", path, DescribeLoadError(load_result));
        ui.SetStatusMessage(
            TrFormat("Could not load this title: {}", Tr(DescribeLoadError(load_result))));
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
    // A game Azahar has speed patches for, in a copy they do not fit: the copy they are made for,
    // until the player closes it. A launch by path (a scripted launch) has it on the TTY alone.
    if (const std::string warning = system.GetAppLoader().TitlePatchesWarning();
        !warning.empty()) {
        LOG_INFO(Frontend, "{}", warning);
        if (!g_launched_by_path) {
            ui.SetStatusMessage(warning + "\n\n" + Tr("Press X or O to continue."));
            // The buttons let go, one pressed, let go again.
            for (int phase = 0; phase < 3;) {
                window.PollEvents();
                ui.Update();
                window.Present(&ui);
                sceDisplayWaitVblankStart();
                SceCtrlData pad{};
                const bool held = sceCtrlPeekBufferPositive(0, &pad, 1) > 0 &&
                                  (pad.buttons & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) != 0;
                if (held == (phase == 1)) {
                    phase++;
                }
            }
            ui.ClearStatusMessage();
        }
    }
    ApplyConsoleName(system);
    ApplySystemLanguage(system);
    if (u64 program_id = 0; system.GetAppLoader().ReadProgramId(program_id) ==
                            Loader::ResultStatus::Success) {
        Config::SeedTitleValues(Config::TitleKeyOf(path), program_id);
    }
    Config::SetRunningTitle(Config::TitleKeyOf(path));
    NoteRunningTitle(system);
    system.SetResetCallback([&system] {
        NoteRunningTitle(system);
        VitaFrontend::CrashReport::NoteJump();
    });
    VitaFrontend::CrashReport::NoteCheats(&system.CheatEngine());
    ui.SetGame(path);
    // nwm::UDS signals its events directly only on this thread; the room's thread queues them.
    system.RegisterCoreLoopThreadId();
#ifdef VITA_DEBUG_TOOLS
    StateArguments state_arguments = TakeStateArguments();
#endif
    window.UpdateCurrentFramebufferLayout(); // the title's own layout, or a layout=N argument's
    // The timing exists since Load, at the clock all games' settings had: the title's own or a
    // clock=N argument's from here, before any guest code runs.
    system.CoreTiming().UpdateClockSpeed(
        static_cast<u32>(Settings::values.cpu_clock_percentage.GetValue()));

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
    // A savestate's save or load: a panel with its progress on black, ten times a second. The
    // render thread presents it after the frames queued before (RunSync); in the load's restart,
    // with none running, this thread does. The load phases' shares are a measured load's: the file
    // read 3.0 s, the restart 1.9 s, the restore 3.4 s.
    u32 progress_slot = 0;
    u64 progress_shown = 0;
    system.SetStateProgressCallback([&](Core::System::StatePhase phase, float fraction) {
        using Phase = Core::System::StatePhase;
        const u64 now = sceKernelGetProcessTimeWide();
        if (now - progress_shown < 100'000) {
            return;
        }
        progress_shown = now;
        float overall = fraction;
        switch (phase) {
        case Phase::Saving:
            break;
        case Phase::Reading:
            overall = 0.36f * fraction;
            break;
        case Phase::Restarting:
            overall = 0.36f + 0.23f * fraction;
            break;
        case Phase::Restoring:
            overall = 0.59f + 0.41f * fraction;
            break;
        }
        ui.SetStatusMessage(phase == Phase::Saving ? TrFormat("Saving slot {}...", progress_slot)
                                                   : TrFormat("Loading slot {}...", progress_slot));
        ui.SetProgress(overall);
        // The present on the render thread, the GPU's owner, while a title runs (the save, a
        // load's read and restore, its restart's end); between a load's shutdown and its start
        // again there is none, and the present is this thread's, as the game list's.
        const auto present = [&] {
            ui.Update();
            window.PresentInterfaceOnly(&ui);
        };
        if (system.IsPoweredOn()) {
            system.GPU().GetRenderThread().RunSync(present);
        } else {
            present();
        }
    });
    // The callback holds this function's locals: gone with them, whichever way it returns.
    struct ProgressReset {
        Core::System& system;
        ~ProgressReset() {
            system.SetStateProgressCallback({});
        }
    } progress_reset{system};
#ifdef VITA_DEBUG_TOOLS
    if (state_arguments.load >= 0) {
        progress_slot = static_cast<u32>(state_arguments.load);
        window.SetLoading(true);
        RestoreStateAtStart(system, static_cast<u32>(state_arguments.load));
        window.SetLoading(false);
        ui.ClearStatusMessage();
        if (system.IsStateLoadPartial()) {
            LOG_INFO(Frontend, "savestate: the load stopped partway; the title is stopped");
            g_crash_notice = PartialLoadNotice(progress_slot);
            window.RequestQuit(false);
        }
    }
#endif
    // A failed save or load's text over the paused game, for three seconds.
    const auto show_state_error = [&](const std::string& text) {
        ui.SetStatusMessage(text);
        const auto present = [&] {
            ui.Update();
            window.PresentInterfaceOnly(&ui);
        };
        for (int i = 0; i < 180; i++) {
            system.GPU().GetRenderThread().RunSync(present);
            sceDisplayWaitVblankStart();
        }
        ui.ClearStatusMessage();
    };
#ifdef VITA_DEBUG_TOOLS
    // `save=N@T`: once the title has run T seconds (real time, from here).
    const SceUInt64 title_start_us = sceKernelGetProcessTimeWide();
    bool state_save_sent = false;
#endif
    // A save or load sent (the argument's or the menu's) whose result is still to print.
    bool state_watch = false;
    bool state_watch_is_load = false;
    // An exception in the title's code, or its read of memory it has not mapped.
    bool title_crashed = false;
    // With Multicore on at the launch: the seconds in a row the game has run without a new
    // frame, and whether the pause menu asked about Multicore (once a run).
#ifdef VITA_DEBUG_TOOLS
    const bool stuck_watch = system.MulticoreAtLoad() && stuck_check;
#else
    const bool stuck_watch = system.MulticoreAtLoad();
#endif
    u32 stuck_seconds = 0;
    bool stuck_asked = false;
    // The render thread's presents at the last stats second: its count starts over with each
    // title's GPU.
    u32 last_shown = system.GPU().GetRenderThread().ShownFrames();

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
            ShutdownSystem(system, threads_before);
            window.ForgetSwapToggle();
            Config::SetRunningTitle({});
            VitaFrontend::CrashReport::NoteCheats(nullptr);
            VitaFrontend::CrashReport::NoteGame({}, 0, {});
            Config::Save();
            // The menu's quit has left the interface on the game list already; the command
            // channel's has not, and the render thread that drew the title is gone by now.
            ui.LeaveGame();
            return action == VitaFrontend::Ui::Action::QuitGame;
        case VitaFrontend::Ui::Action::SaveState:
            progress_slot = ui.SelectedSlot();
            state_watch_is_load = false;
            state_watch = system.SendSignal(Core::System::Signal::Save, ui.SelectedSlot());
            LOG_INFO(Frontend, "savestate: saving slot {} from the menu", ui.SelectedSlot());
            break;
        case VitaFrontend::Ui::Action::LoadState:
            progress_slot = ui.SelectedSlot();
            state_watch_is_load = true;
            state_watch = system.SendSignal(Core::System::Signal::Load, ui.SelectedSlot());
            LOG_INFO(Frontend, "savestate: loading slot {} from the menu", ui.SelectedSlot());
            break;
        default:
            break;
        }

#ifdef VITA_DEBUG_TOOLS
        if (state_arguments.save >= 0 && !state_save_sent &&
            sceKernelGetProcessTimeWide() - title_start_us >= state_arguments.save_after_us) {
            state_save_sent = true;
            progress_slot = static_cast<u32>(state_arguments.save);
            state_watch_is_load = false;
            state_watch = system.SendSignal(Core::System::Signal::Save,
                                            static_cast<u32>(state_arguments.save));
            LOG_INFO(Frontend, "savestate: saving slot {} {}", state_arguments.save,
                     state_watch ? "queued" : "refused");
        }
        if (CommandChannel()) {
            if (const std::string command = command_file.Poll(); !command.empty()) {
                int slot = -1;
                const bool save = std::sscanf(command.c_str(), "save %d", &slot) == 1;
                const bool load = !save && std::sscanf(command.c_str(), "load %d", &slot) == 1;
                if ((save || load) && slot >= 0) {
                    system.SetLoadStateAnyBuild(true);
                    progress_slot = static_cast<u32>(slot);
                    state_watch_is_load = load;
                    state_watch = system.SendSignal(save ? Core::System::Signal::Save
                                                         : Core::System::Signal::Load,
                                                    static_cast<u32>(slot));
                    LOG_INFO(Frontend, "savestate: {} slot {} (command) {}",
                             save ? "saving" : "loading", slot, state_watch ? "queued" : "refused");
                } else if (RunInspectCommand(system, command)) {
                } else if (command == "menu quit") {
                    // As a player quits: the pause menu open a second, then its "Quit game".
                    LOG_INFO(Frontend, "menu quit (command)");
                    window.ScriptPauseMenu(static_cast<int>(VitaFrontend::Ui::Action::QuitGame));
                } else if (command == "quit" || command == "exit") {
                    // The next round of this loop shuts the title down, as after the menu's.
                    LOG_INFO(Frontend, "{} (command)", command);
                    window.RequestQuit(command == "exit");
                } else if (command == "crashtest guest") {
                    // The title stopped as a crash in its code stops it: the frontend's side
                    // (the core dump with the title's state, the game list's notice).
                    LOG_INFO(Frontend, "crashtest guest (command)");
                    title_crashed = true;
                    break;
                } else if (RunLayoutCommand(window, command) || RunSettingCommand(command) ||
                           VitaFrontend::Multiplayer::RunCommand(system, command) ||
                           VitaFrontend::Adhoc::RunCommand(system, command)) {
                } else {
                    LOG_INFO(Frontend, "command \"{}\" not known", command);
                }
            }
        }
#endif
        // A save or load sent and not done yet: the volume keys do nothing meanwhile.
        window.SetLoading(state_watch);
        const auto status = system.RunLoop();
        // The signal becomes a request inside RunLoop, which runs it once no HLE request is
        // in flight: the same call or a later one.
        if (state_watch && !system.IsSaveStateRequestPending()) {
            state_watch = false;
            window.SetLoading(false);
            ui.ClearStatusMessage();
            const bool ok = status == Core::System::ResultStatus::Success;
            LOG_INFO(Frontend, "savestate: {}{}",
                     ok ? "done" : "failed: ", ok ? "" : system.GetStatusDetails().c_str());
            if (ok && state_watch_is_load) {
                VitaFrontend::CrashReport::NoteStateLoaded(progress_slot);
            }
            if (!ok && !system.IsStateLoadPartial()) {
                show_state_error(StateErrorText(state_watch_is_load, progress_slot));
            }
        }
        if (status == Core::System::ResultStatus::ErrorSavestate && system.IsStateLoadPartial()) {
            // A load that failed partway through the file left a state the title cannot run on
            // (part the old one, part the file's): back to the game list.
            LOG_INFO(Frontend, "savestate: the load stopped partway; the title is stopped");
            LOG_CRITICAL(Frontend, "Savestate load stopped partway: {}", system.GetStatusDetails());
            g_crash_notice = PartialLoadNotice(progress_slot);
            window.RequestQuit(false);
        } else if (status == Core::System::ResultStatus::ErrorSavestate ||
                   status == Core::System::ResultStatus::ErrorSavestateBuildMismatch) {
            // A refused save or load is not fatal; the title keeps running. Another build's
            // state is refused before anything of it is read.
            LOG_ERROR(Frontend, "Savestate operation failed");
        } else if (status != Core::System::ResultStatus::Success) {
            LOG_CRITICAL(Frontend, "Emulation stopped: {}", static_cast<int>(status));
            title_crashed = status == Core::System::ResultStatus::ErrorCoreExceptionRaised ||
                            status == Core::System::ResultStatus::ErrorMemoryExceptionRaised;
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
                // The frames the render thread presented, not the vblanks (perf.system_fps):
                // dropped presents and skipped frames show nothing new.
                const u32 shown = system.GPU().GetRenderThread().ShownFrames();
                shown_fps.store(shown - last_shown, std::memory_order_relaxed);
#ifdef VITA_DEBUG_TOOLS
                if (stats_log) {
                    const u32 g10 = game_fps10.load(std::memory_order_relaxed);
                    // With the presents' gaps at three stages, each to 20 ms / 37 / 54 / longer.
                    // And how many refreshes apart the display took them (vsync on): 1 2 3 4+.
                    const auto p = SgxdRenderer::RenderThread::TakePaceGaps();
                    const auto f = VitaFrontend::SgxdPresent::TakeFlipGaps();
                    LOG_INFO(Frontend,
                             "stats: speed {}% game {}.{} fps shown {} fps; gaps handed {} {} {} "
                             "{} reached {} {} {} {} shown {} {} {} {} flips {} {} {} {}; paced waits {} {} {} "
                             "{}, queued {} {} {}",
                             speed_percent.load(std::memory_order_relaxed), g10 / 10, g10 % 10,
                             shown - last_shown, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                             p[8], p[9], p[10], p[11], f[0], f[1], f[2], f[3], f[4], f[5],
                             f[6], f[7], f[8], f[9], f[10]);
                    // The CPU time each thread ran in the interval (its own run clock): which one
                    // runs out of time when the game slows down.
                    static u64 last_emu = 0, last_render = 0, last_parse = 0;
                    SceKernelThreadInfo self{};
                    self.size = sizeof(self);
                    const u64 emu = sceKernelGetThreadInfo(sceKernelGetThreadId(), &self) >= 0
                                        ? static_cast<u64>(self.runClocks)
                                        : last_emu;
                    const u64 render = SgxdRenderer::RenderThread::ToolsRunUs(0);
                    const u64 parse = SgxdRenderer::RenderThread::ToolsRunUs(1);
                    static std::array<u64, 3> last_wait{};
                    std::array<u64, 3> wait{};
                    for (int k = 0; k < 3; k++) {
                        wait[k] = SgxdRenderer::RenderThread::ToolsEmuWaitUs(k);
                    }
                    LOG_INFO(Frontend,
                             "cpu: emulation {} ms, render {} ms, parse {} ms; emulation asleep for "
                             "the render thread: ticket {} ms, ring space {} ms, sync {} ms",
                             (emu - last_emu) / 1000, (render - last_render) / 1000,
                             (parse - last_parse) / 1000, (wait[0] - last_wait[0]) / 1000,
                             (wait[1] - last_wait[1]) / 1000, (wait[2] - last_wait[2]) / 1000);
                    last_wait = wait;
                    static std::array<u64, 5> last_rc{};
                    std::array<u64, 5> rc{};
                    for (int k = 0; k < 5; k++) {
                        rc[k] = SgxdRenderer::RenderThread::ToolsRenderCounter(k);
                    }
                    const u64 draws = rc[1] - last_rc[1];
                    LOG_INFO(Frontend,
                             "cpu: render thread running records {} ms; {} draws in {} ms ({} us "
                             "each); asleep for the GPU {} ms, for records {} ms",
                             (rc[0] - last_rc[0]) / 1000, draws, (rc[2] - last_rc[2]) / 1000,
                             draws != 0 ? (rc[2] - last_rc[2]) / draws : 0,
                             (rc[3] - last_rc[3]) / 1000, (rc[4] - last_rc[4]) / 1000);
                    last_rc = rc;
                    static u32 last_renders = 0;
                    const u32 renders = sgxd_session_renders();
#ifdef VITA_DIAGNOSTICS
                    // perflog has them by site.
                    const std::string sites;
#else
                    std::string sites = SgxdRenderer::SgxdBackend::ToolsWaitSites();
                    static std::array<u32, 3> last_geo{};
                    std::array<u32, 3> geo{};
                    for (int k = 0; k < 3; k++) {
                        geo[k] = SgxdRenderer::RasterizerSgxd::ToolsGeometryDraws(k);
                    }
                    sites += fmt::format("; geometry draws queued {}, alone without a bound {}, "
                                         "alone past the room {}; stream-out recipes {}",
                                         geo[0] - last_geo[0], geo[1] - last_geo[1],
                                         geo[2] - last_geo[2],
                                         SgxdRenderer::SgxdBackend::ToolsStreamRecipes());
                    last_geo = geo;
                    // Pass A1's draws and invocations by how they ran: side by side, in order (a
                    // program that reads a temporary first), side by side by the Parallel geometry
                    // option.
                    static std::array<u32, 6> last_order{};
                    std::array<u32, 6> order{};
                    for (int k = 0; k < 6; k++) {
                        order[k] = SgxdRenderer::RasterizerSgxd::ToolsGeometryOrder(k);
                    }
                    sites += fmt::format("; geometry parallel {} draws {} invocations, serial {} "
                                         "draws {} invocations, forced parallel {} draws {} "
                                         "invocations",
                                         order[0] - last_order[0], order[1] - last_order[1],
                                         order[2] - last_order[2], order[3] - last_order[3],
                                         order[4] - last_order[4], order[5] - last_order[5]);
                    last_order = order;
                    static std::array<u64, 2> last_geo_us{};
                    const std::array<u64, 2> geo_us{SgxdRenderer::RasterizerSgxd::ToolsGeometryUs(0),
                                                    SgxdRenderer::RasterizerSgxd::ToolsGeometryUs(1)};
                    sites += fmt::format("; geometry stage {} ms (programs {} ms)",
                                         (geo_us[0] - last_geo_us[0]) / 1000,
                                         (geo_us[1] - last_geo_us[1]) / 1000);
                    last_geo_us = geo_us;
#endif
                    LOG_INFO(Frontend, "cpu: GPU waits {} ms; renders {}{}",
                             sgxd_wait_us_take() / 1000, renders - last_renders, sites);
                    last_renders = renders;
                    last_emu = emu;
                    last_render = render;
                    last_parse = parse;
                }
#endif
                last_shown = shown;
                valid.store(true, std::memory_order_release);
                // Launched with Multicore on only: a game that runs on (its clock at half speed
                // or more) and shows no new frame for StuckSeconds. Code shared across the cores
                // with no barriers, which the 3DS's in-order cores never needed, can spin
                // forever on the Vita's. The pause menu says so, once.
                constexpr u32 StuckSeconds = 20;
                if (stuck_watch && !stuck_asked) {
                    const bool still = perf.game_fps < 0.5 && perf.emulation_speed > 0.5;
                    stuck_seconds = still ? stuck_seconds + 1 : 0;
                    if (stuck_seconds == StuckSeconds) {
                        stuck_asked = true;
                        LOG_WARNING(Frontend, "no new frame for {} s with Multicore on: asking",
                                    StuckSeconds);
                        ui.AskStuck();
                        window.OpenPauseMenuSoon();
                    }
                }
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
                    // The second emulation thread (emu_host.h): its passes and slices, its idles
                    // (and host 0's) with the time asleep, its svcs answered there and handed to
                    // host 0 (the busiest by number), the wait for those, and how far its clock
                    // was ahead of host 0's.
                    {
                        const auto c = system.TakeSecondHostCensus();
                        const auto top = [](const std::array<u16, 256>& by) {
                            std::array<std::pair<u32, u32>, 256> all{};
                            for (u32 i = 0; i < 256; i++) {
                                all[i] = {by[i], i};
                            }
                            std::sort(all.begin(), all.end(), std::greater<>());
                            std::string list;
                            for (u32 i = 0; i < 6 && all[i].first != 0; i++) {
                                fmt::format_to(std::back_inserter(list), " {:02x}:{}",
                                               all[i].second, all[i].first);
                            }
                            return list;
                        };
                        if (c.passes != 0 || c.host0_idles != 0) {
                            LOG_INFO(Frontend,
                                     "perf: second thread: {} passes, {} runs, {} idles ({:.1f} "
                                     "ms asleep); local svcs {}{}; forwarded {} ({:.1f} ms "
                                     "waiting, {} served){}; held pages {} ({} raced a release); "
                                     "host 0 idles {} ({:.1f} ms asleep; woken by a message {} "
                                     "{:.1f} ms, the "
                                     "clock {} {:.1f} ms, an idle {} {:.1f} ms, else {} {:.1f} "
                                     "ms); lead {} ticks; clock wakes {:.1f} us after the "
                                     "clock ({} timed), sleeps <10/25/50/100/250/more us {}",
                                     c.passes, c.runs, c.idles, c.idle_us / 1000.0, c.local_svcs,
                                     top(c.local_svcs_by), c.forwards, c.forward_wait_us / 1000.0,
                                     c.served, top(c.forward_svcs), c.held_pages, c.raced_holds,
                                     c.host0_idles,
                                     c.host0_idle_us / 1000.0, c.host0_woken[0],
                                     c.host0_woken_us[0] / 1000.0, c.host0_woken[1],
                                     c.host0_woken_us[1] / 1000.0, c.host0_woken[2],
                                     c.host0_woken_us[2] / 1000.0, c.host0_woken[3],
                                     c.host0_woken_us[3] / 1000.0, c.lead,
                                     c.clock_wakes_timed == 0
                                         ? 0.0
                                         : static_cast<double>(c.clock_wake_latency_us) /
                                               c.clock_wakes_timed,
                                     c.clock_wakes_timed,
                                     fmt::join(c.clock_sleeps_by_length, "/"));
                            const auto& h = c.host0_idle_how;
                            const u32 jumps = h[1] + h[2] + h[3];
                            LOG_INFO(Frontend,
                                     "perf: host 0 idles: work at once {}, no sleep to its own "
                                     "event with host 1 idle {}, with host 1 past it {}, to host "
                                     "1's clock or event {}, slept {}; {:.1f} us of guest time a "
                                     "no-sleep idle",
                                     h[0], h[1], h[2], h[3], h[4],
                                     jumps == 0 ? 0.0
                                                : c.host0_jump_ticks /
                                                      (BASE_CLOCK_RATE_ARM11 / 1e6) / jumps);
                        }
                        // Seconds with no game frame: every guest thread once, to the log.
                        static u32 frameless = 0;
                        frameless = perf.game_fps < 0.5 ? frameless + 1 : 0;
                        if (frameless == 3) {
                            system.RequestThreadDump();
                        }
                    }
                    // The native core's slices: time in the guest and in azaharRun (ms), how
                    // the slices ended, the svcs answered in the guest, and the svcs that left
                    // it, most frequent first.
                    const auto n = Core::TakeVitaNativeCensus();
                    const double tick_ms = BASE_CLOCK_RATE_ARM11 / 1e3;
                    LOG_INFO(
                        Frontend,
                        "perf: guest {:.1f} ms, azaharRun {:.1f} ms, {} slices: svc {}, "
                        "timer {}, faults {} (loads past held bytes {}, stores {} (of both, "
                        "Thumb {}), busy pages opened {}; VFP sites patched to a thunk {}, to "
                        "the svc {}); svc sites patched to a thunk {}; upcall sites {} (Thumb "
                        "{}) runs {} redirected {} rescheduled {} switched {} idled {} ({} "
                        "events, {} exits, {} for core 1, {} for the vblank), exits for the "
                        "slice's end {} and no next thread {} syscalls {} preempted {} ({} in "
                        "the vector), app megabytes taken {}, upcall stack {} bytes; svcs "
                        "answered in the guest: tick {}, cache {}, thread id {}, process id {}, "
                        "VFP {}",
                        n.guest_ticks / tick_ms,
                        n.kernel_cycles / (scePowerGetArmClockFrequency() * 1e3), n.slices,
                        n.reasons[AZAHAR_EXIT_SVC], n.reasons[AZAHAR_EXIT_TIMER],
                        n.slices - n.reasons[AZAHAR_EXIT_SVC] - n.reasons[AZAHAR_EXIT_TIMER],
                        n.hold_loads, n.hold_stores, n.hold_thumb, n.hold_opens, n.vfp_thunks,
                        n.vfp_svcs, n.svc_thunks, n.upcall_sites, n.upcall_thumb_sites,
                        n.upcall_runs, n.upcall_redirects, n.upcall_reschedules, n.upcall_switches,
                        n.upcall_idles, n.upcall_idle_events, n.upcall_idle_exits,
                        n.upcall_idle_exits_other_busy, n.upcall_idle_exits_vblank,
                        n.upcall_exits_slice_up, n.upcall_exits_no_next, n.upcall_syscalls,
                        n.upcall_preempted, n.upcall_rearms, n.upcall_synced, n.upcall_stack_used,
                        n.fast[AZAHAR_FAST_TICK], n.fast[AZAHAR_FAST_CACHE],
                        n.fast[AZAHAR_FAST_THREAD_ID], n.fast[AZAHAR_FAST_PROCESS_ID],
                        n.fast[AZAHAR_FAST_VFP]);
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
                             "longer {})",
                             t.busy_us / 1e3, t.idle_us / 1e3, t.gpu_wakes, t.timeouts,
                             render_counters.gpu_wait_us / 1e3, t.records[K::State],
                             t.records[K::Draw], t.records[K::Fill], t.records[K::Transfer],
                             t.records[K::Dma], t.records[K::Present], t.records[K::Other],
                             t.presents, t.presents_behind, t.presents_soon, t.present_gaps[0],
                             t.present_gaps[1], t.present_gaps[2], t.present_gaps[3]);
                    LOG_INFO(
                        Frontend,
                        "perf: late skip: {} frames, {} draws dropped, "
                        "{} kept for the other screen; screens presented from another buffer {}; "
                        "drawn whole with the GPU late {}, the vblank waiting {:.1f} ms; right "
                        "eye: {} draws, {} transfers left out",
                        t.late_frames, t.late_draws, t.late_kept, t.late_partial, t.late_waits,
                        t.late_wait_us / 1e3, t.right_eye_draws, t.right_eye_transfers);
                    LOG_INFO(Frontend,
                             "perf: parse thread busy {:.1f} ms; the render thread waited for "
                             "a list's parse {} times ({:.1f} ms of its busy time); geometry "
                             "draws merged: {} vertices in {} runs",
                             t.parse_busy_us / 1e3, t.parsed_waits, t.parsed_wait_us / 1e3,
                             t.merged_draws, t.merged_runs);
                    LOG_INFO(Frontend,
                             "perf: command lists: {} ({:.1f} KiB; {:.1f} KiB parsed with the "
                             "buffers they jump to, in {} ranges; {:.1f} KiB the most in one), "
                             "{} draws; the emulation thread's scan {:.1f} ms; index scans {} "
                             "({} indices, {:.1f} ms), {} ranges from the memo; words in VRAM: "
                             "{:.1f} KiB from the kept copy, {:.1f} KiB read",
                             t.lists, t.list_bytes / 1024.0, t.list_parsed_bytes / 1024.0,
                             t.list_ranges, t.list_parsed_max / 1024.0, t.list_draws,
                             t.scan_us / 1e3, t.index_scans, t.index_scan_indices,
                             t.index_scan_us / 1e3, t.index_memo_hits,
                             t.list_kept_bytes / 1024.0, t.list_vram_bytes / 1024.0);
                    LOG_INFO(Frontend,
                             "perf: resource files: {} named, {} gone, {} known ({:.1f} MiB); "
                             "reads inside them not held: textures {}, indices {}, vertices {}",
                             t.resource_named, t.resource_gone, t.resources,
                             t.resource_bytes / 1048576.0, t.resource_skips[0],
                             t.resource_skips[1], t.resource_skips[2]);
                    LOG_INFO(Frontend,
                             "perf: frame buffers: {} reads not held; {} turns waited for the "
                             "GPU, {:.1f} ms",
                             t.frame_buffer_reads, t.frame_buffer_waits,
                             t.frame_buffer_wait_us / 1e3);
                    LOG_INFO(Frontend,
                             "perf: ring: {} publishes; wakes of the render thread {}, of the "
                             "parse thread {} ({:.1f} ms in the syscalls)",
                             t.publishes, t.render_wakes, t.parse_wakes, t.wake_us / 1e3);
                    const auto eye = system.GPU().GetRightEyeDisabler().TakeCounts();
                    LOG_INFO(Frontend,
                             "perf: right-eye guess: {} lists and {} transfers left out, {} "
                             "frames with the top picture sent to both eyes",
                             eye.lists, eye.transfers, eye.both_eyes);
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
                             "waited, {:.1f} ms; {} list pages copied "
                             "for the parse thread, {} waits for a parse ({:.1f} ms), {} copies "
                             "for the render thread's CPU reads ({} bytes)), HLE waits {} "
                             "({:.1f} ms), HLE writes past the holds {}, onto a version {}",
                             hs.holds.calls, hs.holds.runs, hs.holds.descriptors,
                             hs.holds.held_pages, hs.holds.released_pages, hs.faults,
                             hs.versioned_faults, hs.fault_waits, hs.fault_wait_us / 1e3,
                             hs.parse_copies, hs.parse_waits,
                             hs.parse_wait_us / 1e3, hs.copy_faults, hs.copy_bytes,
                             hs.host_waits, hs.host_wait_us / 1e3, hs.host_past,
                             hs.host_versions);
                    // Page versions: azaharFlip's calls, the pages that got a second slot,
                    // the swaps, the writes in place (with queued work reading the guest's slot,
                    // and with none), the remaps at frame starts and before a same-frame use,
                    // the pages gone home, and the pool's slots in use.
                    LOG_INFO(Frontend,
                             "perf: page versions: azaharFlip {} calls; {} versioned, {} swaps, "
                             "{} written in place under queued reads, {} in place; remaps {} at "
                             "frame starts, {} early; {} home; {} slots in use; cached "
                             "geometry: {} slots in use, {} pages cleaned at their reads "
                             "(stores past the holds alone {} on {} lines, with another cause "
                             "{}, flips {}, other {}), {} pages left their versions",
                             hs.holds.flip_calls, hs.holds.versions, hs.holds.swaps,
                             hs.holds.in_place_read, hs.holds.in_place, hs.holds.frame_remaps,
                             hs.holds.early_remaps, hs.holds.homes, hs.holds.slots_used,
                             hs.holds.cached_slots_used, hs.holds.cleaned_pages,
                             hs.holds.cleaned_past, hs.holds.past_lines, hs.holds.cleaned_past_and,
                             hs.holds.cleaned_flip, hs.holds.cleaned_other, hs.holds.left_versions);
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
                                 "perf: dsp: {} frames ({} started at the tick), {:.1f} ms on "
                                 "its thread ({:.2f} ms a frame); {} waits at the interrupt, "
                                 "{:.2f} ms; {} semaphore "
                                 "signals, {:.0f} us after the interrupt (longest {}); {} ticks "
                                 "with no new frame from the application, {} buffer changes "
                                 "reported in them; ticks waited {} steps for the answer, {} "
                                 "given up; frames started {:.0f} us after their post, the "
                                 "ones waited for were posted {:.0f} us before the wait",
                                 d.frames, d.tick_frames, d.work_us / 1e3,
                                 d.frames ? d.work_us / 1e3 / d.frames : 0.0, d.waits,
                                 d.wait_us / 1e3, d.semaphores,
                                 d.semaphores ? static_cast<double>(d.answer_us) / d.semaphores
                                              : 0.0,
                                 d.answer_longest_us, d.late_ticks, d.late_buffer_changes,
                                 d.tick_waits, d.unanswered,
                                 d.frames ? static_cast<double>(d.start_us) / d.frames : 0.0,
                                 d.waits ? static_cast<double>(d.wait_lead_us) / d.waits : 0.0);
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

    if (title_crashed) {
        // A core dump while the title's state is still there, then the game list's notice. The
        // report's preamble as things are now, unless this crash's own lines wrote it.
        Common::Log::RenewCrashReport();
        LOG_CRITICAL(Frontend, "The title crashed: {}", system.GetStatusDetails());
#ifdef VITA_DIAGNOSTICS
        // Launch argument crashhold: the crashed title's state stays for the command channel's
        // reading commands (what the title wrote where its code went wrong) until `go`, ten
        // minutes at most.
        if (CommandChannel() &&
            std::find(g_arguments.begin() + 1, g_arguments.end(), "crashhold") != g_arguments.end()) {
            sceClibPrintf("[azahar] crashhold: reading commands until go\n");
            const SceUInt64 hold_start_us = sceKernelGetProcessTimeWide();
            while (sceKernelGetProcessTimeWide() - hold_start_us < 600 * 1000000ull) {
                const std::string command = command_file.Poll();
                if (command == "go") {
                    break;
                }
                if (!command.empty() && !RunInspectCommand(system, command)) {
                    sceClibPrintf("[azahar] crashhold: \"%s\" is not a reading command\n",
                                  command.c_str());
                }
                sceKernelDelayThread(100000);
            }
            sceClibPrintf("[azahar] crashhold: over\n");
        }
#endif
        VitaFrontend::CrashReport::WriteProcessState();
        // While the title is up its presents are the render thread's: the guest frames it still
        // had queued built the same ImGui frame as this thread. Nothing comes after.
        const bool powered = system.IsPoweredOn();
        if (powered) {
            system.GPU().GetRenderThread().RunSync([] {});
        }
        const auto present = [&] {
            const auto build = [&] {
                ui.Update();
                window.Present(&ui, false);
            };
            if (powered) {
                system.GPU().GetRenderThread().RunSync(build);
            } else {
                build();
            }
        };
        ui.SetStatusMessage(Tr("The game crashed. Saving a crash report..."));
        present();
        // The dump on a thread of its own, the panel meanwhile showing how much of it is written:
        // minutes over host0, the whole process suspended for its first part. The
        // last dump's size stands for the whole; without one, the size alone.
        char dump[256] = {};
        std::atomic<bool> dump_done{false};
        int dumped = 0;
        const long dump_since = static_cast<long>(time(nullptr));
        std::thread dumper([&] {
            dumped = VitaFrontend::CrashReport::Coredump(dump, sizeof(dump));
            dump_done = true;
        });
        const u64 expected = VitaFrontend::CrashReport::LastDumpSize();
        const u64 dump_start_us = sceKernelGetProcessTimeWide();
        u64 last_written = 0, done_at_us = 0;
        for (u32 frame = 0; !dump_done; frame++) {
            // About six times a second: the size to a tenth of a MB (KB below one) and, against
            // the last dump's size, the time left at the rate so far. The system writes the dump
            // a MiB at a time, a step every four seconds over host0: the time left
            // counts down between the steps.
            if (frame % 10 == 0) {
                const u64 written = VitaFrontend::CrashReport::DumpBytesWritten(dump_since);
                const u64 now = sceKernelGetProcessTimeWide();
                if (written != last_written && written != 0 && written < expected) {
                    last_written = written;
                    done_at_us = now + (now - dump_start_us) * (expected - written) / written;
                }
                const std::string size =
                    written < (u64{1} << 20)
                        ? fmt::format("{} KB", written >> 10)
                        : fmt::format("{}.{} MB", written >> 20, ((written & 0xFFFFF) * 10) >> 20);
                const u64 left =
                    written < expected && done_at_us > now ? (done_at_us - now + 999'999) / 1'000'000
                                                           : 0;
                ui.SetStatusMessage(
                    left != 0
                        ? TrFormat("The game crashed. Saving a crash report: {}, about {}:{:02} "
                                   "left...",
                                   size, left / 60, left % 60)
                        : TrFormat("The game crashed. Saving a crash report: {}...", size));
                if (expected != 0) {
                    ui.SetProgress(std::min(0.99f, static_cast<float>(written) /
                                                       static_cast<float>(expected)));
                }
            }
            present();
            sceDisplayWaitVblankStart();
        }
        dumper.join();
        if (dumped >= 0) {
            VitaFrontend::CrashReport::NoteDumpSize(dump);
        }
        sceClibPrintf("[azahar] the title crashed; core dump %08x, %s\n",
                      static_cast<unsigned>(dumped), dumped >= 0 ? dump : "none");
        ui.ClearStatusMessage();
        // The title's settings still applied (SetRunningTitle below).
        char changed[192];
        Config::WriteChangedFromDefaults(changed, sizeof(changed));
        g_crash_notice = ArchivedCrashNotice(
            ui, present,
            {CrashWhat(ui, Tr("The game crashed."), changed), Common::Log::CrashReportPath,
             dumped >= 0 ? dump : ""});
    }
    window.SetPauseMenu(nullptr);
    window.SetInputToGuest(true);
    window.SetLoading(false);
    window.SetUi(nullptr);
    ShutdownSystem(system, threads_before);
    window.ForgetSwapToggle();
    Config::SetRunningTitle({});
    VitaFrontend::CrashReport::NoteCheats(nullptr);
    VitaFrontend::CrashReport::NoteGame({}, 0, {});
    Config::Save();
    // A stopped title can leave the interface on a pause screen: back to the game list.
    ui.LeaveGame();
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
 * timing and the GPU exist (Core::System::InitGpuOnly); the frame's commands and memory come
 * from the capture. The first requests are the vpk's app0:replay.txt when host0 has none,
 * later ones host0:vprof-replay.txt's, and the results go to host0 (vprof_replay.cpp), so the
 * player needs host0.
 */
void RunReplay(Core::System& system, VitaFrontend::EmuWindowVita& window,
               const std::string& capture) {
    window.UpdateCurrentFramebufferLayout();
    std::string first;
    SceIoStat st{};
    if (sceIoGetstat(fmt::format("{}vprof-replay.txt", SgxdRenderer::Vprof::VprofDir).c_str(),
                     &st) < 0) {
        first = ReadWholeFile("app0:replay.txt");
        if (first.empty()) {
            first = "results\nprofile 30\nquit\n";
        }
    }
    std::string shown = first.empty() ? std::string{"vprof-replay.txt's"} : first;
    std::replace(shown.begin(), shown.end(), '\n', ';');
    LOG_INFO(Frontend, "replay player: {}, first requests {}", capture, shown);
    SgxdRenderer::Vprof::Info("replay", capture.c_str());
    if (system.InitGpuOnly(window) != Core::System::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "replay player: the GPU could not be brought up");
        return;
    }
    system.GPU().VprofReplay(capture, first);
    system.ShutdownGpuOnly();
    LOG_INFO(Frontend, "replay player: done");
}
#endif

/**
 * The title to start without going through the list, if the launcher named one.
 *
 * Two ways in, because there are two launchers. A LiveArea bubble or another application
 * starts this one with a `psgm:play?titleid=...&param=<path>` string, which arrives through
 * sceAppMgrGetAppParam. The Vita3K emulator passes its --app-args as the process's own
 * arguments instead; a `-` there names no title, for a scripted launch that starts on the list
 * with arguments after it (`commands`). Empty when neither did: then the list comes up as always.
 */
std::string LaunchParameter() {
    if (!g_arguments.empty()) {
        return g_arguments.front() == "-" ? std::string{} : g_arguments.front();
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
bool IsOwnSelfPath(std::string_view argument);

/**
 * Finds the marker LoadAzaharNative's restart carries. The arguments after it, when there are
 * any, are the launch parameter the first process was started with and the ones after it
 * (load=N, save=N@T), which the restart would otherwise lose; SceAppMgr's options among them
 * are skipped as main does, and a SELF's path among them says the first process was launched by
 * path. The scan starts at argv[0] because nothing says whether the restarted process gets its
 * own path there.
 */
bool TakeRelaunchArguments(int argc, char* argv[]) {
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == nullptr || std::strcmp(argv[i], RelaunchArgument) != 0) {
            continue;
        }
        for (int j = i + 1; j < argc && argv[j] != nullptr && argv[j][0] != '\0'; ++j) {
            if (argv[j][0] == '-' && argv[j][1] != '\0') {
                j += AppMgrOptionValues(argv[j]);
                continue;
            }
            if (IsOwnSelfPath(argv[j])) {
                g_launched_by_path = true;
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

StartError LoadAzaharNative(bool relaunched) {
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
                    return [] {
                        return std::string{Tr("Remove the azaharnative.skprx line from your taiHEN "
                                              "config.txt (ux0:tai/config.txt or "
                                              "ur0:tai/config.txt), then reboot your console.")};
                    };
                }
                // Retail firmware keeps a module that exports calls to user mode (a syscall
                // export, as this one's are) loaded outside development mode: SceKernelModulemgr's
                // stop answers 0x8002d002. This vpk's module is
                // loaded by the next start after a reboot.
                sceClibPrintf("[azahar] azaharnative %d stays loaded, this vpk's is %d: a reboot "
                              "finishes the update\n",
                              loaded, AZAHAR_NATIVE_VERSION);
                return [] {
                    return std::string{
                        Tr("Azahar was updated. Reboot your console to finish the update.")};
                };
            }
            // The kernel's printf text from here on reaches this app's TTY (klog.c).
            SgxdRenderer::SgxdDrainKernelLog();
            // sgxd's spins time out on the calling thread's cycle counter (azaharCycles): every
            // thread of this process may read it, those that exist now and those created later.
            if (const int cycles = azaharCycleCounter(); cycles < 0) {
                return [cycles] {
                    return fmt::format("azaharCycleCounter failed ({:#010x}).",
                                       static_cast<u32>(cycles));
                };
            }
            return {};
        }
    }
    const SceUID module = taiLoadStartKernelModule(AzaharNativePath, 0, nullptr, 0);
    if (module == static_cast<SceUID>(SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE)) {
        // The kernel's own memory, which kernel plugins share, has no room for the module.
        sceClibPrintf("[azahar] azaharnative.skprx not loaded: 0x%08x, kernel memory full\n",
                      static_cast<u32>(module));
        return [] {
            return std::string{Tr("Your kernel plugins leave too little memory for Azahar. Remove "
                                  "some lines under *KERNEL in your taiHEN config.txt "
                                  "(ux0:tai/config.txt or ur0:tai/config.txt), then reboot your "
                                  "console.")};
        };
    }
    if (module < 0) {
        return [module] {
            return TrFormat("{} could not be loaded ({:#010x}). Reinstall the vpk.",
                            AzaharNativePath, static_cast<u32>(module));
        };
    }
    // Restarting a second time would not bind anything the first restart did not.
    if (relaunched) {
        return [] {
            return std::string{
                "azaharnative.skprx was loaded twice without the restarted app seeing it."};
        };
    }

    // The restart carries the launch parameter and the arguments after it (load=N, save=N@T), and
    // for a launch by path a SELF's path before them.
    std::string marker{RelaunchArgument};
    std::vector<std::string> carried = g_arguments;
    if (carried.empty()) {
        if (std::string launch = LaunchParameter(); !launch.empty()) {
            carried.push_back(std::move(launch));
        }
    }
    std::vector<char*> args{marker.data()};
    std::string self_path{"app0:eboot.bin"};
    if (g_launched_by_path) {
        args.push_back(self_path.data());
    }
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
    return [exec] {
        return TrFormat(
            "Azahar could not restart to use azaharnative.skprx ({:#010x}). Start Azahar again.",
            static_cast<u32>(exec));
    };
}

/// What stops the start, on the notice through GXM: only before sgxd has the GPU, or when it could
/// not take it.
void ShowStartError(const std::string& message) {
    VitaFrontend::ShowGxmNotice(Tr("Azahar can't start"), message, "", Tr("Close"));
}

/// The emulator proper. Runs on a thread of its own; see main.
int EmulatorMain() {
    // This thread drives the guest (which itself runs on the detached core 2, outside the
    // system scheduler) and runs the GPU; RoleCores in common/thread.cpp lays out
    // which of the three schedulable cores each host thread gets.
    Common::SetCurrentThreadRole(Common::ThreadRole::Emulation);

    SetupPaths();
    SetupLogging();
    // What the player chose, from the memory card: the values for all games until a title runs.
    // Before the first screen, which takes its theme and language from them.
    Config::Load();
    Loader::SetTitlePatchesTranslator(&Tr);

    // What stops the start comes first, in the menus' language: before anything else is set up
    // and before the startup notice. Closing it exits the app.
    if (g_native_error) {
        ShowStartError(g_native_error());
        return -1;
    }
    // azaharnative knows the thread managers of the firmwares it unlocks core 3 on.
    if (g_cores_unlocked == AZAHAR_ERR_RESOLVE) {
        ShowStartError(Tr("Azahar needs firmware 3.60, 3.65, 3.68 or 3.73."));
        return -1;
    }
    ASSERT_MSG(g_cores_unlocked > 0, "azaharUnlockCores: {:#010x}",
               static_cast<u32>(g_cores_unlocked));

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

    // The last moment the system can draw: anything that needs its notifications or quick menu
    // (a plugin's menu there, such as PSVshell's clocks) has to happen now. The clocks are set
    // already (SetupClocks), so a plugin's choice there is the one that stays, and the PS button
    // is not locked yet. Not on a crash's restart, whose session showed it, nor on a launch by
    // path (a scripted launch drives the console itself) unless it asks with `notice`.
    const auto argument_given = [](auto test) {
        return std::any_of(g_arguments.begin(), g_arguments.end(), test);
    };
    const bool crash_restart = argument_given(
        [](const std::string& argument) { return argument.starts_with("crashed="); });
    const bool asked =
        argument_given([](const std::string& argument) { return argument == "notice"; });
    // Nor in the replay player, which a scripted launch starts with no path and nobody presses X
    // for.
#ifdef AZAHAR_REPLAY_PLAYER
    constexpr bool replay_player = true;
#else
    constexpr bool replay_player = false;
#endif
    if (Config::Global(Option::StartupNotice) != 0 && !crash_restart && !replay_player &&
        (!g_launched_by_path || asked)) {
        VitaFrontend::ShowGxmNotice(
            Tr("Before Azahar starts"),
            Tr("Azahar is about to take over the GPU. Until you close it, notifications, the quick "
               "menu and plugin menus such as PSVshell's won't show, and the PS button won't leave "
               "Azahar.\n\nUse them now if you need them, then press X to continue."),
            Tr("You can turn this screen off in Settings > System > Startup notice."),
            Tr("Continue"));
    }
    // Nor is the GPU taken from under the system's UI: the quick menu, opened during the start with
    // the notice off, or a notification's popup. SceAppMgr has no event for either; the take after
    // a resume waits the same way (PowerThread). A popup the take found up stayed, its plane
    // showing the app's data in the shell's CDRAM; frame buffers the shell hands the display after
    // the take are kept off by the module.
    if (VitaFrontend::SystemUiOverlaid() || VitaFrontend::ShellPlaneShown()) {
        LOG_INFO(Frontend,
                 "the system's UI or its plane over the app: the GPU taken once they are gone");
        while (VitaFrontend::SystemUiOverlaid() || VitaFrontend::ShellPlaneShown()) {
            sceKernelDelayThread(100 * 1000);
        }
        LOG_INFO(Frontend, "the system's UI and its plane gone");
    }

    // The GPU is sgxd's from here to the end of the session (docs/sgxd/takeover.md). No GXM call
    // may follow: SceGpu's callers would wait at its gate.
    std::string sgxd_error;
    if (!VitaFrontend::SgxdPresent::Initialize(&sgxd_error)) {
        LOG_CRITICAL(Frontend, "{}", sgxd_error);
        ShowStartError(sgxd_error);
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
    {
        const SceUID power = sceKernelCreateThread("AzaharPower", PowerThread, 0x10000100, 0x4000,
                                                   0, 0, nullptr);
        ASSERT_MSG(power >= 0 && sceKernelStartThread(power, 0, nullptr) >= 0,
                   "the power thread did not start");
        SceUID standby_flag = sceKernelCreateEventFlag("AzaharStandby", 0, 0, nullptr);
        ASSERT_MSG(standby_flag >= 0, "the standby's flag was not created ({:#010x})",
                   static_cast<u32>(standby_flag));
        const SceUID standby = sceKernelCreateThread(
            "AzaharStandby", StandbyThread, SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH, 0x4000, 0,
            0, nullptr);
        ASSERT_MSG(standby >= 0 &&
                       sceKernelStartThread(standby, sizeof(standby_flag), &standby_flag) >= 0,
                   "the standby thread did not start");
        VitaFrontend::SgxdPresent::WatchStandby(standby_flag, 1);
    }

    auto& system = Core::System::GetInstance();

    // The Vita has neither the memory nor the drivers for the LLE service modules; sgxd is the
    // only renderer.
    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }
    Settings::values.graphics_api = Settings::GraphicsAPI::Sgxd;
    // Vertex shading happens on the GPU or not at all: with this off, the PICA code would send
    // every draw down its CPU vertex path, which the sgxd rasterizer does not have.
    Settings::values.use_hw_shader = true;
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

    VitaFrontend::EmuWindowVita window(system);
    power_window = &window;
    // Not InputCommon::Init(): every factory it registers is one this frontend does not use (the
    // buttons, circle pad and touch come from the "vita" engine and EmuWindow's own factory).
    VitaFrontend::InitInput(window);

    VitaFrontend::Ui ui(system, window);

    // The selected title's banner in the list's panel: the GPU on its own while the list shows,
    // down while a title runs, which brings up its own.
    VitaFrontend::BannerPlayer banner{system, window};
    const auto run_title = [&](const std::string& path) {
        banner.Stop();
        const bool more = RunTitle(system, window, ui, path);
        if (more) {
            // A title may have installed or removed titles itself (AM: a launcher that
            // reinstalls itself with a new content file): the list read again, or its entry names
            // a content file that is gone.
            ui.ScanGames();
            banner.Start(VitaFrontend::Ui::BannerRect());
        }
        return more;
    };
    // A crash's restart (CrashReport::Recover): its notice, with the title's settings changed
    // from their defaults, and the command channel going on after the last line the crashed
    // process ran.
    std::string_view changed;
    for (const std::string& argument : g_arguments) {
        if (argument.starts_with("changed=")) {
            changed = std::string_view{argument}.substr(8);
        }
    }
    for (const std::string& argument : g_arguments) {
        if (argument.starts_with("crashed=")) {
            g_crash_files = CrashFiles{CrashWhat(ui, Tr("Azahar crashed and started again."), changed),
                                       VitaFrontend::CrashReport::LastReportPath,
                                       argument.substr(8)};
        }
#ifdef VITA_DEBUG_TOOLS
        if (argument.starts_with("seq=")) {
            command_file.StartAfter(std::strtoul(argument.c_str() + 4, nullptr, 10));
        }
#endif
    }
    // From here a crash the process finds itself starts it again on the game list
    // (CrashReport::Recover); before, the system takes it, as a restart would crash again. The
    // restart leaves a title's launch parameter and savestate arguments behind.
    std::vector<std::string> restart{RelaunchArgument, "-"};
    for (std::size_t i = 1; i < g_arguments.size(); i++) {
        const std::string& argument = g_arguments[i];
        if (!argument.starts_with("load=") && !argument.starts_with("save=") &&
            !argument.starts_with("seq=") && !argument.starts_with("crashed=") &&
            !argument.starts_with("changed=")) {
            restart.push_back(argument);
        }
    }
#ifdef VITA_DEBUG_TOOLS
    VitaFrontend::CrashReport::EnableRecovery(restart,
                                              CommandChannel() ? command_file.LastSeq() : nullptr);
#else
    VitaFrontend::CrashReport::EnableRecovery(restart, nullptr);
#endif
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
        // A bubble's game checked as the list's is (a launch by path is scripted): a
        // damaged one leaves the game list showing its question.
        const auto present_start = [&] {
            window.PollEvents();
            ui.Update();
            window.Present(&ui);
        };
        if (g_launched_by_path || ui.LaunchChecked(launch, present_start)) {
            running = RunTitle(system, window, ui, launch);
            // As run_title's: what the title installed or removed itself.
            if (running) {
                ui.ScanGames();
            }
        }
    }
    if (running) {
        banner.Start(VitaFrontend::Ui::BannerRect());
    }
    // A frame of the game list, for a busy stretch (a pin, an install): the loop is alive.
    const auto present_list = [&] {
        VitaFrontend::CrashReport::StallWatchBeat();
        window.PollEvents();
        banner.Frame(ui.BannerTitle(), VitaFrontend::Ui::BannerBackdrop());
        ui.Update();
        window.Present(&ui);
    };
    // An outcome as the status message, for two seconds, as a load's failure.
    const auto show_outcome = [&](std::string message) {
        ui.SetStatusMessage(std::move(message));
        for (int i = 0; i < 120; i++) {
            present_list();
            sceDisplayWaitVblankStart();
        }
        ui.ClearStatusMessage();
    };
    // A notice the player closes (a crash's): the status message until X or O is pressed and let
    // go, so that the list does not take the press.
    const auto show_notice = [&](std::string message) {
        ui.SetStatusMessage(std::move(message));
        int phase = 0; // 0: the buttons let go, 1: one pressed, 2: let go again
        while (phase < 3) {
            present_list();
            sceDisplayWaitVblankStart();
            SceCtrlData pad{};
            const bool held = sceCtrlPeekBufferPositive(0, &pad, 1) > 0 &&
                              (pad.buttons & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) != 0;
            if (held == (phase == 1)) {
                phase++;
            }
        }
        ui.ClearStatusMessage();
    };
    const auto install_selected = [&] {
        std::string outcome = ui.InstallSelected(present_list);
        LOG_INFO(Frontend, "install: {}", outcome);
        show_outcome(std::move(outcome));
    };
    const auto check_selected = [&] {
        const std::string outcome = ui.CheckSelected(present_list);
        LOG_INFO(Frontend, "check: {}", outcome);
        show_notice(outcome + "\n\n" + Tr("Press X or O to continue."));
    };
    while (running && !window.IsQuitRequested()) {
        if (g_crash_files) {
            g_crash_notice = ArchivedCrashNotice(ui, present_list, *g_crash_files);
            g_crash_files.reset();
        }
        if (!g_crash_notice.empty()) {
            LOG_INFO(Frontend, "crash notice shown");
            show_notice(std::exchange(g_crash_notice, {}));
            LOG_INFO(Frontend, "crash notice closed");
        }
        window.PollEvents();
        // The banner before the interface: the list leaves the panel open for the banner as the
        // banner stands when it draws (a new selection hides the old one here); drawn after it,
        // the panel shows the frame's black clear for a few refreshes at each new selection.
        banner.Frame(ui.BannerTitle(), VitaFrontend::Ui::BannerBackdrop());
        const auto action = ui.Update();
        window.Present(&ui);
        // The present waits for no vblank: a frame a refresh.
        sceDisplayWaitVblankStart();

        switch (action) {
        case VitaFrontend::Ui::Action::LaunchGame: {
            // A damaged file raises a question instead (Launch anyway, Full check, Cancel).
            const std::string path = ui.SelectedGamePath();
            if (ui.LaunchChecked(path, present_list)) {
                running = run_title(path);
            }
            break;
        }
        case VitaFrontend::Ui::Action::ExitApplication:
            running = false;
            break;
        case VitaFrontend::Ui::Action::TogglePin: {
            ui.SetStatusMessage(Tr("Changing the LiveArea..."));
            banner.Frame(ui.BannerTitle(), VitaFrontend::Ui::BannerBackdrop());
            ui.Update();
            window.Present(&ui);
            show_outcome(ui.TogglePin());
            break;
        }
        case VitaFrontend::Ui::Action::InstallTitle:
            install_selected();
            break;
        case VitaFrontend::Ui::Action::CheckFile:
            check_selected();
            break;
        case VitaFrontend::Ui::Action::ClearData: {
            const std::string outcome = ui.ClearData(present_list);
            LOG_INFO(Frontend, "clear: {}", outcome);
            show_notice(outcome + "\n\n" + Tr("Press X or O to continue."));
            break;
        }
        default:
            break;
        }
#ifdef VITA_DEBUG_TOOLS
        if (const std::string command = running && CommandChannel() ? command_file.Poll() : "";
            command.starts_with("launch ")) {
            LOG_INFO(Frontend, "{} (command)", command);
            running = run_title(command.substr(7));
        } else if (command.starts_with("select ")) {
            const bool found = ui.SelectTitle(command.substr(7));
            LOG_INFO(Frontend, "{}: {} (command)", command, found ? "selected" : "no such title");
        } else if (command == "rescan") {
            ui.ScanGames();
            LOG_INFO(Frontend, "rescan (command)");
        } else if (command.starts_with("install ")) {
            // The CIA whose name or path holds the text, installed as the list's cross does.
            if (ui.SelectTitle(command.substr(8))) {
                LOG_INFO(Frontend, "{} (command)", command);
                install_selected();
            } else {
                LOG_INFO(Frontend, "{}: no such title (command)", command);
            }
        } else if (command.starts_with("check ")) {
            // The title whose name or path holds the text, checked as its File tab does.
            if (ui.SelectTitle(command.substr(6))) {
                LOG_INFO(Frontend, "{} (command)", command);
                const std::string outcome = ui.CheckSelected(present_list);
                LOG_INFO(Frontend, "check: {}", outcome);
                show_outcome(outcome);
            } else {
                LOG_INFO(Frontend, "{}: no such title (command)", command);
            }
        } else if (command == "exit") {
            running = false;
        } else if (!command.empty() && !RunSettingCommand(command) &&
                   !VitaFrontend::Multiplayer::RunCommand(system, command) &&
                   !VitaFrontend::Adhoc::RunCommand(system, command)) {
            LOG_INFO(Frontend, "command \"{}\" not known on the game list", command);
        }
#endif
    }

    banner.Stop();
    Config::Save();
    power_window = nullptr;
    if (VitaFrontend::Adhoc::Active()) {
        VitaFrontend::Adhoc::Leave();
    }
    VitaFrontend::Multiplayer::Shutdown();
    VitaFrontend::ShutdownInput();
    Common::Log::Stop();

    sceShellUtilUnlock(shell_locks);
    VitaFrontend::SgxdPresent::Shutdown();
    return 0;
}

/// Whether an argument is this process's own SELF: a path launch passes it first.
bool IsOwnSelfPath(std::string_view argument) {
    return argument.size() >= 9 && argument.substr(argument.size() - 9) == "eboot.bin";
}

/// How many values follow one of SceAppMgr's launch options (-budget, -memsize,
/// -phcontmemsize, -la_type, -titleid, -param, -exit_to take one; -livearea_off,
/// -nonsuspendable, -usecdlg none). An unknown option takes none.
int AppMgrOptionValues(std::string_view option) {
    for (const char* name : {"-budget", "-memsize", "-phcontmemsize", "-la_type", "-titleid",
                             "-param", "-exit_to"}) {
        if (option == name) {
            return 1;
        }
    }
    return 0;
}

} // Anonymous namespace

int main(int argc, char* argv[]) {
#ifdef VITA_DIAGNOSTICS
    // The main thread's slot is the list's first (_init_vita_reent).
    newlib_reent_slots = reinterpret_cast<NewlibReentSlot*>(reinterpret_cast<char*>(__getreent()) -
                                                            offsetof(NewlibReentSlot, reent));
    ASSERT(newlib_reent_slots->thread_id == sceKernelGetThreadId());
#endif
    const bool relaunched = TakeRelaunchArguments(argc, argv);
    // A restart's arguments are its marker and the launch parameter, taken above. A launch by
    // path (LOADP, SceAppMgr's LaunchAppByPath) hands over the SELF's own path as the
    // first argument, and can carry SceAppMgr's options (-titleid ID and the like) after the
    // title's own; neither is a launch parameter.
    for (int i = 1; i < argc && !relaunched; ++i) {
#ifdef VITA_DIAGNOSTICS
        sceClibPrintf("[azahar] argv[%d] %s\n", i, argv[i] != nullptr ? argv[i] : "(null)");
#endif
        if (argv[i] == nullptr || argv[i][0] == '\0') {
            continue;
        }
        if (IsOwnSelfPath(argv[i])) {
            g_launched_by_path = true;
            continue;
        }
        // SceAppMgr's options start with a dash; a dash alone is the launch parameter's "no
        // title" (LaunchParameter).
        if (argv[i][0] == '-' && argv[i][1] != '\0') {
            i += AppMgrOptionValues(argv[i]);
            continue;
        }
        g_arguments.emplace_back(argv[i]);
    }
#ifdef VITA_DEBUG_TOOLS
    // A scripted launch from a host that cannot pass arguments: ux0:data/azahar/launch.txt holds
    // them one a line (the title's path first, or `-`), and is removed as it is read, so only the
    // launch after it was written takes them. A restart carries them on as any arguments.
    if (g_arguments.empty() && !relaunched) {
        static constexpr const char* LaunchFile = "ux0:data/azahar/launch.txt";
        if (FILE* f = std::fopen(LaunchFile, "rb")) {
            char line[1024];
            while (std::fgets(line, sizeof(line), f) != nullptr) {
                std::string argument{line};
                while (!argument.empty() && (argument.back() == '\n' || argument.back() == '\r' ||
                                             argument.back() == ' ')) {
                    argument.pop_back();
                }
                if (!argument.empty()) {
                    g_arguments.push_back(std::move(argument));
                }
            }
            std::fclose(f);
            sceIoRemove(LaunchFile);
            sceClibPrintf("[azahar] launch.txt: %u arguments\n",
                          static_cast<unsigned>(g_arguments.size()));
        }
    }
    VitaFrontend::EmuWindowVita::keep_display_on = CommandChannel();
#endif
    SetupTerminateHandler();
    Common::Log::SetStopHook(VitaFrontend::CrashReport::BlankDisplay);
    VitaFrontend::CrashReport::PrintLast();
#ifdef VITA_DEBUG_TOOLS
    if (CommandChannel()) {
        VitaFrontend::CrashReport::StartStallWatch();
    }
#endif
    // Before anything is brought up, because the first start after a boot restarts from here.
    g_native_error = LoadAzaharNative(relaunched);
    if (!g_native_error) {
        VitaFrontend::CrashReport::WatchBusErrors();
        // Core 3, which the system keeps from games: before this thread and the rest are pinned
        // there (SetCurrentThreadRole). CapUnlocker's where it is loaded, else the module's own.
        g_cores_unlocked = azaharUnlockCores();
        sceClibPrintf("[azahar] cores: %s (%08x)\n",
                      g_cores_unlocked == AZAHAR_CORES_CAPUNLOCKER ? "CapUnlocker"
                      : g_cores_unlocked == AZAHAR_CORES_OWN       ? "azaharnative"
                                                                   : "not unlocked",
                      static_cast<unsigned>(g_cores_unlocked));
    }
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
    if (!g_native_error) {
        UnloadAzaharNative(azaharVersion());
    }
    return 0;
}

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstddef>
#include "common/vita_data_dir.h"
#include <exception>
#include <memory>
#include <string>
#include <vector>
#include <pthread.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <psp2/appmgr.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/message_dialog.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/shellutil.h>
#include <psp2/vshbridge.h>
#include "citra_vita/heap_prof.h"
#include "video_core/renderer_gxm/gxm_video_options.h"
#include "citra_vita/gxm_present.h"
#ifdef CITRA_VITA_GPUPROF
#include "citra_vita/gpuprof.h"
#endif

#include "citra_vita/bug_report.h"
#include "common/big_arena.h"
#include "citra_vita/emu_window_vita.h"
#include "citra_vita/input_factory_vita.h"

#include "azahar_native.h"
#include "common/named_thread.h"
#include "common/pipeline_stats.h"
#include "core/arm/vita/native_stats.h"
#include "core/arm/vita/mh4u_hooks.h"

#include <cstdint>
#include <cstdio>
#include <malloc.h>
#include <psp2/io/stat.h>

namespace {
// One record per second, appended to ux0:data/mh4u/perf.bin; read the file back with
// src/citra_vita/tools/read_perf.py. The console keeps only the speed line - everything
// else lives here, where a run's whole history survives instead of scrolling away.
#pragma pack(push, 1)
struct PerfFileHeader {
    char magic[4]; // "AZPF"
    std::uint32_t version;
    std::uint32_t record_size;
};

struct PerfRecord {
    std::uint64_t time_us;
    float speed;
    float fps;
    std::uint32_t slices;
    std::uint32_t svcs;
    std::uint64_t guest_ns;
    std::uint32_t gpu_ops;
    std::uint32_t gpu_brakes;
    std::uint64_t gpu_busy_ns;
    std::uint64_t runloop_us;
    /// Wall time inside the azaharRun syscall. Minus guest_ns it is what entering and leaving
    /// the guest costs: the syscall itself, the two world switches and their TLB flushes.
    std::uint64_t vanrun_us;
    std::uint64_t present_us;
    AzaharPmuStats pmu[4]; // zeros when the build carries no CITRA_VITA_PMU
    // Version 2: the emulation thread's HLE and RunSync-drain time, and the GPU thread's
    // swap-op and stripe-barrier time - the second-level decomposition of the two "busy
    // asleep" totals above it.
    std::uint64_t svc_us;
    std::uint64_t drain_us;
    std::uint32_t drain_count;
    std::uint32_t barrier_count;
    std::uint64_t gpu_swap_ns;
    std::uint64_t barrier_ns;
    // Version 4: how long a completion interrupt sits in the render thread's queue before it
    // is posted - what a guest waiting on a fill or transfer has to sit through.
    std::uint64_t irq_lag_us;
    std::uint32_t irq_count;
    std::uint32_t pad4;
};
#pragma pack(pop)
// The reader (tools/read_perf.py) parses this layout by hand; keep the two in lockstep.
static_assert(sizeof(PerfFileHeader) == 12);
static_assert(sizeof(PerfRecord) == 384);

FILE* OpenPerfFile() {
    sceIoMkdir(VITA_DATA_DIR "", 0777);
    FILE* f = std::fopen(VITA_DATA_DIR "/perf.bin", "wb");
    if (f != nullptr) {
        const PerfFileHeader hdr{{'A', 'Z', 'P', 'F'}, 5u, sizeof(PerfRecord)};
        std::fwrite(&hdr, sizeof(hdr), 1, f);
        std::fflush(f);
    }
    return f;
}
} // namespace
#include "citra_vita/hud_stats.h"
#include "citra_vita/vita_ui.h"
#include "common/emu_prof.h"
#include "common/vita_tuning.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/memory.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/service.h"
#include "core/movie.h"
#include "pte_sema.h"
#include "video_core/gpu.h"
#include "video_core/pica/vertex_cache.h"
#include "video_core/renderer_gxm/gxm_flags.h"

// The default heap is nowhere near enough for a 3DS: FCRAM alone is 128 MB, and the caches and
// the surface storage sit on top of it. Newlib reads this at startup.
extern "C" {
// Old-3DS FCRAM is 128 MB (memory.cpp), so the runtime fits in ~200 MB and this leaves room
// in the game partition - about 238 MB of USER_RW, measured with sceKernelGetFreeMemorySize -
// for the thread stacks and the GPU allocations that come out of it alongside this.
// Sized against the extended-memory budget (ATTRIBUTE2=12 in the param.sfo, +109 MB). The
// guest's 128 MB FCRAM and 6 MB VRAM are ordinary mallocs inside this heap and the emulator's
// working set sits on top, which measured a 207 MB peak - so the old 210 MB ran out, and a
// level load's transient took it over into std::bad_alloc.
//
// Not more than this, though: newlib takes the whole heap as one memblock at startup and the
// kernel commits real pages for it, so every megabyte here is a megabyte the *system* cannot
// have. At 300 MB, 100 MB of it never allocated, the console had trouble the moment it wanted
// to start its own services. 240 MB clears the measured peak by ~33 MB and leaves the rest.
unsigned int _newlib_heap_size_user = 240 * 1024 * 1024;

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

constexpr int MinAzaharNativeVersion = 1;

namespace {
/// The process's own arguments, kept for LaunchParameter.
std::vector<std::string> g_arguments;

// Startup trace, to the kernel's printf channel (visible over the module logger / a PC). The
// app was failing to launch with no on-screen detail; this says how far it got.
#define VTRACE(...)                                                                                \
    do {                                                                                           \
        sceClibPrintf("[azahar] " __VA_ARGS__);                                                    \
        sceClibPrintf("\n");                                                                       \
    } while (0)

#ifdef CITRA_VITA_PMU
/// The kernel module's counters as one string: SGI service windows, the last pending mask,
/// windows that timed out, what pends now, the resident loop's heartbeat, and the core-2
/// eviction sweep (threads moved off the core, threads pinned to it alone, threads last run
/// on it). Empty when the module does not answer.
void FormatSgiStats(char* out, std::size_t size) {
    out[0] = '\0';
    AzaharSgiStats st{};
    if (azaharSgiStats(&st) == 0) {
        sceClibSnprintf(out, size, "  guest cpu%u  sgi w%u p%04x s%u pend%04x hb%u", st.core,
                        st.windows, st.last_pend, st.stuck, st.pending_now, st.heartbeat);
    }
}

/// Appended to the render-queue-stuck log (GpuThread::Enqueue), once a second while it lasts:
/// the emulation thread is parked at the brake then and its own per-second line stops.
void StuckReport() {
    char sgi[128];
    FormatSgiStats(sgi, sizeof(sgi));
    const struct mallinfo heap = mallinfo();
    sceClibPrintf("[azahar] stuck: heap %u/%u KiB%s\n", static_cast<unsigned>(heap.uordblks / 1024),
                  static_cast<unsigned>(heap.arena / 1024), sgi);
    // Where every thread is, into native.txt: the render thread's pc is the question.
    static int dumps = 0;
    if (dumps < 3) {
        dumps++;
        sceClibPrintf("[azahar] stuck: azaharThreadDump wrote %d threads to native.txt\n",
                      azaharThreadDump());
    }
}
#endif

#ifdef VITA_ALLOC_TRACE
extern "C" void VitaAllocTraceDump(void);
extern "C" void VitaAllocTraceWatchThread(void);
#else
// So the call sites do not each need a guard: with the tracer off this folds away to nothing.
inline void VitaAllocTraceDump() {}

inline void VitaAllocTraceWatchThread() {}
#endif

constexpr char UserDirectory[] = VITA_DATA_DIR "/";

void SetupPaths() {
    // Must happen before anything asks for a path: the first request bootstraps the whole table
    // from whatever the platform default is, and on this console there is no such thing.
    FileUtil::SetUserPath(UserDirectory);
#ifdef VITA_DIAGNOSTICS
    // Diagnostic builds only: the game's own data - the update, saves and extdata (sdmc), the
    // system files (nand, sysdata) - is used from Azahar's folder in place unless the port has
    // its own copy, so testing needs no copy. A release uses ux0:data/mh4u alone; the port's
    // files (flags, logs, cheats, mods) always live there.
    const std::pair<FileUtil::UserPath, const char*> shared[] = {
        {FileUtil::UserPath::SDMCDir, "sdmc"},
        {FileUtil::UserPath::NANDDir, "nand"},
        {FileUtil::UserPath::SysDataDir, "sysdata"},
    };
    for (const auto& [path, name] : shared) {
        const std::string own = std::string(VITA_DATA_DIR "/") + name;
        const std::string azahar = std::string(VITA_AZAHAR_DIR "/") + name;
        if (!FileUtil::IsDirectory(own) && FileUtil::IsDirectory(azahar)) {
            FileUtil::UpdateUserPath(path, azahar);
        }
    }
#endif
}

/// Names the exception behind an abort. std::terminate goes to newlib's abort(), which raises
/// SIGABRT - the crash dump then points at _kill_r and says nothing about the cause, and azahar's
/// own ASSERT is not it (that traps). std::bad_alloc and the std::system_error that a failing
/// pthread call turns into both arrive here.
void SetupTerminateHandler() {
    std::set_terminate([] {
        std::string what = "no active exception";
        if (std::current_exception()) {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
                what = "an exception not derived from std::exception";
            }
        }
        // Straight to the kernel channel as well as the log: whatever broke may have been the
        // logging thread, and this is the last thing the process does.
        sceClibPrintf("[azahar] std::terminate: %s\n", what.c_str());
        LOG_CRITICAL(Frontend, "std::terminate: {}", what);
        Common::Log::Stop();
        sceKernelExitProcess(1);
    });
}

/// Brings the network stack up now rather than leaving it to the first guest socket call.
///
/// newlib initialises it lazily: socket(), inet_pton(), gethostbyname() and friends all route
/// through _vita_net_init, which loads SCE_SYSMODULE_NET, takes ~140 KB for a net memory pool
/// and calls sceNetInit/sceNetCtlInit. Loading that sysmodule drags in SceNet, SceLibNetCtl,
/// SceLibHttp, SceLibSsl, SceNpManager, SceNpCommon, ScePaf, SceSqliteVsh, SceDbutil and
/// SceDrmPsmKdc - about ten system modules, arriving in the middle of a title the moment its
/// own soc:U or http:C service first touches a host socket, which is exactly where a level
/// load was seen to fall over. Paying for it here means it happens once, before the guest
/// exists, at a moment when nothing is timing-critical and the heap is nearly empty.
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
    filter.ParseFilterString("*:Info");
    Common::Log::SetGlobalFilter(filter);
#endif
    // The release build starts no logging backend at all, so every LOG_* macro reaches a
    // filter that discards it before it formats anything.
}

/// The Vita boots at a conservative clock; a software-rendered 3DS needs every megahertz.
/// Only ever raises a clock: a clock plugin may already run the CPU at 500 MHz (above what
/// the user API can ask for), and asking for 444 here took it back down (measured with the
/// clocks line, 2026-11-15).
void SetupClocks() {
    sceClibPrintf("[mh4u] clocks at start: arm %d bus %d gpu %d xbar %d MHz\n",
                  scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
                  scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    // ux0:data/mh4u/noclocks: leave the clocks entirely to a plugin.
    if (std::FILE* f = std::fopen(VITA_DATA_DIR "/noclocks", "rb")) {
        std::fclose(f);
        return;
    }
    if (scePowerGetArmClockFrequency() < 444) {
        scePowerSetArmClockFrequency(444);
    }
    if (scePowerGetBusClockFrequency() < 222) {
        scePowerSetBusClockFrequency(222);
    }
    if (scePowerGetGpuClockFrequency() < 222) {
        scePowerSetGpuClockFrequency(222);
    }
    if (scePowerGetGpuXbarClockFrequency() < 166) {
        scePowerSetGpuXbarClockFrequency(166);
    }
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
        return "the CPU could not start: load the azahar-native kernel module (ux0:tai/config.txt) "
               "and reboot";
    default:
        return "unknown error";
    }
}

/// Stops the render thread, releases the GPU's view of guest memory, and shuts the system
/// down - in that order, because the unmap must run while guest RAM is still allocated and
/// while no other thread is inside the GXM context.
void ShutdownSystem(Core::System& system) {
    system.GPU().StopRenderThread();
    VitaFrontend::GxmPresent::UnmapAllGuestMemory();
    system.Shutdown();
}

/// Runs one title to completion. Returns false when the user wants the application to close
/// rather than to return to the game list.

// ---- The port's own data folder (ux0:data/mh4u) and how it is first filled -------------------

/// The game image: the first .3ds/.cci/.cxi/.app in ux0:data/mh4u/roms; diagnostic builds also
/// take one in Azahar's roms folder whose name says Monster Hunter 4.
std::string FindRom() {
    const auto search = [](const char* folder, bool only_mh4u) {
        std::string found;
        const SceUID dir = sceIoDopen(folder);
        if (dir < 0) {
            return found;
        }
        SceIoDirent entry{};
        while (found.empty() && sceIoDread(dir, &entry) > 0) {
            std::string lower = entry.d_name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (SCE_S_ISDIR(entry.d_stat.st_mode) ||
                (only_mh4u && lower.find("monster hunter 4") == std::string::npos)) {
                continue;
            }
            for (const char* ext : {".3ds", ".cci", ".cxi", ".app"}) {
                const std::size_t n = std::strlen(ext);
                if (lower.size() > n && lower.compare(lower.size() - n, n, ext) == 0) {
                    found = std::string(folder) + "/" + entry.d_name;
                    break;
                }
            }
        }
        sceIoDclose(dir);
        return found;
    };
    std::string rom = search(VITA_DATA_DIR "/roms", false);
#ifdef VITA_DIAGNOSTICS
    if (rom.empty()) {
        rom = search(VITA_AZAHAR_DIR "/roms", true);
    }
#endif
    return rom;
}

/// The port's cheat file (USA) ships in the vpk and is copied to ux0:data/mh4u/cheats on first
/// start; a file already there (the player's own choices) is kept. Only the 30 fps cap is on: the
/// game wants 60 until the Heavenfall mod's FPS setting is chosen in its START menu, which a new
/// player only reaches after the prologue. It has a block for the v1.1 executable and one for
/// v1.0 (the web installer's build; its pointer 8 bytes lower), each behind its own version check.
/// 3D and bloom stay off (the mod's installer defaults already do them). Each code first checks a v1.1 instruction, so on the game without its
/// update - whose code sits elsewhere - it does nothing.
void InstallDefaultCheats() {
    constexpr char Name[] = "0004000000126300.txt";
    const std::string dest = std::string(VITA_DATA_DIR "/cheats/") + Name;
    SceIoStat st{};
    if (sceIoGetstat(dest.c_str(), &st) >= 0) {
        return;
    }
    sceIoMkdir(VITA_DATA_DIR "/cheats", 0777);
    const SceUID in = sceIoOpen((std::string("app0:cheats/") + Name).c_str(), SCE_O_RDONLY, 0);
    if (in < 0) {
        return;
    }
    std::vector<char> data(16 * 1024);
    const int n = sceIoRead(in, data.data(), static_cast<SceSize>(data.size()));
    sceIoClose(in);
    if (n <= 0) {
        return;
    }
    const SceUID out = sceIoOpen(dest.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out >= 0) {
        sceIoWrite(out, data.data(), static_cast<SceSize>(n));
        sceIoClose(out);
    }
}

/// Installs the CIAs in ux0:data/mh4u/roms that are not installed yet (the game's update, DLC),
/// with progress on screen: the Heavenfall mod's executable is built for the update, so it has to
/// be there before the game boots. An installed title is recognised by its TMD and skipped.
void InstallPendingCias(VitaFrontend::EmuWindowVita& window, VitaFrontend::Ui& ui) {
    const std::string folder = VITA_DATA_DIR "/roms";
    std::vector<std::string> cias;
    const SceUID dir = sceIoDopen(folder.c_str());
    if (dir < 0) {
        return;
    }
    SceIoDirent entry{};
    while (sceIoDread(dir, &entry) > 0) {
        std::string lower = entry.d_name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!SCE_S_ISDIR(entry.d_stat.st_mode) && lower.size() > 4 &&
            lower.compare(lower.size() - 4, 4, ".cia") == 0) {
            cias.push_back(folder + "/" + entry.d_name);
        }
    }
    sceIoDclose(dir);
    for (const std::string& path : cias) {
        const auto infos = Service::AM::GetCIAInfos(path);
        if (infos.Failed()) {
            LOG_WARNING(Frontend, "{} is not an installable CIA", path);
            continue;
        }
        const u64 tid = infos.Unwrap().first.tid;
        // An install that was cut short (the app closed, the console slept) leaves its title
        // metadata behind, which would read as installed. A marker is written before the install
        // and removed only after it succeeds: while it exists, the title is installed again.
        const std::string marker = fmt::format(VITA_DATA_DIR "/installing_{:016X}", tid);
        SceIoStat marker_stat{};
        const bool interrupted = sceIoGetstat(marker.c_str(), &marker_stat) >= 0;
        if (!interrupted && FileUtil::Exists(Service::AM::GetTitleMetadataPath(
                                Service::FS::MediaType::SDMC, tid))) {
            continue;
        }
        if (interrupted) {
            LOG_WARNING(Frontend, "{:016X}: the last install did not finish, installing again", tid);
        } else {
            const SceUID mf =
                sceIoOpen(marker.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
            if (mf >= 0) {
                sceIoWrite(mf, path.c_str(), static_cast<SceSize>(path.size()));
                sceIoClose(mf);
            }
        }
        const std::string name = path.substr(path.find_last_of('/') + 1);
        u64 last_shown = 0;
        const auto show = [&](std::size_t done, std::size_t total) {
            const u64 now = sceKernelGetProcessTimeWide();
            if (now - last_shown < 250000 && done != total) {
                return;
            }
            last_shown = now;
            ui.SetStatusMessage(fmt::format("Installing {} ({}%)...\n\nPlease don't close the "
                                            "app until it finishes.",
                                            name, total != 0 ? done * 100 / total : 0));
            window.PollEvents();
            ui.Update();
            window.Present(&ui);
        };
        show(0, 1);
        const auto status = Service::AM::InstallCIA(path, show);
        LOG_INFO(Frontend, "installed {} ({:016X}): {}", name, tid, static_cast<u32>(status));
        if (status == Service::AM::InstallStatus::Success) {
            sceIoRemove(marker.c_str());
        } else {
            ui.SetStatusMessage(fmt::format("Could not install {} (error {}). It has to be a "
                                            "decrypted CIA.",
                                            name, static_cast<u32>(status)));
            for (int i = 0; i < 180; i++) { // ~3 s on screen
                window.PollEvents();
                ui.Update();
                window.Present(&ui);
            }
        }
        ui.ClearStatusMessage();
    }
}

bool RunTitle(Core::System& system, VitaFrontend::EmuWindowVita& window, VitaFrontend::Ui& ui,
              const std::string& path) {
    VTRACE("RunTitle: %s", path.c_str());

    window.UpdateCurrentFramebufferLayout();

    ui.SetStatusMessage("Loading...");
    ui.Update();
    window.Present(&ui);

    VTRACE("RunTitle: system.Load");
    const auto load_result = system.Load(window, path);
    VTRACE("RunTitle: system.Load -> %d", static_cast<int>(load_result));
    VitaAllocTraceDump(); // what the load itself allocated
    {
        // ballast (test): ux0:data/mh4u/ballast holding a number of MiB to allocate and keep,
        // in 64 KiB blocks, so a fresh state load runs with the heap as full as a long play
        // session leaves it (does a nearly full heap slow the game?). Never in normal use.
        static std::vector<void*> ballast;
        if (std::FILE* f = std::fopen(VITA_DATA_DIR "/ballast", "rb")) {
            char text[16] = {};
            std::fread(text, 1, sizeof(text) - 1, f);
            std::fclose(f);
            const int mib = std::atoi(text);
            for (int i = 0; i < mib * 16; i++) {
                void* block = std::malloc(64 * 1024);
                if (block == nullptr) {
                    break;
                }
                std::memset(block, 0, 64 * 1024);
                ballast.push_back(block);
            }
            sceClibPrintf("[mh4u] ballast: %u KiB held\n",
                          static_cast<unsigned>(ballast.size() * 64));
        }
    }
    if (load_result != Core::System::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Could not load {}: {}", path, DescribeLoadError(load_result));
        ui.SetStatusMessage("Could not load this title: " + DescribeLoadError(load_result));
        for (int i = 0; i < 240; i++) {
            window.PollEvents();
            ui.Update();
            window.Present(&ui);
        }
        ui.ClearStatusMessage();
        return true;
    }
    ui.ClearStatusMessage();

    // The window presents straight out of guest memory, so the renderer can skip its
    // screen_infos conversion entirely.
    // Applied to this GPU and, through the configurator, to the one a state load rebuilds.
    const auto configure_gpu = [](VideoCore::GPU& gpu) {
        gpu.Renderer().SetFrontendPresentsGuestMemory(true);
        // Bring-up switch: frameskip drops top-screen draws when the render thread is behind,
        // which is indistinguishable from a renderer that draws nothing until it is turned off.
        gpu.SetFrameskipAllowed(!GxmRenderer::GxmFlag("noskip"));
        // Bring-up switch: every operation the emulation thread queues is finished before it
        // goes on, so nothing the guest writes can change under a draw the render thread has
        // not run yet. A picture that comes right this way was being raced.
        gpu.SetRenderThreadLockstep(GxmRenderer::GxmFlag("lockstep"));
    };
    configure_gpu(system.GPU());
    system.SetGpuConfigurator(configure_gpu);
    // A state load frees and reallocates guest RAM (and the GPU's display snapshots) inside
    // the core: the GPU's mappings of them go first, as in ShutdownSystem. They are mapped
    // again on the next present.
    system.SetBeforeShutdown([] { VitaFrontend::GxmPresent::UnmapAllGuestMemory(); });
    // States are refused across builds by default, and every commit is another build: each
    // update would orphan the player's states. What has to match is the archive layout, not
    // the revision (the diag autoload has always relied on that).
    system.SetLoadStateAnyBuild(true);

    VTRACE("RunTitle: StartRenderThread");
    system.GPU().StartRenderThread();
    ui.EnterGame();
    // ux0:data/mh4u/autoload.txt names a save-state slot to load once the game has run for
    // ten seconds - the same request the pause menu makes - so speed can be measured from a
    // state without driving the menu (test scripts write the file before launching and delete
    // it once started). In every build. Requested earlier (before the render thread, or on the
    // first frames) the reload could not take the native core back.
    int autoload_slot = 0;
    if (FILE* f = std::fopen(VITA_DATA_DIR "/autoload.txt", "rb")) {
        char buf[16]{};
        std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        autoload_slot = std::atoi(buf);
    }
    const SceUInt64 autoload_at = sceKernelGetProcessTimeWide() + 10'000'000ull;
    // From here the GXM presentation layer belongs to the render thread, which draws the
    // interface along with the frame it has just finished.
    window.SetUi(&ui);
    VTRACE("RunTitle: entering run loop");

    bool keep_running = true;
    bool combo_held = false;
    SceUInt64 last_poll_us = 0;
    SceUInt64 last_event_us = 0;
    u64 last_wall_us = 0;
    u64 stopped_at_us = 0;
    while (!window.IsQuitRequested()) {
        // The pad and touch panel, every 2 ms at most: RunLoop returns several thousand times a
        // second, and the two peeks cost ~30 us together - a sixth of the emulation thread's
        // time in a movie. The 3DS's own HID samples every 4 ms or so.
        if (const SceUInt64 now = sceKernelGetProcessTimeWide(); now - last_poll_us >= 2000) {
            last_poll_us = now;
            window.PollEvents();
        }

        // Edge triggered: Select + Start together opens the pause menu (the PS button belongs
        // to the shell). Coming back from the shell opens it too, so the title waits for the
        // player instead of running on from wherever it was suspended.
        const u32 buttons = window.ButtonsHeld();
        constexpr u32 PauseCombo = SCE_CTRL_SELECT | SCE_CTRL_START;
        const bool combo_now = (buttons & PauseCombo) == PauseCombo;
        bool resumed = false;
        // A resume event alone is not proof of a suspend: one arrived mid-load with the app
        // never having left the screen (2026-11-14), and paused the game. It counts only when
        // the wall clock also jumped by two seconds or more between two of these checks (they
        // run every 100 ms) - the process was stopped - within the last second.
        if (const SceUInt64 now = sceKernelGetProcessTimeWide(); now - last_event_us >= 100000) {
            last_event_us = now;
            SceRtcTick tick{};
            sceRtcGetCurrentTick(&tick);
            const u64 wall_us = tick.tick;
            if (last_wall_us != 0 && wall_us - last_wall_us >= 2000000) {
                stopped_at_us = wall_us;
            }
            last_wall_us = wall_us;
            SceAppMgrSystemEvent event{};
            while (sceAppMgrReceiveSystemEvent(&event) == 0) {
                if (event.systemEvent == SCE_APPMGR_SYSTEMEVENT_ON_RESUME) {
                    // A real return from the shell delivers its resume right after the jump; a
                    // window of 5 s also caught our own stalls (a state load's ~12 s) followed
                    // by an unrelated resume event a few seconds later (vita-agent waking the
                    // console in scripted runs opened the menu over 1 in ~8 loads, 2026-11-19).
                    const bool was_stopped = stopped_at_us != 0 && wall_us - stopped_at_us < 1000000;
                    sceClibPrintf("[mh4u] resume event: %s (%u ms after a %s)\n",
                                  was_stopped ? "pausing" : "ignored",
                                  stopped_at_us != 0
                                      ? static_cast<unsigned>((wall_us - stopped_at_us) / 1000)
                                      : 0u,
                                  stopped_at_us != 0 ? "2 s clock jump" : "run without jumps");
                    LOG_INFO(Frontend, "resume event ({})",
                             was_stopped ? "after a stop" : "ignored: no stop seen");
                    resumed |= was_stopped;
                }
            }
        }
        if (((combo_now && !combo_held) || resumed) && !ui.MenuOpen()) {
            if (resumed) {
                LOG_INFO(Frontend, "resumed from the shell: pausing");
            }
            ui.OpenPauseMenu();
        }
        combo_held = combo_now;

        if (ui.MenuOpen()) {
            // Emulation does not advance while a menu is up, so no frames arrive to carry a
            // present: ask the render thread, which owns presentation, for one of its own. Waiting
            // for it costs nothing here, because nothing is running to be held up.
            system.GPU().RunOnRenderThread([&] { window.Present(&ui); });
            const auto action = static_cast<VitaFrontend::Ui::Action>(window.TakeUiAction());
            switch (action) {
            case VitaFrontend::Ui::Action::QuitGame:
                keep_running = true;
                window.SetInputToGuest(true);
                // The render thread is about to stop; take the interface back first, so nothing
                // there reaches for it while it goes.
                window.SetUi(nullptr);
                ShutdownSystem(system);
                return keep_running;
            case VitaFrontend::Ui::Action::ExitApplication:
                window.SetUi(nullptr);
                ShutdownSystem(system);
                return false;
            case VitaFrontend::Ui::Action::SaveState:
                system.SendSignal(Core::System::Signal::Save, ui.SelectedSlot());
                ui.EnterGame();
                break;
            case VitaFrontend::Ui::Action::LoadState:
                system.SendSignal(Core::System::Signal::Load, ui.SelectedSlot());
                ui.EnterGame();
                break;
            case VitaFrontend::Ui::Action::SaveReport:
                ui.SetReportResult(VitaFrontend::WriteBugReport(ui.ReportWithState()));
                break;
            default:
                break;
            }
            continue;
        }

        if (autoload_slot >= 1 && sceKernelGetProcessTimeWide() >= autoload_at) {
            if (!system.SendSignal(Core::System::Signal::Load, static_cast<u32>(autoload_slot))) {
                LOG_WARNING(Frontend, "autoload.txt: could not request slot {}", autoload_slot);
            }
            autoload_slot = 0;
        }

        // Periodic snapshot on the emulation thread, never from the allocation path.
        static unsigned van_trace_tick = 0;
        if ((++van_trace_tick % 600u) == 0u) {
            VitaAllocTraceDump();
        }
        const SceUInt64 runloop_start = sceKernelGetProcessTimeWide();
        static SceUInt64 last_iter_end = 0;
        if (Common::EmuProf::enabled && last_iter_end != 0) {
            Common::EmuProf::Add("main: between runloops", runloop_start - last_iter_end);
        }
        const auto status = system.RunLoop();
        last_iter_end = sceKernelGetProcessTimeWide();
        Core::NativeStats::runloop_us.fetch_add(last_iter_end - runloop_start,
                                                std::memory_order_relaxed);
        if (Common::EmuProf::enabled) {
            Common::EmuProf::Add("main: runloop", last_iter_end - runloop_start);
        }
        if (status == Core::System::ResultStatus::ErrorSavestate) {
            // A refused save or load is not fatal; the title keeps running.
            LOG_ERROR(Frontend, "Savestate operation failed");
        } else if (status != Core::System::ResultStatus::Success) {
            LOG_CRITICAL(Frontend, "Emulation stopped: {}", static_cast<int>(status));
            break;
        }

        // Once a second: the speed line to the console, the full record - GPU health, the
        // emulation thread's time split, all four cores' PMU counters - to the perf blob.
        // Everything measured once a second, and where it is printed. Compiled in only
        // for the diagnostic build: the release build takes no clock readings, drains no
        // counters, and prints nothing.
        {
            static SceUInt64 last_perf_us = 0;
            const SceUInt64 now_us = sceKernelGetProcessTimeWide();
            if (now_us - last_perf_us >= 1000000ull) {
                last_perf_us = now_us;
                const auto perf = system.GetAndResetPerfStats();
                const int speed = static_cast<int>(perf.emulation_speed * 100.0 + 0.5);
                const int fps10 = static_cast<int>(perf.game_fps * 10.0 + 0.5);

                const unsigned presented =
                    Common::PipelineStats::presents.exchange(0, std::memory_order_relaxed);

#ifdef VITA_DIAGNOSTICS
                // Frames the guest ran whose draws were dropped: the difference between
                // this and the game's frame rate is what the renderer actually drew, which
                // is the number to read against the presented rate.
                static u64 skipped_total = 0;
                const u64 skipped_now = system.GPU().SkippedFrames();
                const unsigned skipped = static_cast<unsigned>(skipped_now - skipped_total);
                skipped_total = skipped_now;
                static FILE* const perf_file = OpenPerfFile();

                PerfRecord rec{};
                rec.time_us = now_us;
                rec.speed = static_cast<float>(perf.emulation_speed);
                rec.fps = static_cast<float>(perf.game_fps);
                rec.slices = Core::NativeStats::slices.exchange(0);
                rec.svcs = Core::NativeStats::svcs.exchange(0);
                rec.guest_ns = Core::NativeStats::credited_ns.exchange(0);
                rec.gpu_ops = Common::PipelineStats::gpu_ops.exchange(0);
                rec.gpu_brakes = Common::PipelineStats::gpu_brakes.exchange(0);
                rec.gpu_busy_ns = Common::PipelineStats::gpu_busy_us.exchange(0) * 1000ull;
                rec.runloop_us = Core::NativeStats::runloop_us.exchange(0);
                rec.vanrun_us = Core::NativeStats::vanrun_us.exchange(0);
                rec.present_us = Core::NativeStats::present_us.exchange(0);
                rec.svc_us = Core::NativeStats::svc_us.exchange(0);
                rec.drain_us = Common::PipelineStats::drain_us.exchange(0);
                rec.drain_count = Common::PipelineStats::drain_count.exchange(0);
                rec.gpu_swap_ns = Common::PipelineStats::gpu_swap_us.exchange(0) * 1000ull;
                rec.barrier_ns = Common::PipelineStats::barrier_us.exchange(0) * 1000ull;
                rec.barrier_count = Common::PipelineStats::barrier_count.exchange(0);
                rec.irq_lag_us = Common::PipelineStats::interrupt_lag_us.exchange(0);
                rec.irq_count = Common::PipelineStats::interrupt_count.exchange(0);
#ifdef CITRA_VITA_PMU
                {
                    AzaharPmuAll pmu;
                    if (azaharPmuReadAll(&pmu) == 0) {
                        for (unsigned c = 0; c < 4; c++) {
                            rec.pmu[c] = pmu.core[c];
                        }
                    }
                }
#endif
                // sceClibPrintf has no reliable float formatting, so fixed-point by hand.
                // Live heap, because a run that lasts long enough dies in std::bad_alloc and
                // the rate of climb here is the only thing that says what is leaking. uordblks
                // is what newlib has handed out and not got back; arena is what it took from
                // the kernel, which only grows.
                const struct mallinfo heap = mallinfo();
                // Both empty unless the Vita tracing is built in.
                char sema[64] = {};
                char phase[128] = {};
                // azahar-native's SGI service windows and core 2's heartbeat: a freeze log's last
                // second says whether core 2 was still polling between commands, whether a
                // cross-core call was pending, and whether a window ever timed out.
                // Like azaharPmuReadAll, an import only a module new enough exports, so it sits
                // behind the same VITA_PMU option (under Vita3K, which has no module, the
                // import resolves to a stub that returns 0 and the fields print as zeros).
                char sgi[128] = {};
#ifdef CITRA_VITA_PMU
                FormatSgiStats(sgi, sizeof(sgi));
#endif
#ifdef VITA_ALLOC_TRACE
                unsigned sema_created = 0, sema_deleted = 0, sema_refused = 0;
                VitaSemaTotals(&sema_created, &sema_deleted, &sema_refused);
                sceClibSnprintf(sema, sizeof(sema), "  sema %d/%d (%u-%u)", VitaSemaLive(),
                                VitaSemaPeak(), sema_created, sema_deleted);
#endif
#ifdef VITA_RENDER_TRACE
                // The render thread's breadcrumb. It hangs without printing anything itself,
                // and this line keeps coming for the few seconds before the queue reaches its
                // ceiling and the brake parks the emulation thread too - so a phase that stops
                // advancing while its age climbs names the call that never returned.
                {
                    const char* const where =
                        Common::PipelineStats::render_phase.load(std::memory_order_relaxed);
                    const u64 since =
                        Common::PipelineStats::render_phase_us.load(std::memory_order_relaxed);
                    const u64 now = Common::PipelineStats::NowUs();
                    sceClibSnprintf(
                        phase, sizeof(phase), "  render %s %llu us [%llu %llu %llu]", where,
                        since != 0 && now > since ? now - since : 0,
                        Common::PipelineStats::render_phase_a.load(std::memory_order_relaxed),
                        Common::PipelineStats::render_phase_b.load(std::memory_order_relaxed),
                        Common::PipelineStats::render_phase_c.load(std::memory_order_relaxed));
                }
#endif
                const auto wt_stats = system.Memory().WriteTracker().TakeStats();
                sceClibPrintf(
                    "[azahar] speed %d%%  game %d.%d fps  shown %u fps  skipped %u  dup %u  bottom %u  limiter %u us  gx %u us/%u  "
                    "slices %u (svc %u fast %u)  guest %u us  in %u us  wf %u wa %u wh %u  "
                    "queue %u brake %u us  heap %u/%u KiB  skipsig p%u b%u a%u x%u sup %u%s%s%s\n",
                    speed, fps10 / 10, fps10 % 10, presented, skipped,
                    Common::PipelineStats::dup_presents.exchange(0, std::memory_order_relaxed),
                    Common::PipelineStats::bottom_draws_dropped.exchange(0, std::memory_order_relaxed),
                    Common::PipelineStats::limiter_sleep_us.exchange(0, std::memory_order_relaxed),
                    Common::PipelineStats::gx_list_us.exchange(0, std::memory_order_relaxed),
                    Common::PipelineStats::gx_lists.exchange(0, std::memory_order_relaxed), rec.slices, rec.svcs, static_cast<unsigned>(Core::NativeStats::fast_svcs.exchange(0)),
                    static_cast<unsigned>(rec.guest_ns / 1000),
                    static_cast<unsigned>(rec.vanrun_us),
                    Core::NativeStats::write_faults.exchange(0, std::memory_order_relaxed),
                    static_cast<unsigned>(wt_stats.arms), static_cast<unsigned>(wt_stats.hot_skips),
                    system.GPU().RenderQueueDepth(), rec.gpu_brakes,
                    static_cast<unsigned>(heap.uordblks / 1024),
                    static_cast<unsigned>(heap.arena / 1024),
                    Common::PipelineStats::skipsig_presents.exchange(0, std::memory_order_relaxed),
                    Common::PipelineStats::skipsig_backlog.exchange(0, std::memory_order_relaxed),
                    Common::PipelineStats::skipsig_arena.exchange(0, std::memory_order_relaxed),
                    Common::PipelineStats::skipsig_armed.exchange(0, std::memory_order_relaxed),
                    Common::PipelineStats::presents_superseded.exchange(0, std::memory_order_relaxed),
                    sema, phase, sgi);
                {
                    // MH4U cover trap: traps taken, cover key changes, layout walk us (mean/worst),
                    // armed frames, mispredicts, units hidden, proofs covered.
                    const Mh4uHooks::CoverStats cov = Mh4uHooks::TakeCoverStats();
                    if (cov.traps != 0) {
                        sceClibPrintf("[mh4u] cov t%u k%u w%u/%u a%u m%u u%u p%u\n", cov.traps,
                                      cov.key_changes, cov.walk_avg_us, cov.walk_max_us,
                                      cov.armed, cov.mispredicts, cov.hidden, cov.proofs);
                    }
                }
                if (Common::EmuProf::enabled) {
                    sceClibPrintf("[emuprof]%s\n", Common::EmuProf::TakeReport(GxmRenderer::GxmFlag("pcsample") ? 60 : 30).c_str());
                }
                if (Pica::GetVertexCache().enabled) {
                    const auto vc = Pica::GetVertexCache().TakeStats();
                    sceClibPrintf("[vtxcache] %u/%u hits, %u inserts, %u invalidated, %u resets, "
                                  "%u entries, %u KiB used\n",
                                  vc.hits, vc.lookups, vc.inserts, vc.invalidations, vc.resets,
                                  vc.entries, vc.pool_kib);
                }
                if (perf_file != nullptr) {
                    std::fwrite(&rec, sizeof(rec), 1, perf_file);
                    std::fflush(perf_file);
                }
#else
                // Release: one short line a second, so a console log still tells the speed -
                // and which thread holds it back: the GX thread's command-list time and the
                // render thread's busy time (it times one op in eight here), in ms a second.
                const unsigned gx_ms = Common::PipelineStats::gx_list_us.exchange(
                                           0, std::memory_order_relaxed) / 1000;
                const unsigned render_ms = static_cast<unsigned>(
                    Common::PipelineStats::gpu_busy_us.exchange(0, std::memory_order_relaxed) *
                    8 / 1000);
                // vc: the vertex cache's hits/lookups, inserts, invalidations, resets, entries,
                // pool use and keys refused as volatile, this second.
                const auto vc = Pica::GetVertexCache().TakeStats();
                // skip: vblanks each frameskip signal was on (presents, backlog, arenas), vblanks
                // that armed a skip, and presents dropped as superseded on the render thread.
                VitaFrontend::ReportPrintf("[mh4u] speed %d%%  game %d.%d fps  shown %u fps  gx %u ms  "
                              "render ~%u ms  skip p%u b%u a%u x%u sup %u  fs %u ms %u r %u KiB async %u ms  "
                              "vc %u/%u i%u x%u r%u e%u %uK v%u t%u p%u  gxc l%uK d%u s%u v%uK c%uK gm%uK\n",
                              speed, fps10 / 10, fps10 % 10, presented, gx_ms, render_ms,
                              Common::PipelineStats::skipsig_presents.exchange(0, std::memory_order_relaxed),
                              Common::PipelineStats::skipsig_backlog.exchange(0, std::memory_order_relaxed),
                              Common::PipelineStats::skipsig_arena.exchange(0, std::memory_order_relaxed),
                              Common::PipelineStats::skipsig_armed.exchange(0, std::memory_order_relaxed),
                              Common::PipelineStats::presents_superseded.exchange(0, std::memory_order_relaxed),
                              Common::PipelineStats::fs_read_us.exchange(0, std::memory_order_relaxed) / 1000,
                              Common::PipelineStats::fs_reads.exchange(0, std::memory_order_relaxed),
                              Common::PipelineStats::fs_read_kib.exchange(0, std::memory_order_relaxed),
                              Common::PipelineStats::fs_async_us.exchange(0, std::memory_order_relaxed) / 1000,
                              vc.hits, vc.lookups, vc.inserts, vc.invalidations, vc.resets,
                              vc.entries, vc.pool_kib, vc.volatile_keys, vc.tracked, vc.prunes,
                              Common::PipelineStats::gxc_cmd_bytes.exchange(0, std::memory_order_relaxed) / 1024,
                              Common::PipelineStats::gxc_draws.exchange(0, std::memory_order_relaxed),
                              Common::PipelineStats::gxc_ships.exchange(0, std::memory_order_relaxed),
                              Common::PipelineStats::gxc_vtx_bytes.exchange(0, std::memory_order_relaxed) / 1024,
                              Common::PipelineStats::gxc_code_bytes.exchange(0, std::memory_order_relaxed) / 1024,
                              Common::PipelineStats::gs_memo_kib.load(std::memory_order_relaxed));
                // cpu: the CPU time each named thread actually ran this second (gx above is
                // wall time, preemption and waits included).
                {
                    // wait: GX-thread blocked ms by cause (shade ring, queue brake, vertex
                    // ring, arenas); ps: pre-shade used/stolen/waited and the render thread's
                    // own shading ms; memo: GS memo hits/misses (all mirrors).
                    namespace PS = Common::PipelineStats;
                    const auto take = [](auto& a) { return a.exchange(0, std::memory_order_relaxed); };
                    const Mh4uHooks::CoverStats cov = Mh4uHooks::TakeCoverStats();
                    VitaFrontend::ReportPrintf("[mh4u] cpu%s  wait s%u b%u r%u a%u  ps u%u s%u w%u l%u  memo %u/%u  "
                                  "re l%u x%u t%u  bot d%u x%u  fx %u  hud2d p%u d%u s%u g%u c%u  hudres r%u f%u b%u a%uK z%uK  op d%u/%u a%uK  "
                                  "cov t%u k%u w%u/%u a%u m%u u%u p%u\n",
                                  Common::ThreadCpuReport().c_str(),
                                  static_cast<unsigned>(take(PS::gxw_shade_us) / 1000),
                                  static_cast<unsigned>(take(PS::gxw_brake_us) / 1000),
                                  static_cast<unsigned>(take(PS::gxw_ring_us) / 1000),
                                  static_cast<unsigned>(take(PS::gxw_arena_us) / 1000),
                                  static_cast<unsigned>(take(PS::preshade_used)),
                                  static_cast<unsigned>(take(PS::preshade_stolen)),
                                  static_cast<unsigned>(take(PS::preshade_waited)),
                                  static_cast<unsigned>(take(PS::preshade_local_us) / 1000),
                                  static_cast<unsigned>(take(PS::gs_memo_hit_n)),
                                  static_cast<unsigned>(take(PS::gs_memo_miss_n)),
                                  // Second-eye refusals (lists, transfers) and top-sized
                                  // transfers; bottom-screen draws and transfers dropped;
                                  // post-process draws dropped (decomp plan, P1-P3).
                                  static_cast<unsigned>(take(PS::re_lists_blocked)),
                                  static_cast<unsigned>(take(PS::re_xfers_blocked)),
                                  static_cast<unsigned>(take(PS::top_xfers)),
                                  static_cast<unsigned>(take(PS::bottom_draws_dropped)),
                                  static_cast<unsigned>(take(PS::bottom_xfers_dropped)),
                                  static_cast<unsigned>(take(PS::fx_dropped)),
                                  static_cast<unsigned>(take(PS::hud_post_draws)),
                                  static_cast<unsigned>(take(PS::hud_post_depth)),
                                  static_cast<unsigned>(take(PS::hud_post_scenes)),
                                  static_cast<unsigned>(take(PS::hud_grades)),
                                  static_cast<unsigned>(take(PS::hud_cut)),
                                  static_cast<unsigned>(take(PS::hudres_draws)),
                                  static_cast<unsigned>(take(PS::hudres_frames)),
                                  static_cast<unsigned>(take(PS::hudres_resolves)),
                                  static_cast<unsigned>(take(PS::hud_post_area)),
                                  static_cast<unsigned>(take(PS::hud_post_area_zero)),
                                  // Opaque 2D sprites drawn unblended: draws / candidate
                                  // (blended software) 2D draws, their area (K guest px).
                                  static_cast<unsigned>(take(PS::op2d_draws)),
                                  static_cast<unsigned>(take(PS::op2d_cands)),
                                  static_cast<unsigned>(take(PS::op2d_area)),
                                  // MH4U cover trap: traps taken, cover key changes, layout
                                  // walk us (mean/worst), armed frames, mispredicts, units
                                  // hidden (most at once), proofs covered -
                                  // docs/plans/vita-mh4u-menu-world-skip.md.
                                  cov.traps, cov.key_changes, cov.walk_avg_us, cov.walk_max_us,
                                  cov.armed, cov.mispredicts, cov.hidden, cov.proofs);
                    if (PS::stat_timing) {
                        // Render thread, ms this second: draw ops (all), their register
                        // replay, software-shaded draws (incl. the pre-shade feed), the feed
                        // alone and its triangles.
                        sceClibPrintf("[mh4u] rt ops %u %u ms apply %u sw %u (%u draws) feed %u "
                                      "(%u tris) local %u running %u wait %u other %u  interp %u\n",
                                      static_cast<unsigned>(take(PS::rt_ops)),
                                      static_cast<unsigned>(take(PS::rt_op_us) / 1000),
                                      static_cast<unsigned>(take(PS::rt_apply_us) / 1000),
                                      static_cast<unsigned>(take(PS::sw_shaded_us) / 1000),
                                      static_cast<unsigned>(take(PS::sw_shaded_draws)),
                                      static_cast<unsigned>(take(PS::preshade_feed_us) / 1000),
                                      static_cast<unsigned>(take(PS::preshade_tris)),
                                      static_cast<unsigned>(take(PS::preshade_local_us) / 1000),
                                      static_cast<unsigned>(take(PS::preshade_steal_running_us) / 1000),
                                      static_cast<unsigned>(take(PS::preshade_wait_us) / 1000),
                                      static_cast<unsigned>(take(PS::preshade_other_us) / 1000),
                                      static_cast<unsigned>(take(PS::jit_interp_runs)));
                        sceClibPrintf("[mh4u] psw apply %u | gs %u draws (%u hits) %u verts: "
                                      "total %u key %u setup %u loop %u (load %u vs %u gs %u)\n",
                                      static_cast<unsigned>(take(PS::psw_apply_us) / 1000),
                                      static_cast<unsigned>(take(PS::psw_draws)),
                                      static_cast<unsigned>(take(PS::psw_hits)),
                                      static_cast<unsigned>(take(PS::psw_vertices)),
                                      static_cast<unsigned>(take(PS::psw_total_us) / 1000),
                                      static_cast<unsigned>(take(PS::psw_key_us) / 1000),
                                      static_cast<unsigned>(take(PS::psw_setup_us) / 1000),
                                      static_cast<unsigned>(take(PS::psw_loop_us) / 1000),
                                      static_cast<unsigned>(take(PS::psw_load_us) / 1000),
                                      static_cast<unsigned>(take(PS::psw_vs_us) / 1000),
                                      static_cast<unsigned>(take(PS::psw_gs_us) / 1000));
                        std::string rows;
                        for (u32 r = 0; r < 96; r++) {
                            const u32 n = take(PS::vs_frow_changes[r]);
                            if (n >= 100) {
                                rows += fmt::format(" {}:{}", r, n);
                            }
                        }
                        sceClibPrintf("[mh4u] vs uniform rows changed/s:%s\n", rows.c_str());
                    }
                }
                static unsigned heap_prof_seconds = 0;
                if (HeapProfEnabled() && ++heap_prof_seconds % 10 == 0) {
                    HeapProfReport();
                }
                if (Common::EmuProf::enabled) {
                    sceClibPrintf("[mh4u] gxprof%s\n", Common::EmuProf::TakeReport(24).c_str());
                }
#endif // VITA_DIAGNOSTICS
                // The clocks actually in effect (a clock plugin may override SetupClocks),
                // every 30 s.
                static unsigned clock_lines = 0;
                if (clock_lines++ % 30 == 0) {
                    sceClibPrintf("[mh4u] clocks arm %d bus %d gpu %d xbar %d MHz\n",
                                  scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
                                  scePowerGetGpuClockFrequency(),
                                  scePowerGetGpuXbarClockFrequency());
                }
       // The stats window reads these on the render thread.
                {
                    using namespace VitaFrontend::HudStats;
                    speed_percent.store(static_cast<u32>(std::max(speed, 0)),
                                        std::memory_order_relaxed);
                    game_fps10.store(static_cast<u32>(std::max(fps10, 0)),
                                     std::memory_order_relaxed);
                    shown_fps.store(presented, std::memory_order_relaxed);
                    valid.store(true, std::memory_order_release);
                }
            }
        }
        // Nothing to present from here: a finished frame is presented by the render thread
        // where it arrives (EmuWindowVita::SwapBuffers), so this thread never waits on one.
    }

    window.SetUi(nullptr);
    ShutdownSystem(system);
    return keep_running;
}

/**
 * The title to start without going through the list, if the launcher named one.
 *
 * Two ways in, because there are two launchers. A LiveArea bubble or another application
 * starts this one with a `psgm:play?titleid=...&param=<path>` string, which arrives through
 * sceAppMgrGetAppParam. The Vita3K emulator, where the rendering is debugged off-console,
 * passes its --app-args as the process's own arguments instead. Empty when neither did:
 * then the list comes up as always.
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

void show_error_dialog(const std::string& message) {
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
}

/// The emulator proper. Runs on a thread of its own; see main.
int EmulatorMain() {
    // Register this stack with the allocation tracer's overflow watch; see alloc_trace.c.
    VitaAllocTraceWatchThread();
    // This thread drives the guest (which itself runs on the detached core 2, outside the
    // system scheduler) and feeds the render thread; RoleCore in common/thread.cpp lays out
    // which of the three schedulable cores each host thread gets.
    Common::SetCurrentThreadRole(Common::ThreadRole::Emulation);
    Common::RegisterThreadCpu("Emu");

    VTRACE("EmulatorMain: clocks");
    SetupClocks();
    VTRACE("EmulatorMain: paths");
    SetupPaths();
    VTRACE("EmulatorMain: logging");
    SetupLogging();
    VTRACE("EmulatorMain: network");
    SetupNetwork();

#ifdef CITRA_VITA_GPUPROF
    // Before sceGxmInitialize: the identity model counts every scene and present from the start.
    VitaFrontend::GpuProf::Install();
#endif
    VTRACE("EmulatorMain: GxmPresent::Initialize");
    if (!VitaFrontend::GxmPresent::Initialize()) {
        VTRACE("EmulatorMain: GxmPresent::Initialize FAILED");
        LOG_CRITICAL(Frontend, "Could not bring up GXM");
        return -1;
    }

    {
        SceCommonDialogConfigParam dialog_config_param{};
        sceCommonDialogSetConfigParam(&dialog_config_param);

        // ux0:data/mh4u/cpu_interpreter: run without the kernel modules (the interpreter
        // takes the guest, as ARM_VitaNative::CreateMap does for the same file) - the only way
        // to run under the Vita3K emulator, which cannot load a kernel plugin.
        SceIoStat interpreter_stat{};
        const bool interpreter_only =
            sceIoGetstat(VITA_DATA_DIR "/cpu_interpreter", &interpreter_stat) >= 0;
        if (interpreter_only) {
            LOG_WARNING(Frontend, "cpu_interpreter present: skipping the kernel module checks");
        } else if (!module_installed("mh4unative")) {
            show_error_dialog("mh4unative.skprx not installed: add it to the *KERNEL section of ur0:tai/config.txt.");
            return -1;
        }

        if (!interpreter_only && !module_installed("CapUnlocker")) {
            show_error_dialog("CapUnlocker.skprx not installed. Install it from "
                              "https://github.com/GrapheneCt/CapUnlocker");
            return -1;
        }

        if (!interpreter_only && azaharVersion() < MinAzaharNativeVersion) {
            show_error_dialog(
                "The installed mh4unative.skprx module is not compatible. Please update it.");
            return -1;
        }
    }

    // Presents never block this thread in a vblank wait: the display queue's own thread
    // does the flip and the wait, and submission only blocks past two pending flips - which
    // the run loop's own pacing keeps from happening.
    // The PS button and the quick menu are left to the shell. The guest runs folded into the
    // emulation thread (azaharTakeCore's folded mode): core 2 is the kernel's again between
    // slices, so a suspend just stops the thread between two of them like any other. The old
    // resident design held the core for the whole session and could not survive one, which
    // is what these used to be locked for. The pause menu is on Select + Start instead.
    sceShellUtilInitEvents(0);

    VTRACE("EmulatorMain: shell lock");
    auto& system = Core::System::GetInstance();
    VTRACE("EmulatorMain: got System instance");

    // The Vita has neither the memory nor the drivers for the LLE service modules. GXM is the
    // only renderer in this build: the software renderer was too slow on this
    // CPU to be worth carrying and is no longer compiled for the console.
    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }
    Settings::values.graphics_api = Settings::GraphicsAPI::Gxm;
    Settings::values.use_cpu_jit = false;
    Settings::values.disable_right_eye_render = true;
    // gxprof (ux0:data/mh4u/gxprof, release too): the GX thread's profile, where its list
    // time goes by register handler and draw step, printed once a second.
    bool gx_prof_file = false;
    if (std::FILE* f = std::fopen(VITA_DATA_DIR "/gxprof", "rb")) {
        std::fclose(f);
        gx_prof_file = true;
    }
    // heapprof (ux0:data/mh4u/heapprof): the sampled heap profile, top sites every 10 s.
    if (std::FILE* f = std::fopen(VITA_DATA_DIR "/heapprof", "rb")) {
        std::fclose(f);
        HeapProfInit(true);
    }
    // BigArena: 8 MiB of the heap for large short-lived buffers (GPU command list copies,
    // decoded audio, file reads), which otherwise fragmented the heap until an allocation
    // failed after 1-2 hours. Kill switch ux0:data/mh4u/nobigarena.
    if (std::FILE* f = std::fopen(VITA_DATA_DIR "/nobigarena", "rb")) {
        std::fclose(f);
        sceClibPrintf("[mh4u] bigarena off (nobigarena)\n");
    } else if (!Common::BigArena::Init(8 * 1024 * 1024)) {
        sceClibPrintf("[mh4u] bigarena: could not allocate\n");
    }
    HeapReserveInit(2 * 1024 * 1024);
    Common::EmuProf::enabled = gx_prof_file || GxmRenderer::GxmFlag("emuprof") ||
                               GxmRenderer::GxmFlag("emuprofgx");
    Common::EmuProf::gx_thread = gx_prof_file || GxmRenderer::GxmFlag("emuprofgx");
    Common::EmuProf::thread_disabled = Common::EmuProf::gx_thread;
    Common::dsp_parallel = !GxmRenderer::GxmFlag("nodspthread");
    Common::dsp_pipelined = !GxmRenderer::GxmFlag("nodsppipe");
    Common::y2r_fast = !GxmRenderer::GxmFlag("noy2rfast");
    Common::y2r_check = GxmRenderer::GxmFlag("y2rcheck");
    Common::y2r_async = !GxmRenderer::GxmFlag("noy2rasync");
    Common::gx_async = GxmRenderer::GxmFlag("gxasync");
    Common::gx_early_p3d = GxmRenderer::GxmFlag("gxearlyp3d");
    {
        // The stage 2 GX queue, on by default (implies the above); ux0:data/mh4u/nogxfifo (any
        // build) or the nogxfifo flag turns it off.
        std::FILE* f = std::fopen(VITA_DATA_DIR "/nogxfifo", "rb");
        if (f != nullptr) {
            std::fclose(f);
        }
        Common::gx_fifo = f == nullptr && !GxmRenderer::GxmFlag("nogxfifo");
        Common::gx_snapshot = !GxmRenderer::GxmFlag("nogxsnap");
        if (Common::gx_fifo) {
            Common::gx_async = true;
            Common::gx_early_p3d = true;
        }
    }
    // The vertex cache needs arm-first inserts once lists are parsed beside the guest.
    Common::vcache_arm_first = GxmRenderer::GxmFlag("vcarmfirst") || Common::gx_async;
    // Hardware draws reuse the GPU copy of vertex data that has not changed (pica/vertex_cache.h);
    // the emulator's own writes into guest memory drop what they overwrite. `novtxcache`.
    Pica::GetVertexCache().enabled = !GxmRenderer::GxmFlag("novtxcache");
    if (Pica::GetVertexCache().enabled) {
        Memory::host_write_hook = [](const u8* host, std::size_t size) {
            Pica::GetVertexCache().InvalidateHost(host, size);
        };
    }
    // One screen by default: the port is played with the Heavenfall mod's single-screen mode
    // (the touch screen's menus moved to the top screen), and the top screen then fills the
    // display. The pause menu's Screen layout still switches back.
    Settings::values.layout_option = Settings::LayoutOption::SingleScreen;
    // Picture shape (video.txt `wide`): the whole display for the mod's 16:9 option.
    Settings::values.aspect_ratio = GxmRenderer::Video().wide ? Settings::AspectRatio::Stretch
                                                             : Settings::AspectRatio::Default;
    VTRACE("EmulatorMain: applets");
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

    VTRACE("EmulatorMain: EmuWindowVita");
    VitaFrontend::EmuWindowVita window(system);
    VTRACE("EmulatorMain: InitInput");
    // Not InputCommon::Init(): every factory it registers is one this frontend does not use.
    // The buttons and the circle pad come from the "vita" engine above, the touchscreen from
    // EmuWindow's own "emu_window" factory, and nothing here sets a motion device. What it would
    // add is a Cemuhook UDP client - a socket and a thread of its own, talking to a desktop that
    // is not there - and factories for a keyboard and a GameCube adapter this console has not
    // got. SDL is a null state in this build either way.
    VitaFrontend::InitInput(window);

    VTRACE("EmulatorMain: Ui");
    VitaFrontend::Ui ui(system, window);
    VTRACE("EmulatorMain: entering main loop");

    VitaAllocTraceDump();

    // A one-game port: no title list. The game is launched at once and quitting it ends the app.
    if (const std::string launch = LaunchParameter(); !launch.empty()) {
        LOG_INFO(Frontend, "launch parameter names {}", launch);
        RunTitle(system, window, ui, launch);
    } else {
        sceIoMkdir(VITA_DATA_DIR "/roms", 0777);
        InstallDefaultCheats();
        InstallPendingCias(window, ui);
        const std::string rom = FindRom();
        if (rom.empty()) {
            ui.SetStatusMessage("Monster Hunter 4 Ultimate was not found.\n\nPut your own decrypted "
                                "game (.3ds, .cci or .cxi) in " VITA_DATA_DIR "/roms/ and start "
                                "again.\n\nPress START to exit.");
            while (!window.IsQuitRequested()) {
                window.PollEvents();
                ui.Update();
                window.Present(&ui);
                if ((window.ButtonsHeld() & SCE_CTRL_START) != 0) {
                    break;
                }
            }
        } else {
            RunTitle(system, window, ui, rom);
        }
    }

    VitaFrontend::ShutdownInput();
    Common::Log::Stop();

    VitaFrontend::GxmPresent::Shutdown();
    return 0;
}
} // Anonymous namespace

int main(int argc, char* argv[]) {
#ifdef CITRA_VITA_PMU
    Common::PipelineStats::stuck_report.store(&StuckReport, std::memory_order_relaxed);
#endif
#ifdef VITA_DIAGNOSTICS
    // What the partition has left after newlib took the heap. The extended-memory mode
    // (ATTRIBUTE2=12, ~365 MB) is only granted with every system app closed; without it the
    // 240 MB heap overruns the plain partition and newlib silently has no heap at all.
    {
        SceKernelFreeMemorySizeInfo info{};
        info.size = sizeof(info);
        sceKernelGetFreeMemorySize(&info);
        sceClibPrintf(
            "[azahar] free memory after the heap: user %u KiB, cdram %u KiB, phycont %u KiB\n",
            static_cast<unsigned>(info.size_user / 1024),
            static_cast<unsigned>(info.size_cdram / 1024),
            static_cast<unsigned>(info.size_phycont / 1024));
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
    for (int i = 1; i < argc; ++i) {
        if (argv[i] != nullptr && argv[i][0] != '\0') {
            g_arguments.emplace_back(argv[i]);
        }
    }
    SetupTerminateHandler();
#endif
    Common::SetCurrentThreadRole(Common::ThreadRole::Other);
    // The process's own thread gets a stack sized for a homebrew launcher, and the global that
    // is supposed to change that does not work. Everything therefore runs on a thread created
    // here with a stack the emulator can actually use - service handling and the savestate
    // serializer both go deep.
    // A thread stack comes out of the game partition, which measures about 238 MB in total and
    // is mostly spoken for by the newlib heap - so this is worth no more than it needs. Eight
    // megabytes still leaves the savestate serializer, which is the deepest thing that runs
    // here, a wide margin over the megabyte every other thread gets.
    constexpr std::size_t EmulationStackSize = 4 * 1024 * 1024;

    {
        Common::NamedThread emu_thread{Common::ThreadCfg{"emulation", EmulationStackSize},
                                       EmulatorMain};
        emu_thread.join();
    }

    return 0;
}

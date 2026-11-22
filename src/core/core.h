// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <boost/optional.hpp>
#include <boost/serialization/version.hpp>
#include "common/common_types.h"
#include "common/mutex.h"
#include "common/vector_math.h"
#include "core/arm/arm_interface.h"
#include "core/cheats/cheats.h"
#include "core/emu_host.h"
#include "core/hle/service/apt/applet_manager.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/movie.h"
#include "core/perf_stats.h"

namespace Frontend {
class EmuWindow;
class ImageInterface;
class MiiSelector;
class SoftwareKeyboard;
} // namespace Frontend

namespace Memory {
class MemorySystem;
}

namespace AudioCore {
class DspInterface;
}

namespace Core::RPC {
class Server;
}

namespace Service {
namespace SM {
class ServiceManager;
}
namespace FS {
class ArchiveManager;
}
} // namespace Service

namespace Common {
class NamedThread;
}

namespace Kernel {
class KernelSystem;
class SVCContext;
struct New3dsHwCapabilities;
enum class MemoryMode : u8;
} // namespace Kernel

namespace VideoDumper {
class Backend;
}

namespace VideoCore {
class GPU;
} // namespace VideoCore

namespace Loader {
class AppLoader;
}

namespace Core {

class ARM_Interface;
class ExclusiveMonitor;
class Timing;

class System {
public:
    /**
     * Gets the instance of the System singleton class.
     * @returns Reference to the instance of the System singleton class.
     */
    [[nodiscard]] static System& GetInstance() {
        return s_instance;
    }

    /// Enumeration representing the return values of the System Initialize and Load process.
    enum class ResultStatus : u32 {
        Success,                    ///< Succeeded
        ErrorNotInitialized,        ///< Error trying to use core prior to initialization
        ErrorGetLoader,             ///< Error finding the correct application loader
        ErrorSystemMode,            ///< Error determining the system mode
        ErrorLoader,                ///< Error loading the specified application
        ErrorLoader_ErrorEncrypted, ///< Error loading the specified application due to encryption
        ErrorLoader_ErrorInvalidFormat, ///< Error loading the specified application due to an
                                        /// invalid format
        ErrorLoader_ErrorGbaTitle, ///< Error loading the specified application as it is GBA Virtual
                                   ///< Console
        ErrorLoader_ErrorPatches,  ///< Generic error while loading patches for an application
        ErrorLoader_ErrorPatchesInvalidTitle, ///< A patch was loaded for the incorrect application
        ErrorSystemFiles,                     ///< Error in finding system files
        ErrorSavestate,                       ///< Error saving or loading
        ErrorArticDisconnected,               ///< Error when artic base disconnects
        ErrorN3DSApplication,        ///< Error launching New 3DS application in Old 3DS mode
        ErrorCoreExceptionRaised,    ///< The CPU emulation raised an exception
        ErrorMemoryExceptionRaised,  ///< Unmmaped memory was accessed
        ErrorSavestateBuildMismatch, ///< Tried to load savestate from a different Azahar version
        ShutdownRequested,           ///< Emulated program requested a system shutdown
        ErrorUnknown                 ///< Any other error
    };

    explicit System();
    ~System();

    /**
     * Run the core CPU loop
     * This function runs the core for the specified number of CPU instructions before trying to
     * update hardware. This is much faster than SingleStep (and should be equivalent), as the CPU
     * is not required to do a full dispatch with each instruction. NOTE: the number of instructions
     * requested is not guaranteed to run, as this will be interrupted preemptively if a hardware
     * update is requested (e.g. on a thread switch).
     * @param tight_loop If false, the CPU single-steps.
     * @return Result status, indicating whethor or not the operation succeeded.
     */
    [[nodiscard]] ResultStatus RunLoop(bool tight_loop = true);

    /**
     * Step the CPU one instruction
     * @return Result status, indicating whethor or not the operation succeeded.
     */
    [[nodiscard]] ResultStatus SingleStep();

    /// Shutdown the emulated system.
    void Shutdown(bool is_deserializing = false);

    /// Shutdown and then load again
    void Reset();

    enum class Signal : u32 { None, Shutdown, Reset, Save, Load };

    bool SendSignal(Signal signal, u32 param = 0);

    /// Request reset of the system
    void RequestReset(const std::string& chainload = "", std::optional<u8> mem_mode = {}) {
        m_chainloadpath = chainload;
        m_mem_mode = mem_mode;
        SendSignal(Signal::Reset);
    }

    /// Request shutdown of the system
    void RequestShutdown() {
        SendSignal(Signal::Shutdown);
    }

    /**
     * Load an executable application.
     * @param emu_window Reference to the host-system window used for video output and keyboard
     *                   input.
     * @param filepath String path to the executable application to load on the host file system.
     * @returns ResultStatus code, indicating if the operation succeeded.
     */
    [[nodiscard]] ResultStatus Load(Frontend::EmuWindow& emu_window, const std::string& filepath,
                                    Frontend::EmuWindow* secondary_window = {});

    /**
     * Memory, timing and the GPU alone, with no kernel, services, CPU or application: for work
     * the GPU does with no title, which the host hands it itself (VideoCore::GPU::HostFrame).
     * vprof's replay player replays a capture's frame onto them; the game list draws the
     * selected title's banner with them. Nothing else may run, and the GPU's interrupts go
     * nowhere. ShutdownGpuOnly ends it, before a title's Load.
     */
    [[nodiscard]] ResultStatus InitGpuOnly(Frontend::EmuWindow& emu_window);
    void ShutdownGpuOnly();

    /**
     * Indicates if the emulated system is powered on (all subsystems initialized and able to run an
     * application).
     * @returns True if the emulated system is powered on, otherwise false.
     */
    [[nodiscard]] bool IsPoweredOn() const {
        return is_powered_on;
    }

    /// Whether the title runs as on a New 3DS: a New 3DS title does, and every title with
    /// Settings' is_new_3ds (Load); every other one runs as on an Old 3DS. A loaded state runs
    /// as it was saved.
    [[nodiscard]] bool IsNew3ds() const {
        return new_3ds;
    }

    /// Prepare the core emulation for a reschedule
    void PrepareReschedule();

    /// Makes core `core_id` the running one, as RunLoop does before each core's turn: the
    /// kernel's current core, its timer and its process (the Vita native core's upcalls run the
    /// other cores' timer events while the guest idles).
    void SelectCore(u32 core_id);
    /// SelectCore by the core itself, for the calling emulation thread.
    void SetRunning(ARM_Interface* core);

    [[nodiscard]] PerfStats::Results GetAndResetPerfStats();

    void ReportArticTraffic(u32 bytes) {
        if (perf_stats) {
            perf_stats->AddArticBaseTraffic(bytes);
        }
    }

    void ReportPerfArticEvent(PerfStats::PerfArticEventBits event, bool set) {
        if (perf_stats) {
            perf_stats->ReportPerfArticEvent(event, set);
        }
    }

    [[nodiscard]] PerfStats::Results GetLastPerfStats();

    double GetStableFrameTimeScale();

    /**
     * Gets a reference to the emulated CPU.
     * @returns A reference to the emulated CPU.
     */

    [[nodiscard]] ARM_Interface& GetRunningCore() {
        return *running_core[ThisHost()];
    };

    /**
     * Gets a reference to the emulated CPU.
     * @param core_id The id of the core requested.
     * @returns A reference to the emulated CPU.
     */

    [[nodiscard]] ARM_Interface& GetCore(u32 core_id) {
        return *cpu_cores[core_id];
    };

    [[nodiscard]] const ARM_Interface& GetCore(u32 core_id) const {
        return *cpu_cores[core_id];
    };

    [[nodiscard]] u32 GetNumCores() const {
        return static_cast<u32>(cpu_cores.size());
    }

    void InvalidateCacheRange(u32 start_address, std::size_t length) {
        if (code_sync_batch) {
            code_sync_batch->Add(start_address, length);
            return;
        }
        for (const auto& cpu : cpu_cores) {
            cpu->InvalidateCacheRange(start_address, length);
        }
    }

    /// The InvalidateCacheRange calls made while it lives, done at its end one run of pages each:
    /// an LDR:RO command's relocations each sync a word.
    class CodeSyncBatch {
    public:
        explicit CodeSyncBatch(System& system);
        ~CodeSyncBatch();
        CodeSyncBatch(const CodeSyncBatch&) = delete;
        CodeSyncBatch& operator=(const CodeSyncBatch&) = delete;
        void Add(u32 start_address, std::size_t length);

    private:
        System& system;
        /// [first page, end page)
        std::vector<std::pair<u32, u32>> runs;
    };

    /// Drops every core's instruction cache. Needed when guest code has been rewritten somewhere
    /// this side cannot name a range for - CRO linking patches call sites across every module that
    /// imports from the one being linked, not just the module itself.
    void ClearInstructionCache() {
        for (const auto& cpu : cpu_cores) {
            cpu->ClearInstructionCache();
        }
    }

    /**
     * Gets a reference to the emulated DSP.
     * @returns A reference to the emulated DSP.
     */
    [[nodiscard]] AudioCore::DspInterface& DSP() {
        return *dsp_core;
    }

    [[nodiscard]] VideoCore::GPU& GPU();

    /**
     * Gets a reference to the service manager.
     * @returns A reference to the service manager.
     */
    [[nodiscard]] Service::SM::ServiceManager& ServiceManager();

    /**
     * Gets a const reference to the service manager.
     * @returns A const reference to the service manager.
     */
    [[nodiscard]] const Service::SM::ServiceManager& ServiceManager() const;

    /// Gets a reference to the archive manager
    [[nodiscard]] Service::FS::ArchiveManager& ArchiveManager();

    /// Gets a const reference to the archive manager
    [[nodiscard]] const Service::FS::ArchiveManager& ArchiveManager() const;

    /// Gets a reference to the kernel
    [[nodiscard]] Kernel::KernelSystem& Kernel();

    /// Gets a const reference to the kernel
    [[nodiscard]] const Kernel::KernelSystem& Kernel() const;

    /// Get kernel is running
    [[nodiscard]] bool KernelRunning();

    /// Gets a reference to the timing system
    [[nodiscard]] Timing& CoreTiming();

    /// Gets a const reference to the timing system
    [[nodiscard]] const Timing& CoreTiming() const;

    /// Gets a reference to the memory system
    [[nodiscard]] Memory::MemorySystem& Memory();

    /// Gets a const reference to the memory system
    [[nodiscard]] const Memory::MemorySystem& Memory() const;

    /// Gets a reference to the cheat engine
    [[nodiscard]] Cheats::CheatEngine& CheatEngine();

    /// Gets a const reference to the cheat engine
    [[nodiscard]] const Cheats::CheatEngine& CheatEngine() const;

    /// Gets a reference to the movie recorder
    [[nodiscard]] Core::Movie& Movie();

    /// Gets a const reference to the movie recorder
    [[nodiscard]] const Core::Movie& Movie() const;

    /// Video Dumper interface

    void RegisterVideoDumper(std::shared_ptr<VideoDumper::Backend> video_dumper);

    [[nodiscard]] std::shared_ptr<VideoDumper::Backend> GetVideoDumper() const {
        return video_dumper;
    }

    std::unique_ptr<PerfStats> perf_stats;
    FrameLimiter frame_limiter;

    void SetStatus(ResultStatus new_status, const char* details = nullptr) {
        // The second emulation thread's: RunLoop's own status starts over each call, so it takes
        // this one over there (second_failed).
        if (ThisHost() == 1) {
            second_status = new_status;
            second_details = details ? details : "";
            second_failed.store(true, std::memory_order_release);
            return;
        }
        status = new_status;
        if (details) {
            status_details = details;
        }
    }

    /**
     * The second emulation thread (emu_host.h): the emulated cores other than core 0, beside
     * RunLoop's, on a Vita core of its own, with no lock between the two. RunLoop starts it once
     * the guest's table is in and stops it before a savestate, a load and the end.
     */
    void StartSecondHost();
    void StopSecondHost();
    /// An svc of core `core_id` on the second thread: answered there (LocalToSecondHost), else
    /// by host 0 while the second thread waits (ServeForward).
    void SecondHostSvc(u32 core_id, Kernel::SVCContext& svc, u32 immediate);
    /// Host 0: the second thread's svc or fault it waits for, if there is one.
    void ServeForward();
    /// Whether host 0 may idle in the guest's world (an upcall, no exit) up to tick `until`: the
    /// second thread asleep with nothing due before it, no message and no svc for host 0. Host
    /// 0's clock goes out to it meanwhile (PublishClock).
    [[nodiscard]] bool SecondHostQuiet(u64 until) const;
    void PublishClock(u32 host);
    /// A permission fault of core `core_id` on the second thread (a store or load on a page the
    /// GPU's queued work holds): host 0 takes the page as its own aborts do while the second
    /// thread waits. Whether a hold took it (the access runs again, or ran).
    bool SecondHostHeldPage(u32 core_id);

#ifdef VITA_DIAGNOSTICS
    /// The two emulation threads' counters since the last call (perflog).
    struct SecondHostCensus {
        u32 passes{}, runs{}, idles{}, local_svcs{}, forwards{}, held_pages{};
        /// Of the held pages, the aborts that raced a release (TakeForwardedHeldPage).
        u32 raced_holds{};
        u64 idle_us{}, forward_wait_us{};
        u32 host0_idles{}, served{};
        u64 host0_idle_us{};
        /// Host 0's sleeps by what ended them: a message, the other's clock, the other's idle
        /// changing, none of those.
        std::array<u32, 4> host0_woken{};
        std::array<u64, 4> host0_woken_us{};
        /// Of host 0's sleeps the clock ended: those whose wake was timed, the time from the
        /// other's clock reaching this one's next event to this one running, and the sleeps by
        /// length (under 10, 25, 50, 100, 250 us, longer).
        u32 clock_wakes_timed{};
        u64 clock_wake_latency_us{};
        std::array<u32, 6> clock_sleeps_by_length{};
        std::array<u16, 256> forward_svcs{};
        std::array<u16, 256> local_svcs_by{};
        /// Host 1's clock less host 0's at host 1's last pass, in ticks.
        s64 lead{};
        /// Host 0's idles by how they ended: work at once, no sleep to its own next event with
        /// host 1 idle, the same with host 1 running past it, no sleep to host 1's clock or next
        /// event, a sleep; and the ticks the idles with no sleep moved host 0's clock.
        std::array<u32, 5> host0_idle_how{};
        u64 host0_jump_ticks{};
    };
    /// Why SecondHostQuiet(until) is false: host 1 running, its next event by `until`, a message
    /// for it, one for host 0, a forwarded svc; 5 when quiet.
    [[nodiscard]] u32 SecondHostBusyWhy(u64 until) const;
    SecondHostCensus TakeSecondHostCensus();
    /// Every guest thread to the log at the next RunLoop (perflog: seconds with no frame).
    void RequestThreadDump() {
        thread_dump_requested.store(true, std::memory_order_relaxed);
    }
#endif

    [[nodiscard]] const std::string& GetStatusDetails() const {
        return status_details;
    }

    /// The file the running title came from: the first one's, or a jump's (Reset).
    [[nodiscard]] const std::string& GetFilePath() const {
        return m_filepath;
    }

    /// Called after Reset has loaded the title it was for, on the thread that ran it: a title's
    /// jump to another one ends here, with no call from the frontend.
    void SetResetCallback(std::function<void()> callback) {
        reset_callback = std::move(callback);
    }

    [[nodiscard]] Loader::AppLoader& GetAppLoader() const {
        return *app_loader;
    }

    /// Whether a title was loaded (the Vita's replay player renders without one).
    [[nodiscard]] bool HasAppLoader() const {
        return app_loader != nullptr;
    }

    /// Frontend Applets

    void RegisterMiiSelector(std::shared_ptr<Frontend::MiiSelector> mii_selector);

    void RegisterSoftwareKeyboard(std::shared_ptr<Frontend::SoftwareKeyboard> swkbd);

    [[nodiscard]] std::shared_ptr<Frontend::MiiSelector> GetMiiSelector() const {
        return registered_mii_selector;
    }

    [[nodiscard]] std::shared_ptr<Frontend::SoftwareKeyboard> GetSoftwareKeyboard() const {
        return registered_swkbd;
    }

    /// Image interface

    void RegisterImageInterface(std::shared_ptr<Frontend::ImageInterface> image_interface);

    [[nodiscard]] std::shared_ptr<Frontend::ImageInterface> GetImageInterface() const {
        return registered_image_interface;
    }

    /// Function for checking OS microphone permissions.

    void RegisterMicPermissionCheck(const std::function<bool()>& permission_func) {
        mic_permission_func = permission_func;
    }

    [[nodiscard]] bool HasMicPermission() {
        return !mic_permission_func || mic_permission_granted ||
               (mic_permission_granted = mic_permission_func());
    }

    enum class SaveStateStatus {
        NONE,
        LOADING,
        SAVING,
    };

    SaveStateStatus GetSaveStateStatus() {
        return save_state_status;
    }

    /// A save or load signalled but not yet run: RunLoop performs it once the kernel has no
    /// asynchronous operation pending, which can take many iterations early in a boot.
    bool IsSaveStateRequestPending() const {
        return save_state_request_status != SaveStateStatus::NONE;
    }

    /// Accept a state saved by another build of the same serialization layout (the Vita
    /// autoload restores states other builds saved; every commit changes the revision).
    /// Where a savestate's save or load has got to, for a frontend's progress bar: a save
    /// streams the state into the file; a load reads the file, restarts the system, then restores
    /// the state from what it read.
    enum class StatePhase { Saving, Reading, Restarting, Restoring };
    /// Called on the thread that saves or loads, with the phase and how far through it (0 to 1),
    /// a few times a megabyte; the load's Restarting while no GPU exists.
    using StateProgressCallback = std::function<void(StatePhase phase, float fraction)>;
    void SetStateProgressCallback(StateProgressCallback callback) {
        state_progress = std::move(callback);
    }
    void ReportStateProgress(StatePhase phase, float fraction) const {
        if (state_progress) {
            state_progress(phase, fraction);
        }
    }

    void SetLoadStateAnyBuild(bool accept) {
        load_state_any_build = accept;
    }

    /// A load that failed partway through the archive: the state is part the old one, part the
    /// file's, and the title cannot run on.
    bool IsStateLoadPartial() const {
        return state_load_partial;
    }

    void SaveState(u32 slot) const;

    void LoadState(u32 slot);

    /// Restores what deserializing a save state cannot; see the definition.
    void AfterStateLoaded();

    std::vector<u8> SaveStateBuffer() const;

    bool LoadStateBuffer(std::vector<u8> buffer);

    /// Applies any changes to settings to this core instance.
    void ApplySettings();

    void RegisterAppLoaderEarly(std::unique_ptr<Loader::AppLoader>& loader);

    void InsertCartridge(const std::string& path);

    void EjectCartridge();

    const std::string& GetCartridge() const {
        return inserted_cartridge;
    }

    bool IsInitialSetup();

    // This returns the 3DS notification LED RGB value.
    // Keep in mind this is used as a PWM duty cycle on real HW,
    // so the percieved LED brightness is not linear.
    const Common::Vec3<u8>& GetInfoLEDColor() const {
        return info_led_color;
    }

    void SetInfoLEDColor(const Common::Vec3<u8>& color) {
        if (color == info_led_color)
            return;

        info_led_color = color;
        if (info_led_color_changed) {
            info_led_color_changed();
        }
    }

    void RegisterInfoLEDColorChanged(const std::function<void()>& func) {
        info_led_color_changed = func;
    }

    void SetDebugNextProcessFlag() {
        debug_next_process = true;
    }

    bool GetDebugNextProcessFlag() {
        return debug_next_process;
    }

    void ClearDebugNextProcessFlag() {
        debug_next_process = false;
    }

    void DebugUnscheduleAllThreadsFromFrontend(bool unschedule);

    void SetGDBPortOverride(int port) {
        override_gdb_port = port;
    }

    void RegisterCoreLoopThreadId() {
        core_loop_thread_id = std::this_thread::get_id();
    }

    std::thread::id GetCoreLoopThreadId() {
        return core_loop_thread_id;
    }

private:
    /**
     * Initialize the emulated system.
     * @param emu_window Reference to the host-system window used for video output and keyboard
     *                   input.
     * @param system_mode The system mode.
     * @return ResultStatus code, indicating if the operation succeeded.
     */
    [[nodiscard]] ResultStatus Init(Frontend::EmuWindow& emu_window,
                                    Frontend::EmuWindow* secondary_window,
                                    Kernel::MemoryMode memory_mode, u32 num_cores);

    /// Reschedule the core emulation
    void Reschedule();

    /// AppLoader used to load the current executing application
    std::unique_ptr<Loader::AppLoader> app_loader;

    // Temporary app loader passed from frontend
    std::unique_ptr<Loader::AppLoader> early_app_loader;

    /// Path for current inserted cartridge
    std::string inserted_cartridge;

    /// ARM11 CPU core
    std::vector<std::shared_ptr<ARM_Interface>> cpu_cores;
    /// Each emulation thread's core (emu_host.h).
    std::array<ARM_Interface*, NumHosts> running_core{};
    CodeSyncBatch* code_sync_batch = nullptr;

    /// IsNew3ds, set by Load before the system's parts are made.
    bool new_3ds = false;

    /// DSP core
    std::unique_ptr<AudioCore::DspInterface> dsp_core;

    /// When true, signals that a reschedule should happen (each emulation thread's core)
    std::array<bool, NumHosts> curr_core_reschedule_pending{};

    std::unique_ptr<VideoCore::GPU> gpu;

    /// Service manager
    std::unique_ptr<Service::SM::ServiceManager> service_manager;

    /// Frontend applets
    std::shared_ptr<Frontend::MiiSelector> registered_mii_selector;
    std::shared_ptr<Frontend::SoftwareKeyboard> registered_swkbd;

    /// Movie recorder
    Core::Movie movie;

    /// Cheats manager
    Cheats::CheatEngine cheat_engine;

    /// Video dumper backend
    std::shared_ptr<VideoDumper::Backend> video_dumper;

    /// Custom texture cache system

    /// Image interface
    std::shared_ptr<Frontend::ImageInterface> registered_image_interface;

#ifdef ENABLE_SCRIPTING
    /// RPC Server for scripting support
    std::unique_ptr<RPC::Server> rpc_server;
#endif

    std::unique_ptr<Service::FS::ArchiveManager> archive_manager;

    std::unique_ptr<Memory::MemorySystem> memory;
    std::unique_ptr<Kernel::KernelSystem> kernel;
    std::unique_ptr<Timing> timing;

    std::unique_ptr<Core::ExclusiveMonitor> exclusive_monitor;

private:
    static System s_instance;

    std::atomic_bool is_powered_on{};

    SaveStateStatus save_state_status = SaveStateStatus::NONE;
    SaveStateStatus save_state_request_status = SaveStateStatus::NONE;
    bool load_state_any_build = false;
    StateProgressCallback state_progress;
    bool state_load_partial = false;
    u32 save_state_slot = 0;
    std::chrono::steady_clock::time_point save_state_request_time{};

    ResultStatus status = ResultStatus::Success;
    std::string status_details = "";

    /// The second emulation thread (StartSecondHost) and what the two publish to each other.
    void SecondHostMain();
    /// Whether the second thread can take its cores: the guest's table is in.
    [[nodiscard]] bool SecondHostCanStart() const;
    /// The calling host's cores: host 0's core 0 (every core with no second thread), host 1's
    /// the others.
    [[nodiscard]] std::span<const std::shared_ptr<ARM_Interface>> HostCores(u32 host) const;
    /// No thread of host `host`'s cores can run: its clock follows the other host's while that
    /// one runs, up to its own next event; with both idle, both go to the sooner next event.
    /// Returns once there is something to do (a message, an event due, a forwarded svc, a stop).
    void HostIdle(u32 host);

    /// The waits host 0 made for the other cores' threads, whose timeouts sit on those cores'
    /// timers, onto core 0's (the second thread starts); host 1's event waits into host 0's
    /// lists (it stops).
    void MoveWaitsToFirstHost(bool starting);
    struct HostClock {
        std::atomic<u64> clock{};
        std::atomic<u64> next{};
        std::atomic<bool> idle{};
#ifdef VITA_DIAGNOSTICS
        /// When this host's clock last reached the other's next event and woke it (us, steady).
        std::atomic<u64> woke_other_us{};
#endif
    };
    std::array<HostClock, NumHosts> host_clock;
    std::unique_ptr<Common::NamedThread> second_host;
    std::atomic<bool> second_host_attached{};
    std::atomic<bool> second_host_stop{};
    std::atomic<bool> second_failed{};
    ResultStatus second_status = ResultStatus::Success;
    std::string second_details;
    /// What the second thread hands host 0: an svc (its core, context and number) or a held
    /// page's fault (its core), and the fault's answer.
    enum class ForwardKind : u8 { Svc, HeldPage };
    std::atomic<bool> forward_pending{};
    ForwardKind forward_kind{};
    u32 forward_core{};
    u32 forward_immediate{};
    Kernel::SVCContext* forward_svc{};
    bool forward_taken{};
    /// The second thread's side: hands it over and waits for host 0.
    void ForwardToFirstHost();
#ifdef VITA_DIAGNOSTICS
    SecondHostCensus second_census{};
    std::atomic<bool> thread_dump_requested{};
    void DumpGuestThreads();
#endif
    /// Saved variables for reset
    Frontend::EmuWindow* m_emu_window;
    Frontend::EmuWindow* m_secondary_window;
    std::string m_filepath;
    std::string m_chainloadpath;
    std::optional<u8> m_mem_mode;
    u64 title_id;

    Common::Mutex signal_mutex;
    Signal current_signal;
    u32 signal_param;

    std::function<void()> reset_callback;
    std::function<bool()> mic_permission_func;
    bool mic_permission_granted = false;

    boost::optional<Service::APT::DeliverArg> restore_deliver_arg;
    boost::optional<Service::APT::SysMenuArg> restore_sys_menu_arg;
    boost::optional<Service::PLGLDR::PLG_LDR::PluginLoaderContext> restore_plugin_context;
    std::unique_ptr<IPCDebugger::Recorder> restore_ipc_recorder;
    std::vector<u8> restore_wireless_reboot_info;

    std::vector<u64> lle_modules;

    Common::Vec3<u8> info_led_color;
    std::function<void()> info_led_color_changed;

    bool debug_next_process;
    int override_gdb_port = -1;

    std::thread::id core_loop_thread_id{};

    friend class boost::serialization::access;
    template <typename Archive>
    void serialize(Archive& ar, const unsigned int file_version);
};

[[nodiscard]] inline ARM_Interface& GetRunningCore() {
    return System::GetInstance().GetRunningCore();
}

[[nodiscard]] inline ARM_Interface& GetCore(u32 core_id) {
    return System::GetInstance().GetCore(core_id);
}

[[nodiscard]] inline u32 GetNumCores() {
    return System::GetInstance().GetNumCores();
}

} // namespace Core

BOOST_CLASS_VERSION(Core::System, 1)

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// A small standalone frontend, enough to boot a title and look at the result.
//
// It exists because upstream's frontends do not suit a machine driven over ssh. The renderer is
// chosen at startup: the OpenGL renderer wants GL 4.3 or GLES 3.2, which the native driver here
// cannot reach (the Pi 5's V3D stops at 3.1), so the window falls back to Mesa's llvmpipe and only
// then to the software renderer; Vulkan's C++ bindings do not build for 32-bit ARM at all.
// A frame budget, a seconds budget and the input debug port make it drivable over a terminal.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "citra_sdl/emu_window_sdl2.h"
#include "citra_sdl/input_debug_port.h"
#ifdef CITRA_HAS_NATIVE_ARM
#include "core/arm/native/arm_native.h"
#include "core/arm/native/native_mirror.h"
#endif
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/applets/default_applets.h"
#include "core/frontend/image_interface.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/service.h"
#include "core/loader/loader.h"
#include "core/movie.h"
#include "video_core/gpu.h"
#include "video_core/pica/pica_core.h"
#include "input_common/main.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace {

struct Options {
    std::string rom_path;
    std::string screenshot_path;
    long frame_limit = -1;
    long seconds_limit = -1;
    long input_port = -1;
    bool interpreter = false;
    bool no_shader_jit = false;
    bool no_hw_shader = false;
    std::string record_path;
    std::string playback_path;
    bool deterministic = false;
    bool unlimited = false;
    bool software = false;
    int loadstate_slot = -1;
    std::string log_filter = "*:Info";
};

void PrintUsage(const char* program) {
    std::printf(
        "Usage: %s [options] <path to 3DS title>\n"
        "\n"
        "  -h, --help              Show this message\n"
        "  -f, --frames N          Stop after N presented frames\n"
        "  -s, --seconds N         Stop after N seconds\n"
        "      --interpreter       Run guest code on the dyncom interpreter\n"
        "      --no-shader-jit     Interpret PICA vertex shaders instead of JITing them\n"
        "      --no-hw-shader      Run GL's vertex shaders on the CPU, not the host GPU\n"
        "  -o, --screenshot PATH   Write the final frame to PATH as a PPM image\n"
        "  -l, --log-filter SPEC   Logging filter, e.g. \"*:Debug\" (default \"*:Info\")\n"
        "  -p, --input-port PORT   Listen on 127.0.0.1:PORT for input commands\n"
        "  -r, --record PATH       Record every input to a movie file\n"
        "  -R, --play PATH         Replay a recorded movie file\n"
        "  -d, --deterministic     Advance guest time by guest activity rather than the host\n"
        "                          clock, so a recording replays identically. Implied by --play\n"
        "      --unlimited         No frame pacing: the emulation runs as fast as it can\n"
        "      --software          Use the software renderer (no OpenGL needed). Also the\n"
        "                          automatic fallback when no GL(ES) context can be created\n"
        "      --loadstate N       Restore savestate slot N before the render thread starts,\n"
        "                          so measurements can begin mid-game\n"
        "\n"
        "The input port takes one command per line and replies to each:\n"
        "  press|release|tap <button> [ms]  a b x y up down left right l r start select\n"
        "                               zl zr home power pad-<dir> cstick-<dir>\n"
        "  touch <x> <y> | untouch      Touch screen, in window coordinates\n"
        "  savestate|loadstate [slot]   Save or restore emulator state\n"
        "  screenshot <path>            Write the current frame as a PPM image\n"
        "  status | quit\n"
        "\n"
        "  printf 'tap a\\nscreenshot /tmp/f.ppm\\n' | nc -q1 127.0.0.1 5000\n",
        program);
}

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; i++) {
        const std::string argument = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", name);
                return nullptr;
            }
            return argv[++i];
        };

        if (argument == "-h" || argument == "--help") {
            return false;
        } else if (argument == "-f" || argument == "--frames") {
            const char* value = next("--frames");
            if (value == nullptr) {
                return false;
            }
            options.frame_limit = std::strtol(value, nullptr, 10);
        } else if (argument == "-s" || argument == "--seconds") {
            const char* value = next("--seconds");
            if (value == nullptr) {
                return false;
            }
            options.seconds_limit = std::strtol(value, nullptr, 10);
        } else if (argument == "-o" || argument == "--screenshot") {
            const char* value = next("--screenshot");
            if (value == nullptr) {
                return false;
            }
            options.screenshot_path = value;
        } else if (argument == "-p" || argument == "--input-port") {
            const char* value = next("--input-port");
            if (value == nullptr) {
                return false;
            }
            options.input_port = std::strtol(value, nullptr, 10);
        } else if (argument == "-r" || argument == "--record") {
            const char* value = next("--record");
            if (value == nullptr) {
                return false;
            }
            options.record_path = value;
        } else if (argument == "-R" || argument == "--play") {
            const char* value = next("--play");
            if (value == nullptr) {
                return false;
            }
            options.playback_path = value;
        } else if (argument == "-d" || argument == "--deterministic") {
            options.deterministic = true;
        } else if (argument == "--unlimited") {
            options.unlimited = true;
        } else if (argument == "--interpreter") {
            options.interpreter = true;
        } else if (argument == "--no-shader-jit") {
            options.no_shader_jit = true;
        } else if (argument == "--no-hw-shader") {
            options.no_hw_shader = true;
        } else if (argument == "--software") {
            options.software = true;
        } else if (argument == "--loadstate") {
            const char* value = next("--loadstate");
            if (value == nullptr) {
                return false;
            }
            options.loadstate_slot = std::strtol(value, nullptr, 10);
        } else if (argument == "-l" || argument == "--log-filter") {
            const char* value = next("--log-filter");
            if (value == nullptr) {
                return false;
            }
            options.log_filter = value;
        } else if (!argument.empty() && argument[0] == '-') {
            std::fprintf(stderr, "Unknown option %s\n", argument.c_str());
            return false;
        } else {
            options.rom_path = argument;
        }
    }
    return !options.rom_path.empty();
}

const char* DescribeLoadError(Core::System::ResultStatus status) {
    switch (status) {
    case Core::System::ResultStatus::ErrorGetLoader:
        return "unrecognised file format";
    case Core::System::ResultStatus::ErrorLoader_ErrorEncrypted:
        return "the title is encrypted; a decrypted dump is needed";
    case Core::System::ResultStatus::ErrorLoader_ErrorInvalidFormat:
        return "the file is not a valid 3DS title";
    case Core::System::ResultStatus::ErrorLoader:
        return "the loader rejected the file";
    case Core::System::ResultStatus::ErrorSystemFiles:
        return "required system files are missing";
    default:
        return "unknown error";
    }
}

} // Anonymous namespace

int main(int argc, char** argv) {
#ifdef CITRA_HAS_NATIVE_ARM
    // Before anything can allocate: guest code executes at its own addresses, and on a 32-bit host
    // those addresses are low enough for the heap to reach.
    Core::NativeReserveGuestAddressSpace();
#endif

    Options options;
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage(argv[0]);
        return 1;
    }

    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(true);
    Common::Log::Filter filter(Common::Log::Level::Trace);
    // filter.ParseFilterString(options.log_filter);
    Common::Log::SetGlobalFilter(filter);

    auto& system = Core::System::GetInstance();

    for (const auto& service_module : Service::service_module_map) {
        Settings::values.lle_modules.emplace(service_module.name, false);
    }
    Frontend::RegisterDefaultApplets(system);
    system.RegisterImageInterface(std::make_shared<Frontend::ImageInterface>());

    SetDefaultInputBindings();
    InputCommon::Init();

#ifdef CITRA_HAS_NATIVE_ARM
    // A recording is only worth having if replaying it gives the same result, and under native
    // execution that needs guest time to stop tracking the host clock.
    if (options.deterministic || !options.playback_path.empty()) {
        Core::SetNativeDeterministicTiming(true);
        // Run-to-run reproducibility for the replay harness: the 3DS clock must not read the
        // host's, tick seeds must be fixed, and audio must not pace against a realtime sink -
        // boot animations key off these and diverged between identical replays otherwise.
        Settings::values.init_clock = Settings::InitClock::FixedTime;
        Settings::values.init_ticks_type = Settings::InitTicks::Fixed;
        Settings::values.output_type = AudioCore::SinkType::Null;
        LOG_INFO(Frontend, "Deterministic timing enabled; guest time no longer tracks real time");
    } else if (!options.record_path.empty()) {
        LOG_WARNING(Frontend,
                    "Recording without --deterministic: guest time follows the host clock, so this "
                    "movie will not replay identically");
    }
#endif

    // The window works down a context ladder from the native driver to llvmpipe. Devices with
    // no usable GL(ES) at all run the software renderer instead - explicitly via --software, or
    // as the automatic fallback when the whole ladder fails.
    // Shader paths, for attributing a rendering fault to one of them: the PICA vertex shader
    // JIT (shared by both renderers) and GL's generated GLSL (GL only).
    if (options.interpreter) {
        // The CPU the Vita build cannot use, and the only one an emulated Vita can run: having
        // it here as well is what lets a picture drawn over native execution be compared with
        // one drawn over the interpreter, on a host where both work.
        Settings::values.use_cpu_jit = false;
        LOG_INFO(Frontend, "Guest CPU: the dyncom interpreter");
    }
    if (options.no_shader_jit) {
        Settings::values.use_shader_jit = false;
        LOG_INFO(Frontend, "PICA vertex shaders: interpreter (JIT disabled)");
    }
    if (options.no_hw_shader) {
        Settings::values.use_hw_shader = false;
        LOG_INFO(Frontend, "GL vertex shaders: software (hw shaders disabled)");
    }
    EmuWindow_SDL2 window(system, options.software);
    if (options.software) {
        Settings::values.graphics_api = Settings::GraphicsAPI::Software;
        LOG_INFO(Frontend, "Using the software renderer");
    } else if (window.UsingOpenGL()) {
        Settings::values.graphics_api = Settings::GraphicsAPI::OpenGL;
    } else {
#ifdef ENABLE_SOFTWARE_RENDERER
        LOG_WARNING(Frontend,
                    "Could not create any OpenGL context; falling back to the software renderer");
        window.UseSoftwareRendering();
        Settings::values.graphics_api = Settings::GraphicsAPI::Software;
#else
        LOG_CRITICAL(Frontend, "Could not create any OpenGL context; cannot run");
        InputCommon::Shutdown();
        return 1;
#endif
    }

    InputDebugPort input_port;
    if (options.input_port > 0) {
        input_port.Open(static_cast<u16>(options.input_port));
    }

    auto& movie = system.Movie();
    if (!options.record_path.empty()) {
        movie.PrepareForRecording();
    }
    if (!options.playback_path.empty()) {
        movie.PrepareForPlayback(options.playback_path);
    }

    const auto load_result = system.Load(window, options.rom_path);
    if (load_result != Core::System::ResultStatus::Success) {
        LOG_CRITICAL(Frontend, "Failed to load {}: {}", options.rom_path,
                     DescribeLoadError(load_result));
        InputCommon::Shutdown();
        return 1;
    }
    LOG_INFO(Frontend, "Loaded {}", options.rom_path);

    // GL and the software renderer draw on this thread, which keeps the context. GXM runs its
    // operations on the GPU's own render thread and needs no context here.
    {
        u64 title_id{};
        system.GetAppLoader().ReadProgramId(title_id);
        system.GPU().Renderer().Rasterizer()->SwitchDiskResources(title_id);
    }
    system.GPU().Renderer().Rasterizer()->ClearAll(true);
    // The signal executes at a quiescent point inside RunLoop, so give it a few slices.
    if (options.loadstate_slot >= 0) {
        // A harness tool: the states come from whatever build saved them, and every commit
        // changes the revision the header is checked against.
        system.SetLoadStateAnyBuild(true);
        if (!system.SendSignal(Core::System::Signal::Load,
                               static_cast<u32>(options.loadstate_slot))) {
            LOG_CRITICAL(Frontend, "Could not queue the savestate load");
        } else {
            for (int i = 0; i < 400; i++) {
                const auto status = system.RunLoop();
                if (status != Core::System::ResultStatus::Success) {
                    LOG_CRITICAL(Frontend, "Savestate load failed: {} ({})",
                                 static_cast<int>(status), system.GetStatusDetails());
                    break;
                }
                if (!system.IsSaveStateRequestPending()) {
                    break;
                }
            }
        }
    }

    if (!options.record_path.empty()) {
        movie.StartRecording(options.record_path, "citra_sdl");
        LOG_INFO(Frontend, "Recording inputs to {}", options.record_path);
    }
    if (!options.playback_path.empty()) {
        movie.StartPlayback(options.playback_path);
        LOG_INFO(Frontend, "Replaying inputs from {}", options.playback_path);
    }

    // This thread is the emulation thread: it runs the guest and feeds the render thread.
    Common::SetCurrentThreadRole(Common::ThreadRole::Emulation);

    const auto started = std::chrono::steady_clock::now();
    long frames = 0;
    int exit_code = 0;

    while (true) {
        window.PollEvents();
        if (!input_port.Poll(window) || window.IsQuitRequested()) {
            break;
        }

        const auto status = system.RunLoop();
        if (status == Core::System::ResultStatus::ErrorSavestate) {
            // A refused or failed save state is not fatal; the emulation keeps running.
            LOG_ERROR(Frontend, "Save state operation failed: {}", system.GetStatusDetails());
        } else if (status != Core::System::ResultStatus::Success) {
            LOG_CRITICAL(Frontend, "Emulation stopped: {}", system.GetStatusDetails());
            exit_code = 2;
            break;
        }

        window.Present();
        frames++;

        // Once a second, upstream's perf stats: what the console's stats window shows.
        {
            static auto last_stats = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            if (now - last_stats >= std::chrono::seconds(1)) {
                last_stats = now;
                const auto perf = system.GetAndResetPerfStats();
                LOG_INFO(Frontend, "PIPE speed {:.0f}% game {:.1f} fps shown {:.1f} fps",
                         perf.emulation_speed * 100.0, perf.game_fps, perf.system_fps);
            }
        }

        if (input_port.IsOpen() && (frames % 64) == 0) {
            const auto so_far = std::chrono::steady_clock::now() - started;
            // Integer milliseconds, then divide: the duration<double> cast came back inf on the
            // armhf non-LTO build.
            const double elapsed_s =
                std::chrono::duration_cast<std::chrono::milliseconds>(so_far).count() / 1000.0;
            char status[160];
            std::snprintf(status, sizeof(status), "running frames=%ld elapsed=%.1fs rate=%.1f/s",
                          frames, elapsed_s, elapsed_s > 0.0 ? frames / elapsed_s : 0.0);
            input_port.SetStatus(status);
        }

        if (options.seconds_limit >= 0) {
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >=
                options.seconds_limit) {
                LOG_INFO(Frontend, "Reached the {} second limit", options.seconds_limit);
                break;
            }
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - started;
    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(elapsed).count();
    LOG_INFO(Frontend, "Ran {} frames in {:.2f}s ({:.2f} fps)", frames, seconds,
             seconds > 0.0 ? frames / seconds : 0.0);
    if (!options.screenshot_path.empty()) {
        if (window.SaveScreenshot(options.screenshot_path)) {
            LOG_INFO(Frontend, "Wrote {}", options.screenshot_path);
        } else {
            LOG_ERROR(Frontend, "Could not write {}", options.screenshot_path);
            exit_code = exit_code == 0 ? 3 : exit_code;
        }
    }

    // Flushes the movie to disk, so a recording survives even an interrupted run.
    movie.Shutdown();

    if (system.IsPoweredOn()) {
        system.Shutdown();
    }
    InputCommon::Shutdown();
    Common::Log::Stop();
    return exit_code;
}

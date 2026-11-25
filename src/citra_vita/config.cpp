// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "citra_vita/config.h"
#include "citra_vita/emu_window_vita.h"
#include "citra_vita/i18n.h"
#include "citra_vita/input_factory_vita.h"
#include "citra_vita/sgxd_present.h"
#include "citra_vita/stats_hud.h"
#include "citra_vita/uvc_stream.h"
#include "common/assert.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/loader/title_patches.h"
#include "common/mutex.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "video_core/renderer_sgxd/render_thread.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"

namespace VitaFrontend::Config {

namespace {

/// Azahar's settings. A build with another title ID (AZAHAR_VITA_TITLEID), installed beside it,
/// keeps its own file and shares every other one in ux0:data/azahar (games, their updates and
/// saves, savestates, mods, cheats); it starts from Azahar's the first time.
constexpr char SharedSettingsPath[] = "ux0:data/azahar/settings.txt";
constexpr bool OwnSettings = std::string_view{AZAHAR_VITA_TITLEID} != "AZAHARPSV";
constexpr const char* SettingsPath =
    OwnSettings ? "ux0:data/azahar/settings-" AZAHAR_VITA_TITLEID ".txt" : SharedSettingsPath;
/// The file's section of the values for all games; every other section is a title's.
constexpr char GlobalSection[] = "all games";

constexpr std::size_t Count = static_cast<std::size_t>(Option::Count);

/// One option as the file and the emulator know it. A toggle and a choice are kept as words (a
/// toggle's "off" and "on", a choice's by index), a slider as its number.
struct Definition {
    const char* key;
    bool per_title;
    bool runtime;
    int minimum;
    int maximum;
    int default_value;
    std::vector<const char*> words;
};

const std::vector<const char*> Toggle{"off", "on"};
/// Widescreen's: "on" the hack, as when it was a toggle.
const std::vector<const char*> WidescreenWords{"off", "on", "stretch"};
const std::vector<const char*> Layouts{"default", "single", "large",  "side",
                                      "hybrid",  "overlay", "largeoverlay"};
/// The file's word for a custom screen layout: this, then its name.
constexpr char CustomLayoutWord[] = "custom:";
const std::vector<const char*> Scales{"1x", "1.25x", "1.5x", "1.75x", "2x"};
const std::vector<const char*> HudScales{"1x", "1.25x", "1.5x"};
const std::vector<const char*> Skips{"0", "1", "2", "3", "4", "none"};
const std::vector<const char*> Regions{"auto",      "japan", "usa",   "europe",
                                       "australia", "china", "korea", "taiwan"};
/// Service::CFG::SystemLanguage's order.
const std::vector<const char*> SystemLanguages{
    "japanese", "english", "french", "german",     "italian", "spanish",
    "chinese_simplified", "korean", "dutch", "portuguese", "russian", "chinese_traditional"};
const std::vector<const char*> Themes{"dark", "light"};
const std::vector<const char*> UsbVideoScreens{"off", "top", "bottom"};
const std::vector<const char*> Languages{"english", "chinese_simplified"};
const std::vector<const char*> ControlLayoutWords{"default"};
const std::vector<const char*> RightStickWords{"cstick", "lr", "dpad"};
const std::vector<const char*> ShortcutWords{"ps_circle", "ps_cross",   "ps_triangle", "ps_square",
                                             "ps_up",     "ps_down",    "ps_left",     "ps_right",
                                             "ps_l",      "ps_r",       "ps_start",    "ps_select",
                                             "rear_left", "rear_right", "none"};
const std::vector<const char*> Inputs{"circle", "cross",  "triangle",  "square",     "up",
                                      "down",   "left",   "right",     "l",          "r",
                                      "start",  "select", "rear_left", "rear_right", "none"};

/// In Option's order (checked at Load).
const std::array<Definition, Count> definitions{{
    {"screen_layout", true, true, 0, static_cast<int>(Layout::Count) - 1, 0, Layouts},
    {"swap_screens", true, true, 0, 1, 0, Toggle},
    {"overlay_opacity", true, true, 10, 100, 60, {}},
    {"widescreen", true, true, 0, 2, 0, WidescreenWords},
    {"top_scale", true, true, 0, 4, 0, Scales},
    {"msaa", true, true, 0, 1, 0, Toggle},
    {"hud_scale", true, true, 0, 2, 0, HudScales},
    {"stats_overlay", false, true, 0, 1, 1, Toggle},
    {"vsync", true, true, 0, 1, 1, Toggle},
    // Full speed: applied only with vsync off (vsync holds the display's rate itself).
    {"speed_limit", true, true, 0, 200, 100, {}},
    {"frame_skip_limit", true, true, 0, NoFrameSkipLimit, 2, Skips},
    {"wait_for_gpu", true, true, 0, 1, 1, Toggle},
    {"skip_right_eye", true, true, 0, 1, 1, Toggle},
    {"even_frame_pacing", true, true, 0, 1, 0, Toggle},
    {"fast_shadows", true, true, 0, 1, 1, Toggle},
    {"pv_effect_lighting", true, true, 0, 1, 0, Toggle},
    {"parallel_geometry", true, true, 0, 1, 0, Toggle},
    {"simulate_3ds_gpu_timings", true, true, 0, 1, 0, Toggle},
    {"cpu_clock_percentage", true, false, 5, 400, 100, {}},
    {"multicore", true, false, 0, 1, 1, Toggle},
    {"stretch_audio", false, true, 0, 1, 1, Toggle},
    // The 3DS's layout by position (its A where the Vita's circle is), ZL and ZR on the rear
    // panel's halves.
    {"map_a", true, true, 0, 14, 0, Inputs},
    {"map_b", true, true, 0, 14, 1, Inputs},
    {"map_x", true, true, 0, 14, 2, Inputs},
    {"map_y", true, true, 0, 14, 3, Inputs},
    {"map_up", true, true, 0, 14, 4, Inputs},
    {"map_down", true, true, 0, 14, 5, Inputs},
    {"map_left", true, true, 0, 14, 6, Inputs},
    {"map_right", true, true, 0, 14, 7, Inputs},
    {"map_l", true, true, 0, 14, 8, Inputs},
    {"map_r", true, true, 0, 14, 9, Inputs},
    {"map_zl", true, true, 0, 14, 12, Inputs},
    {"map_zr", true, true, 0, 14, 13, Inputs},
    {"map_start", true, true, 0, 14, 10, Inputs},
    {"map_select", true, true, 0, 14, 11, Inputs},
    {"control_layout", true, true, 0, 0, 0, ControlLayoutWords},
    {"motion", true, true, 0, 1, 1, Toggle},
    {"hidden_screen_touch", true, true, 0, 1, 1, Toggle},
    {"right_stick", true, true, 0, static_cast<int>(RightStickMode::Count) - 1, 0, RightStickWords},
    // PS + cross.
    {"swap_shortcut", true, true, 0, static_cast<int>(Shortcut::Count) - 1, 1, ShortcutWords},
    {"region", false, false, 0, 7, 0, Regions},
    // English, the config save's own on a new card.
    {"system_language", false, false, 0, 11, 1, SystemLanguages},
    {"force_new_3ds", true, false, 0, 1, 0, Toggle},
    {"theme", false, true, 0, 1, 0, Themes},
    {"startup_notice", false, true, 0, 1, 1, Toggle},
    {"usb_video", false, true, 0, 2, 0, UsbVideoScreens},
    // English here; Load starts from the console's language.
    {"language", false, true, 0, 1, 0, Languages},
}};

/// The screen layouts' choices as the emulator's layout options, for its settings' log (the window
/// places the screens itself, EmuWindowVita::SetLayout); the overlays are single screen ones.
constexpr std::array<Settings::LayoutOption, static_cast<std::size_t>(Layout::Count)> layouts{
    Settings::LayoutOption::Default,      Settings::LayoutOption::SingleScreen,
    Settings::LayoutOption::LargeScreen,  Settings::LayoutOption::SideScreen,
    Settings::LayoutOption::HybridScreen, Settings::LayoutOption::SingleScreen,
    Settings::LayoutOption::SingleScreen};

using Values = std::array<std::optional<int>, Count>;

Common::Mutex mutex;
std::array<int, Count> global_values{};
std::map<std::string, Values> title_values;
Values pins{};
std::string running_title;
bool dirty = false;

/// What ApplyLocked last applied and where it came from, for a crash report (WriteApplied), which
/// reads them without the lock.
enum class Source : u8 { AllGames, Title, Pin };
std::array<std::atomic<int>, Count> applied_values{};
std::array<std::atomic<Source>, Count> applied_sources{};
char applied_title[256];

/// The 3DS's user name the player chose (ConsoleName), empty for the console's own; kept in all
/// games' section. A copy for WriteApplied, which reads it without the lock.
constexpr char ConsoleNameKey[] = "console_name";
std::string console_name;
/// ConsoleOwnName's, kept in all games' section too.
constexpr char ConsoleOwnNameKey[] = "console_own_name";
std::string console_own_name;
char applied_console_name[64];
/// RomFolders', kept in all games' section, a line each.
constexpr char RomFolderKey[] = "rom_folder";
std::vector<std::string> rom_folders;

void NoteConsoleNameLocked() {
    std::snprintf(applied_console_name, sizeof(applied_console_name), "%s", console_name.c_str());
}

const Definition& Of(Option option) {
    return definitions[static_cast<std::size_t>(option)];
}

/// The option's last value: the screen layout's after the built-in ones, its custom layouts'.
int MaximumOf(Option option) {
    const int custom = option == Option::ScreenLayout ? static_cast<int>(ScreenLayouts::Count())
                       : option == Option::ControlLayout
                           ? static_cast<int>(ControlLayouts::Count())
                           : 0;
    return Of(option).maximum + custom;
}

int Clamp(Option option, int value) {
    return std::clamp(value, Of(option).minimum, MaximumOf(option));
}

/// A title's options it does not set yet, set to all games' values. Whether any was.
bool FillFromGlobal(Values& values) {
    bool filled = false;
    for (std::size_t i = 0; i < Count; i++) {
        if (definitions[i].per_title && !values[i]) {
            values[i] = global_values[i];
            filled = true;
        }
    }
    return filled;
}

/// What runs for `title` (empty: no title): the pin, else its own value, else all games'.
int EffectiveForLocked(const std::string& title, Option option) {
    const std::size_t i = static_cast<std::size_t>(option);
    if (pins[i]) {
        return *pins[i];
    }
    if (!title.empty()) {
        if (const auto it = title_values.find(title); it != title_values.end() && it->second[i]) {
            return *it->second[i];
        }
    }
    return global_values[i];
}

int EffectiveLocked(Option option) {
    return EffectiveForLocked(running_title, option);
}

Source SourceLocked(Option option) {
    const std::size_t i = static_cast<std::size_t>(option);
    if (pins[i]) {
        return Source::Pin;
    }
    const auto it = title_values.find(running_title);
    return !running_title.empty() && it != title_values.end() && it->second[i] ? Source::Title
                                                                              : Source::AllGames;
}

/// Writes the option's effective value where the emulator reads it.
void ApplyLocked(Option option) {
    const int value = EffectiveLocked(option);
    applied_values[static_cast<std::size_t>(option)].store(value, std::memory_order_relaxed);
    applied_sources[static_cast<std::size_t>(option)].store(SourceLocked(option),
                                                            std::memory_order_relaxed);
    switch (option) {
    case Option::ScreenLayout:
        if (const auto custom = CustomLayoutIndex(value)) {
            // Its transformations as the file has them now (a value past the last is clamped
            // before it gets here).
            Settings::values.layout_option = Settings::LayoutOption::CustomLayout;
            EmuWindowVita::SetLayout(*ScreenLayouts::Get(*custom));
        } else {
            Settings::values.layout_option = layouts[static_cast<std::size_t>(value)];
            EmuWindowVita::SetLayout(static_cast<Layout>(value));
        }
        break;
    case Option::SwapScreens:
        Settings::values.swap_screen = value != 0;
        break;
    case Option::OverlayOpacity:
        EmuWindowVita::SetOverlayOpacity(static_cast<u32>(value));
        break;
    case Option::Widescreen:
        EmuWindowVita::SetWidescreen(static_cast<EmuWindowVita::Wide>(value));
        break;
    case Option::TopScale:
        // In quarters: 4 is the 3DS's resolution (Scales: 1x to 2x).
        SgxdRenderer::RasterizerSgxd::SetTopScale(TopScaleQuarters(value));
        break;
    case Option::Msaa:
        SgxdRenderer::RasterizerSgxd::SetTopMsaa(value != 0);
        break;
    case Option::HudScale:
        SgxdRenderer::RasterizerSgxd::SetHudScale(4 + static_cast<u32>(value));
        break;
    case Option::StatsOverlay:
        StatsHud::SetShown(value != 0);
        break;
    case Option::Vsync:
        Settings::values.use_vsync = value != 0;
        // The speed limit waits while vsync is on.
        ApplyLocked(Option::SpeedLimit);
        break;
    case Option::SpeedLimit: {
        // With vsync on, none: vsync holds the emulation to the display's rate
        // (EmuWindowVita::PaceFrame). A pinned limit is a scripted run's own and applies as asked.
        const bool applies = pins[static_cast<std::size_t>(Option::SpeedLimit)].has_value() ||
                             EffectiveLocked(Option::Vsync) == 0;
        Settings::values.frame_limit = applies ? static_cast<double>(value) : 0.0;
        break;
    }
    case Option::FrameSkipLimit:
        SgxdRenderer::RenderThread::SetMaxScreenSkips(
            value == NoFrameSkipLimit ? SgxdRenderer::RenderThread::NoScreenSkipLimit
                                      : static_cast<u32>(value));
        break;
    case Option::WaitForGpu:
        SgxdRenderer::RenderThread::SetReadbackHolds(value != 0);
        break;
    case Option::SimulateGpuTimings:
        Settings::values.simulate_3ds_gpu_timings = value != 0;
        break;
    case Option::CpuClock:
        // Into the timing as a title starts (RunTitle), and a reset's new one.
        Settings::values.cpu_clock_percentage = value;
        break;
    case Option::Multicore:
        // Read by a title's load (ApplyForLoad before it), and a reset's.
        Settings::values.multicore = value != 0;
        break;
    case Option::SkipRightEye:
        Settings::values.disable_right_eye_render = value != 0;
        break;
    case Option::EvenPacing:
        SgxdRenderer::RenderThread::even_pacing.store(value != 0, std::memory_order_relaxed);
        break;
    case Option::FastShadows:
        SgxdRenderer::RasterizerSgxd::SetFastShadows(value != 0);
        break;
    case Option::PvEffectLighting:
        SgxdRenderer::RasterizerSgxd::SetPvEffectLighting(value != 0);
        break;
    case Option::ParallelGeometry:
        SgxdRenderer::RasterizerSgxd::SetParallelGeometry(value != 0);
        break;
    case Option::StretchAudio:
        Settings::values.enable_audio_stretching = value != 0;
        break;
    case Option::MapA:
    case Option::MapB:
    case Option::MapX:
    case Option::MapY:
    case Option::MapUp:
    case Option::MapDown:
    case Option::MapLeft:
    case Option::MapRight:
    case Option::MapL:
    case Option::MapR:
    case Option::MapZL:
    case Option::MapZR:
    case Option::MapStart:
    case Option::MapSelect:
        // A control layout of the player's maps every button in their place.
        if (!CustomControlIndex(EffectiveLocked(Option::ControlLayout))) {
            SetControl(static_cast<int>(option) - static_cast<int>(Option::MapA),
                       static_cast<ControlInput>(value));
        }
        break;
    case Option::ControlLayout:
        if (const auto custom = CustomControlIndex(value)) {
            // Its mapping and on-screen buttons as the file has them now (a value past the last
            // is clamped before it gets here).
            const std::optional<ControlLayouts::ControlLayout> layout =
                ControlLayouts::Get(*custom);
            ASSERT_MSG(layout, "settings: control layout {} of {}", *custom,
                       ControlLayouts::Count());
            for (std::size_t b = 0; b < ControlLayouts::ButtonCount; b++) {
                SetControl(static_cast<int>(b), static_cast<ControlInput>(layout->inputs[b]));
            }
            EmuWindowVita::SetOnScreenButtons(*layout);
        } else {
            for (int b = 0; b < static_cast<int>(ControlLayouts::ButtonCount); b++) {
                SetControl(b, static_cast<ControlInput>(EffectiveLocked(
                                  static_cast<Option>(static_cast<int>(Option::MapA) + b))));
            }
            EmuWindowVita::SetOnScreenButtons(std::nullopt);
        }
        break;
    case Option::Motion:
        SetMotion(value != 0);
        break;
    case Option::HiddenScreenTouch:
        EmuWindowVita::hidden_screen_touch.store(value != 0, std::memory_order_relaxed);
        break;
    case Option::RightStick:
        SetRightStick(static_cast<RightStickMode>(value));
        break;
    case Option::SwapShortcut: {
        const auto shortcut = static_cast<Shortcut>(value);
        EmuWindowVita::SetSwapShortcut(
            shortcut == Shortcut::None ? 0u : InputMask(static_cast<ControlInput>(value)),
            shortcut < Shortcut::RearLeft);
        break;
    }
    case Option::Region:
        // Index 0 is the automatic choice, Settings' -1.
        Settings::values.region_value = value - 1;
        break;
    case Option::SystemLanguage:
        // Into the config save as a title loads (ApplySystemLanguage), where CFG's module is.
        break;
    case Option::ForceNew3ds:
        // Read by a title's load (ApplyForLoad before it), and a reset's.
        Settings::values.is_new_3ds = value != 0;
        break;
    case Option::Theme:
        // Nothing of the emulator's: the menus read it as they draw (Ui::Update).
        break;
    case Option::StartupNotice:
        // Nothing of the emulator's: the start reads it (EmulatorMain).
        break;
    case Option::UsbVideo:
        // The Vita keeps the player's layout: single screen and swap show the other screen alone.
        UvcStream::SetScreen(value);
        break;
    case Option::Language:
        I18n::SetLanguage(static_cast<I18n::Language>(value));
        break;
    case Option::Count:
        UNREACHABLE();
    }
}

void ApplyAllLocked() {
    for (std::size_t i = 0; i < Count; i++) {
        ApplyLocked(static_cast<Option>(i));
    }
}

/// A file's value for the option: a word of its list, or a number in its range.
std::optional<int> ParseValue(Option option, const std::string& text) {
    const Definition& d = Of(option);
    // A custom screen layout by its name.
    if (option == Option::ScreenLayout && text.starts_with(CustomLayoutWord)) {
        const auto index = ScreenLayouts::Find(text.substr(std::strlen(CustomLayoutWord)));
        return index ? std::optional<int>{CustomLayoutValue(*index)} : std::nullopt;
    }
    if (option == Option::ControlLayout && text.starts_with(CustomLayoutWord)) {
        const auto index = ControlLayouts::Find(text.substr(std::strlen(CustomLayoutWord)));
        return index ? std::optional<int>{CustomControlValue(*index)} : std::nullopt;
    }
    if (!d.words.empty()) {
        for (std::size_t i = 0; i < d.words.size(); i++) {
            if (text == d.words[i]) {
                return static_cast<int>(i);
            }
        }
        return std::nullopt;
    }
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || value < d.minimum || value > d.maximum) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::string FormatValue(Option option, int value) {
    const Definition& d = Of(option);
    if (const auto custom = CustomLayoutIndex(value); custom && option == Option::ScreenLayout) {
        return CustomLayoutWord + ScreenLayouts::Get(*custom)->name;
    }
    if (const auto custom = CustomControlIndex(value); custom && option == Option::ControlLayout) {
        return CustomLayoutWord + ControlLayouts::Get(*custom)->name;
    }
    if (!d.words.empty()) {
        return d.words[static_cast<std::size_t>(value)];
    }
    return std::to_string(value);
}

/// A console name within the 3DS's limit: MaxConsoleName UTF-16 units, cut between characters.
std::string LimitConsoleName(const std::string& name) {
    std::u16string units = Common::UTF8ToUTF16(name);
    if (units.size() > MaxConsoleName) {
        // A surrogate pair's first half at the cut goes with it.
        std::size_t keep = MaxConsoleName;
        if (units[keep - 1] >= 0xD800 && units[keep - 1] <= 0xDBFF) {
            keep--;
        }
        units.resize(keep);
    }
    return Common::UTF16ToUTF8(units);
}

std::string Trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// A folder as the memory devices' file systems tell folders apart: letters' case, a slash after
/// the device ("ux0:/data" is "ux0:data") and at the end left out.
std::string FolderKey(const std::string& folder) {
    std::string key = Common::ToLower(folder);
    if (const std::size_t colon = key.find(':');
        colon != std::string::npos && colon + 1 < key.size() && key[colon + 1] == '/') {
        key.erase(colon + 1, 1);
    }
    while (!key.empty() && key.back() == '/') {
        key.pop_back();
    }
    return key;
}

bool AddRomFolderLocked(const std::string& folder) {
    const std::string key = FolderKey(folder);
    // A path with no device is not one the Vita opens.
    if (key.find(':') == std::string::npos || key == FolderKey("ux0:data/azahar/roms") ||
        std::any_of(rom_folders.begin(), rom_folders.end(),
                    [&](const std::string& f) { return FolderKey(f) == key; })) {
        return false;
    }
    rom_folders.push_back(folder);
    return true;
}

} // Anonymous namespace

bool PerTitle(Option option) {
    return Of(option).per_title;
}

bool Runtime(Option option) {
    return Of(option).runtime;
}

int Minimum(Option option) {
    return Of(option).minimum;
}

int Maximum(Option option) {
    return MaximumOf(option);
}

int Default(Option option) {
    return Of(option).default_value;
}

void Load() {
    std::scoped_lock lock{mutex};
    // The custom layouts first: the file names those it chooses.
    ScreenLayouts::Load();
    ControlLayouts::Load();
    for (std::size_t i = 0; i < Count; i++) {
        const Definition& d = definitions[i];
        ASSERT_MSG(d.words.empty() || static_cast<int>(d.words.size()) == d.maximum + 1,
                   "settings: {}'s words do not match its range", d.key);
        global_values[i] = d.default_value;
    }
    // The menus in the console's language until the player picks one.
    global_values[static_cast<std::size_t>(Option::Language)] =
        static_cast<int>(I18n::ConsoleLanguage());
    FILE* f = std::fopen(SettingsPath, "r");
    // Beside Azahar with no file of its own yet: Azahar's settings to start from, written to its
    // own at the next save.
    const bool from_shared = f == nullptr && OwnSettings &&
                             (f = std::fopen(SharedSettingsPath, "r")) != nullptr;
    if (f != nullptr) {
        // A line is a section's name in brackets, `key = value` or a comment. A title's section is
        // named for its key, which a file's name makes anything but a newline: the name
        // runs from the first bracket to the last.
        Values* section = nullptr;
        bool in_global = false;
        char buffer[512];
        int line_number = 0;
        while (std::fgets(buffer, sizeof(buffer), f) != nullptr) {
            line_number++;
            const std::string line = Trim(buffer);
            if (line.empty() || line[0] == '#') {
                continue;
            }
            if (line.front() == '[' && line.back() == ']') {
                const std::string name = line.substr(1, line.size() - 2);
                in_global = name == GlobalSection;
                section = in_global ? nullptr : &title_values[name];
                continue;
            }
            const std::size_t equals = line.find('=');
            const std::string key = Trim(line.substr(0, equals));
            if (equals != std::string::npos && key == ConsoleNameKey && in_global) {
                console_name = LimitConsoleName(Trim(line.substr(equals + 1)));
                continue;
            }
            if (equals != std::string::npos && key == ConsoleOwnNameKey && in_global) {
                console_own_name = Trim(line.substr(equals + 1));
                continue;
            }
            if (equals != std::string::npos && key == RomFolderKey && in_global) {
                AddRomFolderLocked(Trim(line.substr(equals + 1)));
                continue;
            }
            const auto definition = std::find_if(definitions.begin(), definitions.end(),
                                                 [&](const Definition& d) { return key == d.key; });
            if (equals == std::string::npos || definition == definitions.end() ||
                (!in_global && section == nullptr)) {
                LOG_WARNING(Frontend, "{} line {} not understood: {}", SettingsPath, line_number,
                            line);
                continue;
            }
            const auto option = static_cast<Option>(definition - definitions.begin());
            const std::optional<int> value = ParseValue(option, Trim(line.substr(equals + 1)));
            if (!value || (section != nullptr && !definition->per_title)) {
                LOG_WARNING(Frontend, "{} line {}: {} is not a value of {} here", SettingsPath,
                            line_number, line.substr(equals + 1), key);
                continue;
            }
            if (in_global) {
                global_values[static_cast<std::size_t>(option)] = *value;
            } else {
                (*section)[static_cast<std::size_t>(option)] = *value;
            }
        }
        std::fclose(f);
        LOG_INFO(Frontend, "Settings read from {}: {} titles with their own",
                 from_shared ? SharedSettingsPath : SettingsPath,
                 title_values.size());
    }
    // A title's section with nothing left in it is not kept.
    std::erase_if(title_values, [](const auto& entry) {
        return std::none_of(entry.second.begin(), entry.second.end(),
                            [](const std::optional<int>& v) { return v.has_value(); });
    });
    // A section written before a game's first value took all games' others (SetTitleValue):
    // those it follows still become its own, at the values that apply to it now.
    bool filled = false;
    for (auto& [title, values] : title_values) {
        filled |= FillFromGlobal(values);
    }
    dirty = filled || from_shared;
    NoteConsoleNameLocked();
    ApplyAllLocked();
}

namespace {

void SaveLocked() {
    if (!dirty) {
        return;
    }
    FILE* f = std::fopen(SettingsPath, "w");
    if (f == nullptr) {
        LOG_ERROR(Frontend, "Could not write {}", SettingsPath);
        return;
    }
    std::fprintf(f, "# Azahar's settings: the values for all games, then each title's own (a "
                    "section named for\n# its file's name).\n");
    std::fprintf(f, "[%s]\n", GlobalSection);
    for (std::size_t i = 0; i < Count; i++) {
        std::fprintf(f, "%s = %s\n", definitions[i].key,
                     FormatValue(static_cast<Option>(i), global_values[i]).c_str());
    }
    if (!console_name.empty()) {
        std::fprintf(f, "%s = %s\n", ConsoleNameKey, console_name.c_str());
    }
    if (!console_own_name.empty()) {
        std::fprintf(f, "%s = %s\n", ConsoleOwnNameKey, console_own_name.c_str());
    }
    for (const std::string& folder : rom_folders) {
        std::fprintf(f, "%s = %s\n", RomFolderKey, folder.c_str());
    }
    for (const auto& [title, values] : title_values) {
        std::fprintf(f, "\n[%s]\n", title.c_str());
        for (std::size_t i = 0; i < Count; i++) {
            if (values[i]) {
                std::fprintf(f, "%s = %s\n", definitions[i].key,
                             FormatValue(static_cast<Option>(i), *values[i]).c_str());
            }
        }
    }
    std::fclose(f);
    dirty = false;
    LOG_INFO(Frontend, "Settings written to {}", SettingsPath);
}

} // namespace

void Save() {
    std::scoped_lock lock{mutex};
    SaveLocked();
}

std::size_t SaveScreenLayout(std::optional<std::size_t> index, ScreenLayouts::ScreenLayout layout) {
    std::scoped_lock lock{mutex};
    const std::size_t at = ScreenLayouts::Put(index, std::move(layout));
    // A new name is written where the settings choose it.
    dirty = true;
    ApplyLocked(Option::ScreenLayout);
    SaveLocked();
    return at;
}

void DeleteScreenLayout(std::size_t index) {
    std::scoped_lock lock{mutex};
    ScreenLayouts::Remove(index);
    const int gone = CustomLayoutValue(index);
    const auto follow = [gone](int value) {
        return value == gone ? static_cast<int>(Layout::Default) : value > gone ? value - 1 : value;
    };
    constexpr std::size_t i = static_cast<std::size_t>(Option::ScreenLayout);
    global_values[i] = follow(global_values[i]);
    for (auto& [title, values] : title_values) {
        if (values[i]) {
            values[i] = follow(*values[i]);
        }
    }
    if (pins[i]) {
        pins[i] = follow(*pins[i]);
    }
    dirty = true;
    ApplyLocked(Option::ScreenLayout);
    SaveLocked();
}

std::size_t SaveControlLayout(std::optional<std::size_t> index,
                              ControlLayouts::ControlLayout layout) {
    std::scoped_lock lock{mutex};
    const std::size_t at = ControlLayouts::Put(index, std::move(layout));
    dirty = true;
    ApplyLocked(Option::ControlLayout);
    SaveLocked();
    return at;
}

void DeleteControlLayout(std::size_t index) {
    std::scoped_lock lock{mutex};
    ControlLayouts::Remove(index);
    const int gone = CustomControlValue(index);
    const auto follow = [gone](int value) {
        return value == gone ? 0 : value > gone ? value - 1 : value;
    };
    constexpr std::size_t i = static_cast<std::size_t>(Option::ControlLayout);
    global_values[i] = follow(global_values[i]);
    for (auto& [title, values] : title_values) {
        if (values[i]) {
            values[i] = follow(*values[i]);
        }
    }
    if (pins[i]) {
        pins[i] = follow(*pins[i]);
    }
    dirty = true;
    ApplyLocked(Option::ControlLayout);
    SaveLocked();
}

const char* InputWord(ControlInput input) {
    ASSERT_MSG(input >= ControlInput{} && input < ControlInput::Count, "settings: input {}",
               static_cast<int>(input));
    return Inputs[static_cast<std::size_t>(input)];
}

std::optional<ControlInput> InputFromWord(const std::string& word) {
    for (std::size_t i = 0; i < Inputs.size(); i++) {
        if (word == Inputs[i]) {
            return static_cast<ControlInput>(i);
        }
    }
    return std::nullopt;
}

std::string TitleKeyOf(const std::string& path) {
    // An installed content: .../title/<high>/<low>/content/<id>.app.
    if (const auto at = path.rfind("/title/");
        at != std::string::npos && path.size() > at + 25 && path[at + 15] == '/' &&
        path.compare(at + 24, 9, "/content/") == 0) {
        return path.substr(at + 7, 8) + path.substr(at + 16, 8);
    }
    return std::string{FileUtil::GetFilename(path)};
}

void SetRunningTitle(const std::string& title) {
    std::scoped_lock lock{mutex};
    running_title = title;
    std::snprintf(applied_title, sizeof(applied_title), "%s", title.c_str());
    ApplyAllLocked();
    LOG_INFO(Frontend, "Settings: {}",
             title.empty() ? std::string{"all games'"} : fmt::format("{}'s own", title));
}

void ApplyForLoad(const std::string& title) {
    std::scoped_lock lock{mutex};
    Settings::values.multicore = EffectiveForLocked(title, Option::Multicore) != 0;
    Settings::values.is_new_3ds = EffectiveForLocked(title, Option::ForceNew3ds) != 0;
}

std::string RunningTitle() {
    std::scoped_lock lock{mutex};
    return running_title;
}

int Global(Option option) {
    std::scoped_lock lock{mutex};
    return global_values[static_cast<std::size_t>(option)];
}

void SetGlobal(Option option, int value) {
    std::scoped_lock lock{mutex};
    const std::size_t i = static_cast<std::size_t>(option);
    value = Clamp(option, value);
    dirty |= global_values[i] != value;
    global_values[i] = value;
    pins[i].reset();
    ApplyLocked(option);
}

std::optional<int> TitleValue(const std::string& title, Option option) {
    std::scoped_lock lock{mutex};
    const auto it = title_values.find(title);
    return it == title_values.end() ? std::nullopt : it->second[static_cast<std::size_t>(option)];
}

void SetTitleValue(const std::string& title, Option option, std::optional<int> value) {
    ASSERT_MSG(!title.empty() && PerTitle(option), "settings: {} is not a title's own",
               Of(option).key);
    std::scoped_lock lock{mutex};
    const std::size_t i = static_cast<std::size_t>(option);
    if (value) {
        value = Clamp(option, *value);
    }
    const bool first = value && !title_values.contains(title);
    Values& values = title_values[title];
    if (first) {
        // A game's first value of its own: the others are its own from here on too, all games'
        // values as they are now, so that a change to those later leaves the game as it is
        // (unless the user takes one back: "Use all games' setting").
        FillFromGlobal(values);
        dirty = true;
    }
    dirty |= values[i] != value;
    values[i] = value;
    if (std::none_of(values.begin(), values.end(),
                     [](const std::optional<int>& v) { return v.has_value(); })) {
        title_values.erase(title);
    }
    pins[i].reset();
    ApplyLocked(option);
}

void SeedTitleValues(const std::string& title, u64 program_id) {
    const auto defaults = Loader::TitleSettingDefaultsOf(program_id);
    std::scoped_lock lock{mutex};
    if (title.empty() || defaults.empty() || title_values.contains(title)) {
        return;
    }
    Values& values = title_values[title];
    FillFromGlobal(values);
    for (const Loader::TitleSettingDefault& d : defaults) {
        const auto definition =
            std::find_if(definitions.begin(), definitions.end(),
                         [&](const Definition& def) { return d.key == std::string_view{def.key}; });
        ASSERT_MSG(definition != definitions.end() && definition->per_title,
                   "settings: {} is no title's own setting", d.key);
        const auto option = static_cast<Option>(definition - definitions.begin());
        const std::optional<int> value = ParseValue(option, d.value);
        ASSERT_MSG(value.has_value(), "settings: {} is not a value of {}", d.value, d.key);
        values[static_cast<std::size_t>(option)] = *value;
    }
    dirty = true;
    LOG_INFO(Frontend, "Settings: {} starts from its {} settings of its own", title,
             defaults.size());
}

bool HasTitleValues(const std::string& title) {
    std::scoped_lock lock{mutex};
    return title_values.contains(title);
}

void ClearTitleValues(const std::string& title) {
    std::scoped_lock lock{mutex};
    if (title_values.erase(title) != 0) {
        dirty = true;
        ApplyAllLocked();
    }
}

int Effective(Option option) {
    std::scoped_lock lock{mutex};
    return EffectiveLocked(option);
}

int ForTitle(const std::string& title, Option option) {
    std::scoped_lock lock{mutex};
    const std::size_t i = static_cast<std::size_t>(option);
    if (const auto it = title_values.find(title); it != title_values.end() && it->second[i]) {
        return *it->second[i];
    }
    return global_values[i];
}

void Pin(Option option, int value) {
    std::scoped_lock lock{mutex};
    pins[static_cast<std::size_t>(option)] = Clamp(option, value);
    ApplyLocked(option);
}

std::optional<int> PinOf(Option option) {
    std::scoped_lock lock{mutex};
    return pins[static_cast<std::size_t>(option)];
}

void Unpin(Option option) {
    std::scoped_lock lock{mutex};
    pins[static_cast<std::size_t>(option)].reset();
    ApplyLocked(option);
}

std::string ConsoleName() {
    std::scoped_lock lock{mutex};
    return console_name;
}

void SetConsoleName(const std::string& name) {
    std::scoped_lock lock{mutex};
    const std::string wanted = LimitConsoleName(Trim(name));
    dirty |= wanted != console_name;
    console_name = wanted;
    NoteConsoleNameLocked();
}

std::string ConsoleOwnName() {
    std::scoped_lock lock{mutex};
    return console_own_name;
}

void SetConsoleOwnName(const std::string& name) {
    std::scoped_lock lock{mutex};
    dirty |= name != console_own_name;
    console_own_name = name;
}

std::vector<std::string> RomFolders() {
    std::scoped_lock lock{mutex};
    return rom_folders;
}

bool AddRomFolder(const std::string& folder) {
    std::scoped_lock lock{mutex};
    const bool added = AddRomFolderLocked(folder);
    dirty |= added;
    return added;
}

void RemoveRomFolder(std::size_t index) {
    std::scoped_lock lock{mutex};
    if (index < rom_folders.size()) {
        rom_folders.erase(rom_folders.begin() + static_cast<std::ptrdiff_t>(index));
        dirty = true;
    }
}

std::size_t WriteApplied(char* out, std::size_t size) {
    std::size_t at = 0;
    const auto add = [&](const char* format, auto... args) {
        if (at + 1 < size) {
            const int n = std::snprintf(out + at, size - at, format, args...);
            at = n > 0 ? std::min(at + static_cast<std::size_t>(n), size - 1) : at;
        }
    };
    add("[azahar] settings: %s (* the game's own, ! a launch argument's)\n",
        applied_title[0] != '\0' ? applied_title : "no game running");
    std::size_t line = at;
    add("[azahar] settings:");
    for (std::size_t i = 0; i < Count; i++) {
        const Definition& d = definitions[i];
        const int value = applied_values[i].load(std::memory_order_relaxed);
        const Source source = applied_sources[i].load(std::memory_order_relaxed);
        const char* const mark = source == Source::Title ? "*" : source == Source::Pin ? "!" : "";
        if (at - line > 90) {
            add("\n");
            line = at;
            add("[azahar] settings:");
        }
        if (value >= 0 && static_cast<std::size_t>(value) < d.words.size()) {
            add(" %s=%s%s", d.key, d.words[static_cast<std::size_t>(value)], mark);
        } else if (const auto custom = CustomLayoutIndex(value);
                   custom && static_cast<Option>(i) == Option::ScreenLayout) {
            add(" %s=custom%u%s", d.key, static_cast<unsigned>(*custom), mark);
        } else if (const auto control = CustomControlIndex(value);
                   control && static_cast<Option>(i) == Option::ControlLayout) {
            add(" %s=custom%u%s", d.key, static_cast<unsigned>(*control), mark);
        } else {
            add(" %s=%d%s", d.key, value, mark);
        }
    }
    add(" %s=%s\n", ConsoleNameKey,
        applied_console_name[0] != '\0' ? applied_console_name : "(the console's)");
    return at;
}

/// The options a game runs differently with (WriteChangedFromDefaults): those of the screen and
/// speed rows that change what the emulation draws or when, not where the screens are shown.
constexpr std::array crash_options{Option::Widescreen,   Option::TopScale,
                                   Option::Msaa,         Option::Vsync,
                                   Option::SpeedLimit,   Option::FrameSkipLimit,
                                   Option::WaitForGpu,   Option::SkipRightEye,
                                   Option::SimulateGpuTimings, Option::CpuClock,
                                   Option::Multicore,    Option::ForceNew3ds};

std::size_t WriteChangedFromDefaults(char* out, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (applied_title[0] == '\0') {
        return 0;
    }
    std::size_t at = 0;
    for (const Option option : crash_options) {
        const Definition& d = Of(option);
        if (applied_values[static_cast<std::size_t>(option)].load(std::memory_order_relaxed) ==
            d.default_value) {
            continue;
        }
        const bool own = applied_sources[static_cast<std::size_t>(option)].load(
                             std::memory_order_relaxed) == Source::Title;
        const int n = std::snprintf(out + at, size - at, "%s%s%s", at == 0 ? "" : ",", d.key,
                                    own ? "*" : "");
        if (n < 0 || at + static_cast<std::size_t>(n) >= size) {
            // A key cut short names no option: the list ends before it.
            out[at] = '\0';
            break;
        }
        at += static_cast<std::size_t>(n);
    }
    return at;
}

std::vector<ChangedOption> OptionsOfKeys(std::string_view keys) {
    std::vector<ChangedOption> options;
    while (!keys.empty()) {
        const std::size_t comma = keys.find(',');
        std::string_view key = keys.substr(0, comma);
        const bool own = key.ends_with('*');
        if (own) {
            key.remove_suffix(1);
        }
        for (std::size_t i = 0; i < Count; i++) {
            if (key == definitions[i].key) {
                options.push_back({static_cast<Option>(i), own});
            }
        }
        keys = comma == std::string_view::npos ? std::string_view{} : keys.substr(comma + 1);
    }
    return options;
}

} // namespace VitaFrontend::Config

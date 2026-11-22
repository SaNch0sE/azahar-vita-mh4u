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
#include <vector>

#include <fmt/format.h>

#include "citra_vita/config.h"
#include "citra_vita/emu_window_vita.h"
#include "common/assert.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/mutex.h"
#include "common/settings.h"
#include "video_core/renderer_sgxd/render_thread.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"

namespace VitaFrontend::Config {

namespace {

constexpr char SettingsPath[] = "ux0:data/azahar/settings.txt";
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
const std::vector<const char*> Scales{"1x", "1.25x", "1.5x", "1.75x", "2x"};
const std::vector<const char*> Skips{"0", "1", "2", "3", "4", "none"};
const std::vector<const char*> Regions{"auto",      "japan", "usa",   "europe",
                                       "australia", "china", "korea", "taiwan"};
const std::vector<const char*> Themes{"dark", "light"};

/// In Option's order (checked at Load).
const std::array<Definition, Count> definitions{{
    {"screen_layout", true, true, 0, static_cast<int>(Layout::Count) - 1, 0, Layouts},
    {"swap_screens", true, true, 0, 1, 0, Toggle},
    {"overlay_opacity", true, true, 10, 100, 60, {}},
    {"widescreen", true, true, 0, 2, 0, WidescreenWords},
    {"top_scale", true, true, 0, 4, 0, Scales},
    {"vsync", true, true, 0, 1, 1, Toggle},
    // Full speed: applied only with vsync off (vsync holds the display's rate itself).
    {"speed_limit", true, true, 0, 200, 100, {}},
    {"frame_skip_limit", true, true, 0, NoFrameSkipLimit, 2, Skips},
    {"wait_for_gpu", true, true, 0, 1, 1, Toggle},
    {"skip_right_eye", true, true, 0, 1, 1, Toggle},
    {"stretch_audio", false, true, 0, 1, 1, Toggle},
    {"right_stick", true, true, 0, static_cast<int>(RightStickMode::Count) - 1, 0, RightStickWords},
    {"region", false, false, 0, 7, 0, Regions},
    {"force_new_3ds", true, false, 0, 1, 0, Toggle},
    {"theme", false, true, 0, 1, 0, Themes},
}};

/// The screen layouts' choices as the emulator's layout options; the overlay is the single
/// screen with EmuWindowVita's overlay on.
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

const Definition& Of(Option option) {
    return definitions[static_cast<std::size_t>(option)];
}

int Clamp(Option option, int value) {
    return std::clamp(value, Of(option).minimum, Of(option).maximum);
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

int EffectiveLocked(Option option) {
    const std::size_t i = static_cast<std::size_t>(option);
    if (pins[i]) {
        return *pins[i];
    }
    if (!running_title.empty()) {
        if (const auto it = title_values.find(running_title);
            it != title_values.end() && it->second[i]) {
            return *it->second[i];
        }
    }
    return global_values[i];
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
        Settings::values.layout_option = layouts[static_cast<std::size_t>(value)];
        EmuWindowVita::SetOverlay(value == static_cast<int>(Layout::Overlay)        ? 3
                                  : value == static_cast<int>(Layout::LargeOverlay) ? 5
                                                                                    : 0);
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
    case Option::SkipRightEye:
        Settings::values.disable_right_eye_render = value != 0;
        break;
    case Option::StretchAudio:
        Settings::values.enable_audio_stretching = value != 0;
        break;
    case Option::RightStick:
        SetRightStick(static_cast<RightStickMode>(value));
        break;
    case Option::Region:
        // Index 0 is the automatic choice, Settings' -1.
        Settings::values.region_value = value - 1;
        break;
    case Option::ForceNew3ds:
        // Read by a title's load (ApplyForLoad before it), and a reset's.
        Settings::values.is_new_3ds = value != 0;
        break;
    case Option::Theme:
        // Nothing of the emulator's: the menus read it as they draw (Ui::Update).
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
    if (!d.words.empty()) {
        return d.words[static_cast<std::size_t>(value)];
    }
    return std::to_string(value);
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
    return Of(option).maximum;
}

void Load() {
    std::scoped_lock lock{mutex};
    for (std::size_t i = 0; i < Count; i++) {
        const Definition& d = definitions[i];
        ASSERT_MSG(d.words.empty() || static_cast<int>(d.words.size()) == d.maximum + 1,
                   "settings: {}'s words do not match its range", d.key);
        global_values[i] = d.default_value;
    }
    if (FILE* f = std::fopen(SettingsPath, "r")) {
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
        LOG_INFO(Frontend, "Settings read from {}: {} titles with their own", SettingsPath,
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
    dirty = filled;
    ApplyAllLocked();
}

void Save() {
    std::scoped_lock lock{mutex};
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
        } else {
            add(" %s=%d%s", d.key, value, mark);
        }
    }
    add("\n");
    return at;
}

} // namespace VitaFrontend::Config

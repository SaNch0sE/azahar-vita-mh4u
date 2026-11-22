// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include <imgui_vita2d/imgui.h>

#include "citra_vita/imgui_impl_sgxd.h"
#include "citra_vita/imgui_impl_vita.h"

#include "citra_vita/crash_report.h"
#include "citra_vita/emu_window_vita.h"
#include "citra_vita/hud_stats.h"
#include "citra_vita/sgxd_present.h"
#include "citra_vita/stats_hud.h"
#include "citra_vita/vita_ui.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/renderer_sgxd/render_thread.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"

namespace VitaFrontend {

namespace {
constexpr float ScreenWidth = 960.0f;
constexpr float ScreenHeight = 544.0f;

/// Where titles are looked for. One directory, no recursion: a console file manager is enough
/// host0 does not open and adds nothing.
constexpr char GameDirectory[] = "ux0:/data/azahar/roms";
#ifdef VITA_DIAGNOSTICS
constexpr char HostGameDirectory[] = "host0:roms";
constexpr char ListedDirectories[] = "ux0:/data/azahar/roms and host0:roms";
#else
constexpr const char* ListedDirectories = GameDirectory;
#endif

bool HasRomExtension(const std::string& name) {
    static constexpr const char* extensions[] = {".3ds", ".3dsx", ".cci", ".cxi",
                                                 ".app", ".elf",  ".axf"};
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = name.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::any_of(std::begin(extensions), std::end(extensions),
                       [&](const char* candidate) { return extension == candidate; });
}

/// A full-screen window with no decoration, which is what every screen here wants: the display
/// is fixed and there is nothing to arrange windows against.
bool BeginFullscreen(const char* id) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth, ScreenHeight));
    return ImGui::Begin(id, nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                            ImGuiWindowFlags_NoBringToFrontOnFocus);
}

/// A centred panel over whatever is behind it, for the pause menu.
bool BeginCentered(const char* id, float width, float height) {
    ImGui::SetNextWindowPos(ImVec2((ScreenWidth - width) * 0.5f, (ScreenHeight - height) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    return ImGui::Begin(id, nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
}

/// The title's program ID, as its file names it; 0 for a file without one (a 3DSX).
u64 ProgramIdOf(const std::string& path) {
    u64 program_id = 0;
    if (const std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(path)) {
        loader->ReadProgramId(program_id);
    }
    return program_id;
}
} // Anonymous namespace

Ui::Ui(Core::System& system_, EmuWindowVita& window_) : system{system_}, window{window_} {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    // The console is held at arm's length, so everything is a size up from a desktop default.
    ImGui::GetIO().FontGlobalScale = 1.2f;

    ImGui_ImplVita_Init();
    ImGui_ImplSgxd_Init();
    StatsHud::Init();
    // The backend has copied the font atlas into GPU memory by now, and the pixels it was
    // built from are 4 MB of heap nothing reads again. ImGui rebuilds them on demand if anything
    // ever does.
    ImGui::GetIO().Fonts->ClearTexData();
    ImGui_ImplVita_TouchUsage(true);
    ImGui_ImplVita_GamepadUsage(true);
    ImGui_ImplVita_MouseStickUsage(true);

    BuildSettings();
    ScanGames();
}

Ui::~Ui() {
    ImGui_ImplSgxd_Shutdown();
    ImGui::DestroyContext();
}

void Ui::ScanGames() {
    games.clear();
    const auto scan = [this](const char* directory, const char* suffix) {
        FileUtil::FSTEntry root;
        if (!FileUtil::ScanDirectoryTree(directory, root)) {
            LOG_WARNING(Frontend, "No game directory at {}", directory);
            return;
        }
        for (const auto& entry : root.children) {
            if (entry.isDirectory || !HasRomExtension(entry.virtualName)) {
                continue;
            }
            games.push_back({entry.physicalName, entry.virtualName + suffix});
        }
    };
    scan(GameDirectory, "");
#ifdef VITA_DIAGNOSTICS
    scan(HostGameDirectory, " (host0)");
#endif
    std::sort(games.begin(), games.end(),
              [](const GameEntry& a, const GameEntry& b) { return a.name < b.name; });
    LOG_INFO(Frontend, "Found {} titles", games.size());
}

void Ui::BuildSettings() {
    const auto toggle = [&](const char* title, const char* description, bool runtime,
                            std::function<bool()> get, std::function<void(bool)> set) {
        Setting setting{};
        setting.title = title;
        setting.description = description;
        setting.kind = Setting::Kind::Toggle;
        setting.runtime = runtime;
        setting.get_bool = std::move(get);
        setting.set_bool = std::move(set);
        settings.push_back(std::move(setting));
    };
    const auto choice = [&](const char* title, const char* description, bool runtime,
                            std::vector<const char*> choices, std::function<int()> get,
                            std::function<void(int)> set) {
        Setting setting{};
        setting.title = title;
        setting.description = description;
        setting.kind = Setting::Kind::Choice;
        setting.runtime = runtime;
        setting.choices = std::move(choices);
        setting.get_choice = std::move(get);
        setting.set_choice = std::move(set);
        settings.push_back(std::move(setting));
    };
    const auto slider = [&](const char* title, const char* description, bool runtime, int min,
                            int max, std::function<int()> get, std::function<void(int)> set) {
        Setting setting{};
        setting.title = title;
        setting.description = description;
        setting.kind = Setting::Kind::Slider;
        setting.runtime = runtime;
        setting.slider_min = min;
        setting.slider_max = max;
        setting.get_slider = std::move(get);
        setting.set_slider = std::move(set);
        settings.push_back(std::move(setting));
    };

    slider("Speed limit", "Percentage of full speed to aim for. Lower it to save battery.", true,
           0, 200, [] { return static_cast<int>(Settings::values.frame_limit.GetValue()); },
           [](int value) { Settings::values.frame_limit = static_cast<double>(value); });
    slider("Volume", "Output volume.", true, 0, 100,
           [] { return static_cast<int>(Settings::values.volume.GetValue() * 100.0f); },
           [](int value) { Settings::values.volume = static_cast<float>(value) / 100.0f; });
    toggle("Stretch audio", "Keeps audio continuous when the emulator runs below full speed.",
           true, [] { return Settings::values.enable_audio_stretching.GetValue(); },
           [](bool value) { Settings::values.enable_audio_stretching = value; });
    // The menu's layouts in its order (the enum's own order has SeparateWindows before Hybrid
    // here), then the overlay layout: the single screen with the other one over it.
    static constexpr Settings::LayoutOption layouts[] = {
        Settings::LayoutOption::Default, Settings::LayoutOption::SingleScreen,
        Settings::LayoutOption::LargeScreen, Settings::LayoutOption::SideScreen,
        Settings::LayoutOption::HybridScreen};
    constexpr int overlay_choice = static_cast<int>(std::size(layouts));
    choice("Screen layout",
           "How the two screens are arranged on the display. Overlay: the single screen with the "
           "other one smaller in the bottom right corner, translucent, and touchable there.",
           true,
           {"Default", "Single screen", "Large screen", "Side by side", "Hybrid",
            "Single screen, overlay"},
           [] {
               const auto option = Settings::values.layout_option.GetValue();
               if (option == Settings::LayoutOption::SingleScreen && EmuWindowVita::Overlay()) {
                   return overlay_choice;
               }
               const auto it = std::find(std::begin(layouts), std::end(layouts), option);
               return it == std::end(layouts) ? 0 : static_cast<int>(it - std::begin(layouts));
           },
           [](int value) {
               EmuWindowVita::SetOverlay(value == overlay_choice);
               Settings::values.layout_option = value == overlay_choice
                                                    ? Settings::LayoutOption::SingleScreen
                                                    : layouts[std::clamp(value, 0, overlay_choice - 1)];
           });
    toggle("Widescreen",
           "Draws the top screen wider than the 3DS's, showing more of the scene at its sides "
           "instead of stretching it, so that it fills the display in the single screen and "
           "overlay layouts. A title may leave the new sides empty where it draws nothing past "
           "its screen's edges.",
           true, [] { return EmuWindowVita::Widescreen(); },
           [](bool value) { EmuWindowVita::SetWidescreen(value); });
    slider("Overlay opacity", "How opaque the overlaid screen is, in percent.", true, 10, 100,
           [] { return static_cast<int>(EmuWindowVita::OverlayOpacity()); },
           [](int value) { EmuWindowVita::SetOverlayOpacity(static_cast<u32>(value)); });
    choice("Top screen resolution",
           "Draws the top screen's picture above the 3DS's resolution: sharper in the single and "
           "large screen layouts, the GPU's work for that picture growing with the scale's "
           "square (about 2.25 times at 1.5x).",
           true, {"Native", "1.25x", "1.5x"},
           [] { return static_cast<int>(SgxdRenderer::RasterizerSgxd::TopScale()) - 4; },
           [](int value) { SgxdRenderer::RasterizerSgxd::SetTopScale(static_cast<u32>(value) + 4); });
    // The late skip's cap (RenderThread::MaxScreenSkips): 0 to 4 frames, or none.
    static constexpr int no_skip_limit = 5;
    choice("Frame skip limit",
           "When the GPU falls behind, the screen that draws more is left out of a frame, so the "
           "game keeps its speed and shows fewer pictures. This is how many frames in a row a "
           "screen may be left out: higher keeps GPU-heavy games faster, with fewer pictures. "
           "Off draws every frame, and the game slows down instead.",
           true, {"Off", "1 frame", "2 frames", "3 frames", "4 frames", "No limit"},
           [] {
               const u32 frames = SgxdRenderer::RenderThread::MaxScreenSkips();
               return frames == SgxdRenderer::RenderThread::NoScreenSkipLimit
                          ? no_skip_limit
                          : static_cast<int>(std::min<u32>(frames, no_skip_limit - 1));
           },
           [](int value) {
               SgxdRenderer::RenderThread::SetMaxScreenSkips(
                   value == no_skip_limit ? SgxdRenderer::RenderThread::NoScreenSkipLimit
                                          : static_cast<u32>(value));
           });
    toggle(
        "Vsync",
        "Shows each picture from the display's next refresh, whole. Off, a picture goes on "
        "screen at once, partway through the refresh it lands in, and tears.",
        true, [] { return Settings::values.use_vsync.GetValue(); },
        [](bool value) { Settings::values.use_vsync = value; });
    toggle("Swap screens", "Puts the touch screen where the top screen would be.", true,
           [] { return Settings::values.swap_screen.GetValue(); },
           [](bool value) { Settings::values.swap_screen = value; });
    choice("Region", "The console region reported to the title.", false,
           {"Auto", "Japan", "USA", "Europe", "Australia", "China", "Korea", "Taiwan"},
           [] { return Settings::values.region_value.GetValue() + 1; },
           [](int value) { Settings::values.region_value = value - 1; });
}

bool Ui::BackPressed() const {
    // ImGui has already handled the press by the time this is asked, so a cancel that popped a
    // combo or left a child must not also close the overlay. It only counts when the overlay
    // itself held focus at the end of the previous frame.
    //
    // This version of ImGui has no key enum for a pad button; navigation arrives as the
    // NavInputs array, and a duration of exactly zero is the frame the press landed on.
    const ImGuiIO& io = ImGui::GetIO();
    const bool cancel_pressed = io.NavInputsDownDuration[ImGuiNavInput_Cancel] == 0.0f;
    return overlay_focused && cancel_pressed;
}

void Ui::EnterGame() {
    state = State::InGame;
    window.SetInputToGuest(true);
}

void Ui::LeaveGame() {
    state = State::GameList;
    overlay_focused = false;
}

void Ui::OpenPauseMenu() {
    state = State::Pause;
    window.SetInputToGuest(false);
}

void Ui::SetStatusMessage(std::string message) {
    status_message = std::move(message);
}

void Ui::ClearStatusMessage() {
    status_message.clear();
}

void Ui::DrawSetting(Setting& setting, std::size_t index) {
    ImGui::PushID(static_cast<int>(index));
    ImGui::TextUnformatted(setting.title);
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s", setting.description);

    switch (setting.kind) {
    case Setting::Kind::Toggle: {
        bool value = setting.get_bool();
        if (ImGui::Checkbox("##toggle", &value)) {
            setting.set_bool(value);
        }
        break;
    }
    case Setting::Kind::Choice: {
        int value = setting.get_choice();
        value = std::clamp(value, 0, static_cast<int>(setting.choices.size()) - 1);
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::BeginCombo("##choice", setting.choices[value])) {
            for (int i = 0; i < static_cast<int>(setting.choices.size()); i++) {
                if (ImGui::Selectable(setting.choices[i], i == value)) {
                    setting.set_choice(i);
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case Setting::Kind::Slider: {
        int value = setting.get_slider();
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::SliderInt("##slider", &value, setting.slider_min, setting.slider_max)) {
            setting.set_slider(value);
        }
        break;
    }
    }

    ImGui::Separator();
    ImGui::PopID();
}

void Ui::DrawPatchRow(std::size_t index) {
    constexpr float Pad = 18.0f;

    ImGui::PushID(static_cast<int>(index));
    const Loader::TitlePatchInfo& patch = settings_patches[index];
    // The running title's say whether each went in; a listed title's are built in.
    const char* state = "Built in";
    ImVec4 state_colour = palette->Text;
    if (!settings_patches_in.empty()) {
        const bool in = settings_patches_in[index];
        state = in ? "Applied" : "Left out";
        state_colour = in ? palette->Accent : palette->TextDim;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 state_size = ImGui::CalcTextSize(state);
    const float label_width = width - state_size.x - 3.0f * Pad;
    const ImVec2 label = ImGui::CalcTextSize(patch.name, nullptr, false, label_width);
    const Card card = CardItem("##patch", ImVec2(width, std::max(RowHeight, label.y + 28.0f)));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    if (ImGui::GetFocusID() == card.id) {
        focused_patch = static_cast<int>(index);
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float mid = card.bb.GetCenter().y;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text),
                    patch.name, nullptr, label_width);
        dl->AddText(ImVec2(card.bb.Max.x - Pad - state_size.x, mid - state_size.y * 0.5f),
                    Colour(state_colour), state);
    }
    ImGui::PopID();
}

void Ui::DrawPatchInfo() {
    if (engine_search.joinable()) {
        return;
    }
    if (settings_patches.empty() && !settings_engine.found) {
        ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
        ImGui::TextWrapped("%s", Tr("No patches"));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        // Code that is not the ExeFS's own (an override, a patch) is searched at the start.
        Note(palette->TextDim,
             settings_engine.scanned
                 ? Tr("Azahar has no patches or engine settings for this game.")
                 : Tr("Azahar has no patches for this game. Its engine settings, if it has any, "
                      "show here once it has started."));
        return;
    }
    if (focused_patch < 0) {
        return;
    }
    const auto index = static_cast<std::size_t>(focused_patch);
    const Loader::TitlePatchInfo& patch = settings_patches[index];

    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", patch.name);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", patch.description);
    ImGui::Spacing();

    if (settings_patches_in.empty()) {
        Note(palette->TextDim, "Applied when the game starts, if this copy is the version the "
                               "patch was made for.");
    } else if (settings_patches_in[index]) {
        Note(palette->Accent, "Applied to the running game.");
    } else {
        Note(palette->TextDim, "Left out: this copy of the game is a different version from the "
                               "one the patch was made for.");
    }
    Note(palette->TextDim, "Built into Azahar for this game. It can't be turned off.");
}

void Ui::DrawSettingsBody(bool runtime_only) {
    for (std::size_t i = 0; i < settings.size(); i++) {
        if (runtime_only && !settings[i].runtime) {
            continue;
        }
        DrawSetting(settings[i], i);
    }
}

Ui::Action Ui::DrawGameList() {
    Action action = Action::None;
    BeginFullscreen("##gamelist");

    ImGui::TextUnformatted("Azahar");
    ImGui::SameLine();
    // %zu would be a crash here, not a cosmetic problem: vitasdk's newlib is built without
    // _WANT_IO_C99_FORMATS, so it does not know the z modifier, prints it literally and consumes
    // no argument - and the %s that follows then reads the count instead of the string. With an
    // empty list that count is zero and the formatter walks into strlen(nullptr).
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "  %u titles in %s",
                       static_cast<unsigned>(games.size()), ListedDirectories);
    ImGui::Separator();

    if (games.empty()) {
        ImGui::TextWrapped("No titles found. Copy .3ds or .cci files into %s and choose Rescan.",
                           GameDirectory);
    }

    ImGui::BeginChild("##list", ImVec2(0.0f, 380.0f));
    for (int i = 0; i < static_cast<int>(games.size()); i++) {
        if (ImGui::Selectable(games[i].name.c_str(), i == selected_game)) {
            selected_game = i;
            selected_game_path = games[i].path;
            action = Action::LaunchGame;
        }
        if (ImGui::IsItemFocused()) {
            selected_game = i;
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Settings", ImVec2(200.0f, 48.0f))) {
        state = State::GameSettings;
        overlay_focused = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rescan", ImVec2(200.0f, 48.0f))) {
        ScanGames();
    }
    ImGui::SameLine();
    if (ImGui::Button("Exit", ImVec2(200.0f, 48.0f))) {
        action = Action::ExitApplication;
    }

    ImGui::End();
    return action;
}

Ui::Action Ui::DrawGameSettings() {
    BeginFullscreen("##gamesettings");
    ImGui::TextUnformatted("Settings");
    ImGui::Separator();

    ImGui::BeginChild("##settings_scroll", ImVec2(0.0f, 420.0f));
    DrawSettingsBody(false);
    ImGui::EndChild();

    ImGui::Separator();
    const bool back = ImGui::Button("Back", ImVec2(200.0f, 48.0f)) || BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (back) {
        state = State::GameList;
        overlay_focused = false;
    }
    return Action::None;
}

Ui::Action Ui::DrawPauseMenu() {
    Action action = Action::None;
    BeginCentered("##pause", 360.0f, 480.0f);
    ImGui::TextUnformatted("Paused");
    ImGui::Separator();

    const ImVec2 button_size{320.0f, 48.0f};
    if (ImGui::Button("Resume", button_size)) {
        action = Action::ResumeGame;
        window.UpdateCurrentFramebufferLayout();
    }
    if (ImGui::Button("Multiplayer", button_size)) {
        state = State::Multiplayer;
        room_fields = Multiplayer::CurrentFields();
        editing_field = -1;
        overlay_focused = false;
    }
    if (ImGui::Button("Settings", button_size)) {
        state = State::PauseSettings;
        overlay_focused = false;
    }
    if (ImGui::Button("Save states", button_size)) {
        state = State::Savestates;
        overlay_focused = false;
    }
#ifdef VITA_DIAGNOSTICS
    if (ImGui::Button("Dump frame", button_size)) {
        // The next presented frame, without the menu, to ux0:data/azahar/frame_NN.ppm.
        SgxdPresent::DumpNextFrame();
    }
#endif
    if (ImGui::Button("Quit game", button_size)) {
        action = Action::QuitGame;
    }
    if (ImGui::Button("Exit emulator", button_size)) {
        action = Action::ExitApplication;
    }

    if (BackPressed()) {
        action = Action::ResumeGame;
    }
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    return action;
}

Ui::Action Ui::DrawPauseSettings() {
    BeginCentered("##pausesettings", 700.0f, 460.0f);
    ImGui::TextUnformatted("Settings");
    ImGui::Separator();

    ImGui::BeginChild("##pause_settings_scroll", ImVec2(0.0f, 320.0f));
    // Only what can be changed under a running title; the rest would need a reboot.
    DrawSettingsBody(true);
    ImGui::EndChild();

    ImGui::Separator();
    const bool back = ImGui::Button("Back", ImVec2(200.0f, 48.0f)) || BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (back) {
        state = State::Pause;
        overlay_focused = false;
    }
    return Action::None;
}

Ui::Action Ui::DrawSavestates() {
    Action action = Action::None;
    BeginCentered("##savestates", 560.0f, 460.0f);
    ImGui::TextUnformatted("Save states");
    ImGui::Separator();

    ImGui::BeginChild("##slots", ImVec2(0.0f, 300.0f));
    for (u32 slot = 1; slot <= 9; slot++) {
        ImGui::PushID(static_cast<int>(slot));
        ImGui::Text("Slot %u", slot);
        ImGui::SameLine(140.0f);
        if (ImGui::Button("Save", ImVec2(140.0f, 40.0f))) {
            selected_slot = slot;
            action = Action::SaveState;
        }
        ImGui::SameLine();
        if (ImGui::Button("Load", ImVec2(140.0f, 40.0f))) {
            selected_slot = slot;
            action = Action::LoadState;
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::Separator();
    const bool back = ImGui::Button("Back", ImVec2(200.0f, 48.0f)) || BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (back) {
        state = State::Pause;
        overlay_focused = false;
    }
    return action;
}

Ui::Action Ui::DrawMultiplayer() {
    if (editing_field >= 0) {
        DrawKeyboard();
        return Action::None;
    }
    BeginCentered("##multiplayer", 720.0f, 500.0f);
    ImGui::TextUnformatted("Multiplayer");
    ImGui::Separator();
    ImGui::TextWrapped("%s", Multiplayer::Status().c_str());
    ImGui::Spacing();

    // Each field a button that opens the keyboard on it.
    const std::string port = std::to_string(room_fields.port);
    const std::string password(room_fields.password.size(), '*');
    const std::pair<const char*, const std::string*> rows[] = {
        {"Address", &room_fields.address},
        {"Port", &port},
        {"Nickname", &room_fields.nickname},
        {"Password", &password},
    };
    for (int i = 0; i < 4; i++) {
        ImGui::PushID(i);
        ImGui::Text("%s", rows[i].first);
        ImGui::SameLine(140.0f);
        const std::string label = rows[i].second->empty() ? "(none)" : *rows[i].second;
        if (ImGui::Button(label.c_str(), ImVec2(400.0f, 40.0f))) {
            editing_field = i;
            edit_text = i == 1 ? port : i == 3 ? room_fields.password : *rows[i].second;
            keyboard_shift = false;
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    const bool can_join = !room_fields.address.empty() && !room_fields.nickname.empty();
    if (ImGui::Button(Multiplayer::InRoom() ? "Join again" : "Join", ImVec2(200.0f, 48.0f)) &&
        can_join) {
        Multiplayer::Join(system, room_fields);
    }
    ImGui::SameLine();
    if (Multiplayer::InRoom() && ImGui::Button("Leave", ImVec2(200.0f, 48.0f))) {
        Multiplayer::Leave();
    }
    // Back on the same row: the panel's bottom is under the screen's edge otherwise.
    ImGui::SameLine(472.0f);
    const bool back = ImGui::Button("Back", ImVec2(200.0f, 48.0f)) || BackPressed();
    ImGui::Separator();
    ImGui::BeginChild("##members", ImVec2(0.0f, 0.0f));
    const std::vector<std::string> members = Multiplayer::Members();
    if (members.empty()) {
        ImGui::TextWrapped("Join a room hosted by a desktop Azahar (Multiplayer, Host Room) or its "
                           "room server. Every player runs the same game; its local wireless "
                           "play then finds the others.");
    }
    for (const std::string& member : members) {
        ImGui::TextUnformatted(member.c_str());
    }
    ImGui::EndChild();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    if (back) {
        state = State::Pause;
        overlay_focused = false;
    }
    return Action::None;
}

void Ui::DrawKeyboard() {
    static constexpr const char* rows[] = {"1234567890", "qwertyuiop", "asdfghjkl.",
                                           "zxcvbnm-_:"};
    static constexpr const char* titles[] = {"Address", "Port", "Nickname", "Password"};
    // Ten keys of 56 a row and their spacing.
    BeginCentered("##keyboard", 700.0f, 420.0f);
    ImGui::Text("%s", titles[editing_field]);
    ImGui::Separator();
    ImGui::Text("%s_", editing_field == 3 ? std::string(edit_text.size(), '*').c_str()
                                          : edit_text.c_str());
    ImGui::Spacing();
    const ImVec2 key{56.0f, 44.0f};
    for (const char* row : rows) {
        for (const char* c = row; *c != '\0'; c++) {
            if (c != row) {
                ImGui::SameLine();
            }
            const char ch = keyboard_shift && *c >= 'a' && *c <= 'z' ? *c - 'a' + 'A' : *c;
            const char label[2] = {ch, '\0'};
            // The port takes digits only.
            if (ImGui::Button(label, key) && (editing_field != 1 || (ch >= '0' && ch <= '9')) &&
                edit_text.size() < 64) {
                edit_text.push_back(ch);
            }
        }
    }
    bool done = false, cancel = false;
    if (ImGui::Button(keyboard_shift ? "SHIFT" : "Shift", ImVec2(110.0f, 44.0f))) {
        keyboard_shift = !keyboard_shift;
    }
    ImGui::SameLine();
    if (ImGui::Button("Space", ImVec2(110.0f, 44.0f)) && editing_field != 1) {
        edit_text.push_back(' ');
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete", ImVec2(110.0f, 44.0f)) && !edit_text.empty()) {
        edit_text.pop_back();
    }
    ImGui::SameLine();
    cancel = ImGui::Button("Cancel", ImVec2(110.0f, 44.0f)) || BackPressed();
    ImGui::SameLine();
    done = ImGui::Button("OK", ImVec2(110.0f, 44.0f));
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    if (done) {
        switch (editing_field) {
        case 0:
            room_fields.address = edit_text;
            break;
        case 1:
            if (const int port = std::atoi(edit_text.c_str()); port > 0 && port <= 65535) {
                room_fields.port = static_cast<u16>(port);
            }
            break;
        case 2:
            room_fields.nickname = edit_text;
            break;
        default:
            room_fields.password = edit_text;
            break;
        }
    }
    if (done || cancel) {
        editing_field = -1;
        overlay_focused = false;
    }
}

void Ui::DrawStatusMessage() {
    BeginCentered("##status", 560.0f, 140.0f);
    ImGui::TextWrapped("%s", status_message.c_str());
    ImGui::End();
}

Ui::Action Ui::Update() {
    // In game: the stats overlay, which needs no ImGui frame (Draw).
    imgui_frame = !status_message.empty() || state != State::InGame;
    if (!imgui_frame) {
        return Action::None;
    }
    ImGui_ImplVita_NewFrame();

    Action action = Action::None;
    if (!status_message.empty()) {
        DrawStatusMessage();
    } else {
        switch (state) {
        case State::GameList:
            action = DrawGameList();
            break;
        case State::GameSettings:
            action = DrawGameSettings();
            break;
        case State::InGame:
            break;
        case State::Pause:
            action = DrawPauseMenu();
            break;
        case State::PauseSettings:
            action = DrawPauseSettings();
            break;
        case State::Savestates:
            action = DrawSavestates();
            break;
        case State::Multiplayer:
            action = DrawMultiplayer();
            break;
        }
    }

    if (action == Action::ResumeGame) {
        EnterGame();
    } else if (action == Action::QuitGame) {
        state = State::GameList;
        overlay_focused = false;
    }

    if (state != State::InGame) {
        DrawVolume();
    }
    ImGui::Render();
    return action;
}

void Ui::Draw() {
    if (!imgui_frame) {
        StatsHud::Draw();
        return;
    }
    ImGui_ImplSgxd_RenderDrawData(ImGui::GetDrawData());
}

} // namespace VitaFrontend

// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include "common/vita_data_dir.h"
#include <cstring>

#include <imgui_vita2d/imgui.h>
#include <imgui_vita2d/imgui_internal.h>

#include "citra_vita/imgui_impl_gxm.h"

#include "citra_vita/emu_window_vita.h"
#include "citra_vita/gxm_present.h"
#include "citra_vita/hud_stats.h"
#include "citra_vita/vita_ui.h"
#include "video_core/renderer_gxm/gxm_video_options.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/renderer_gxm/gxm_flags.h"

namespace VitaFrontend {

namespace {
constexpr float ScreenWidth = 960.0f;
constexpr float ScreenHeight = 544.0f;

/// Where titles are looked for. One directory, no recursion: a console file manager is enough
/// work to use that a deep tree is worse than a flat one.
constexpr char GameDirectory[] = VITA_DATA_DIR "/roms";

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

/// A centred panel exactly as tall as its contents (the pause menu: no empty space under the
/// last button).
bool BeginCenteredAuto(const char* id, float width) {
    ImGui::SetNextWindowPos(ImVec2(ScreenWidth * 0.5f, ScreenHeight * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, ScreenHeight));
    const bool open = ImGui::Begin(id, nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_AlwaysAutoResize);
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        ImGui::SetWindowFocus();
    }
    return open;
}

/// A centred panel over whatever is behind it, for the pause menu.
bool BeginCentered(const char* id, float width, float height) {
    ImGui::SetNextWindowPos(ImVec2((ScreenWidth - width) * 0.5f, (ScreenHeight - height) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    const bool open = ImGui::Begin(id, nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    // A tap outside the panel clears ImGui's window focus, and the buttons then ignore the pad
    // until the panel is touched again. The panel is the only thing on screen to talk to -
    // except an open dropdown, which is a popup window of its own: taking the focus back from
    // it closed every combo the moment it opened (the settings' choices could not be changed).
    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopup)) {
        ImGui::SetWindowFocus();
    }
    return open;
}
/// Dark wood, brass edges and parchment text: the look of the series' own menus, in colours only
/// (no artwork from the games).
void ApplyHunterTheme(ImGuiStyle& style) {
    ImVec4* c = style.Colors;
    const ImVec4 parchment{0.96f, 0.91f, 0.80f, 1.0f};
    const ImVec4 brass{0.78f, 0.60f, 0.28f, 1.0f};
    const ImVec4 gold{0.98f, 0.82f, 0.38f, 1.0f};
    c[ImGuiCol_Text] = parchment;
    c[ImGuiCol_TextDisabled] = ImVec4(0.66f, 0.58f, 0.46f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.15f, 0.10f, 0.06f, 0.96f);
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = ImVec4(0.20f, 0.13f, 0.08f, 0.98f);
    c[ImGuiCol_Border] = brass;
    c[ImGuiCol_BorderShadow] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.27f, 0.18f, 0.11f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.38f, 0.26f, 0.15f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.46f, 0.32f, 0.17f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.33f, 0.22f, 0.12f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.50f, 0.34f, 0.17f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.62f, 0.43f, 0.19f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.50f, 0.34f, 0.17f, 1.0f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.58f, 0.40f, 0.19f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.66f, 0.46f, 0.21f, 1.0f);
    c[ImGuiCol_CheckMark] = gold;
    c[ImGuiCol_SliderGrab] = brass;
    c[ImGuiCol_SliderGrabActive] = gold;
    c[ImGuiCol_Separator] = ImVec4(0.52f, 0.39f, 0.20f, 1.0f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.10f, 0.07f, 0.04f, 0.8f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.52f, 0.39f, 0.20f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = brass;
    c[ImGuiCol_ScrollbarGrabActive] = gold;
    c[ImGuiCol_NavHighlight] = gold;
}

/// A panel heading in gold.
void Heading(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.82f, 0.38f, 1.0f));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

/// Makes the item just drawn the pad's target, as a D-pad press would. On a fresh window the
/// old focus call only drew the highlight: X did nothing until a D-pad press or a touch set the
/// navigation target for real. SetFocusID sets it, in the window navigation runs in (a
/// NavFlattened child navigates in its parent).
void FocusLastItem() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImGuiWindow* nav_window = window->RootWindowForNav != nullptr ? window->RootWindowForNav : window;
    ImGui::FocusWindow(nav_window);
    ImGui::SetFocusID(ImGui::GetItemID(), nav_window);
    g.NavDisableHighlight = false;
    g.NavDisableMouseHover = true;
}
} // Anonymous namespace

Ui::Ui(Core::System& system_, EmuWindowVita& window_) : system{system_}, window{window_} {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.WindowPadding = ImVec2(16.0f, 14.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.WindowBorderSize = 2.0f;
    style.FrameBorderSize = 1.0f;
    style.GrabMinSize = 18.0f;
    style.ScrollbarSize = 14.0f;
    ApplyHunterTheme(style);
    // The console is held at arm's length, so everything is a size up from a desktop default.
    ImGui::GetIO().FontGlobalScale = 1.2f;

    ImGui_ImplGxm_Init();
    // The backend has uploaded the font atlas to a GXM texture by now, and the pixels it was
    // built from are 4 MB of heap nothing reads again. ImGui rebuilds them on demand if anything
    // ever does.
    ImGui::GetIO().Fonts->ClearTexData();
    ImGui_ImplGxm_TouchUsage(true);
    ImGui_ImplGxm_GamepadUsage(true);
    // No right-stick cursor: it is never drawn, every item is reachable by pad or touch, and a
    // stick resting just past its dead zone moved it - ImGui then took the mouse as in use and
    // hid the pad highlight a few seconds after each menu opened (focus stayed, 2026-11-20).
    ImGui_ImplGxm_MouseStickUsage(false);

    // The launch values (settings that apply on the next start) are fixed here, before any
    // menu can change them.
    (void)GxmRenderer::VideoAtLaunch();
    BuildSettings();
    ScanGames();
}

Ui::~Ui() {
    ImGui_ImplGxm_Shutdown();
    ImGui::DestroyContext();
}

void Ui::ScanGames() {
    games.clear();
    FileUtil::FSTEntry root;
    if (!FileUtil::ScanDirectoryTree(GameDirectory, root)) {
        LOG_WARNING(Frontend, "No game directory at {}", GameDirectory);
        return;
    }
    for (const auto& entry : root.children) {
        if (entry.isDirectory || !HasRomExtension(entry.virtualName)) {
            continue;
        }
        games.push_back({entry.physicalName, entry.virtualName});
    }
    std::sort(games.begin(), games.end(),
              [](const GameEntry& a, const GameEntry& b) { return a.name < b.name; });
    LOG_INFO(Frontend, "Found {} titles in {}", games.size(), GameDirectory);
}

void Ui::BuildSettings() {
    // Each row lands in the category set last (the tabs at the top of the settings).
    int category = CategorySystem;
    const auto toggle = [&](const char* title, const char* description, bool runtime,
                            std::function<bool()> get, std::function<void(bool)> set) {
        Setting setting{};
        setting.title = title;
        setting.description = description;
        setting.kind = Setting::Kind::Toggle;
        setting.category = category;
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
        setting.category = category;
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
        setting.category = category;
        setting.runtime = runtime;
        setting.slider_min = min;
        setting.slider_max = max;
        setting.get_slider = std::move(get);
        setting.set_slider = std::move(set);
        settings.push_back(std::move(setting));
    };

#ifdef VITA_DIAGNOSTICS
    // Diagnostics only: the release runs MH4U at full speed on one screen (2026-11-20).
    category = CategorySystem;
    slider("Speed limit", "Percentage of full speed to aim for. Lower it to save battery.", true,
           0, 200, [] { return static_cast<int>(Settings::values.frame_limit.GetValue()); },
           [](int value) { Settings::values.frame_limit = static_cast<double>(value); });
#endif
    category = CategoryAudio;
    slider("Volume", "Output volume.", true, 0, 100,
           [] { return static_cast<int>(Settings::values.volume.GetValue() * 100.0f); },
           [](int value) { Settings::values.volume = static_cast<float>(value) / 100.0f; });
    toggle("Stretch audio", "Keeps audio continuous when the emulator runs below full speed.",
           true, [] { return Settings::values.enable_audio_stretching.GetValue(); },
           [](bool value) { Settings::values.enable_audio_stretching = value; });
    category = CategoryDisplay;
#ifdef VITA_DIAGNOSTICS
    choice("Screen layout", "How the two screens are arranged on the display.", true,
           {"Default", "Single screen", "Large screen", "Side by side", "Hybrid"},
           // The list's order, not the enum's: SeparateWindows sits between SideScreen and
           // HybridScreen in the enum and is no layout here.
           [] {
               using L = Settings::LayoutOption;
               switch (Settings::values.layout_option.GetValue()) {
               case L::SingleScreen:
                   return 1;
               case L::LargeScreen:
                   return 2;
               case L::SideScreen:
                   return 3;
               case L::HybridScreen:
                   return 4;
               default:
                   return 0;
               }
           },
           [](int value) {
               using L = Settings::LayoutOption;
               static constexpr L order[] = {L::Default, L::SingleScreen, L::LargeScreen,
                                             L::SideScreen, L::HybridScreen};
               Settings::values.layout_option = order[value < 0 || value > 4 ? 0 : value];
           });
    toggle("Swap screens", "Puts the touch screen where the top screen would be.", true,
           [] { return Settings::values.swap_screen.GetValue(); },
           [](bool value) { Settings::values.swap_screen = value; });
#endif
    choice("Upscale filter",
           "How the 400x240 picture is stretched to the screen. Sharp keeps pixels crisp, "
           "blending only at their edges. With HUD resolution on it applies to the 3D picture "
           "under the HUD.",
           true, {"Linear", "Sharp bilinear"},
           [] { return static_cast<int>(GxmRenderer::Video().upscale); },
           [](int value) {
               GxmRenderer::Video().upscale =
                   static_cast<GxmRenderer::VideoOptions::Upscale>(value < 0 || value > 1 ? 0 : value);
               GxmRenderer::SaveVideoOptions();
           });
    // With the hi-res HUD on, the filter applies on the next start (it jiggled when switched live).
    settings.back().restart = GxmRenderer::FilterLocked();
    toggle("FXAA", "Smooths jagged edges of the game's picture (top screen) before it is "
           "stretched. With HUD resolution on only the 3D picture is smoothed, not the HUD. "
           "Costs about 1-2 ms of GPU a frame.",
           true, [] { return GxmRenderer::Video().fxaa; },
           [](bool value) {
               GxmRenderer::Video().fxaa = value;
               GxmRenderer::SaveVideoOptions();
           });
    settings.back().restart = GxmRenderer::FilterLocked();
    toggle("Smooth gradients",
           "Turns off the game's dither pattern and keeps the picture in full colour, for smooth "
           "skies and fades without the fine grid or banding. With the Heavenfall mod, choose "
           "No Dithering in its Mod Settings as well.",
           true, [] { return GxmRenderer::Video().smooth; },
           [](bool value) {
               GxmRenderer::Video().smooth = value;
               GxmRenderer::SaveVideoOptions();
           });
    category = CategoryHud;
    choice("HUD resolution",
           "Draws the game's HUD, text and menus sharper than the 3DS's 400x240, over the "
           "stretched 3D picture (applies on the next launch). Higher steps are sharper and "
           "cost more GPU time (1.5x holds 30 fps in most places).",
           true,
#ifdef VITA_DIAGNOSTICS
           {"3DS (400x240)", "1.25x", "1.33x", "1.4x", "1.45x", "1.5x", "Screen"},
#else
           // The full "Screen" setting costs too much GPU time to play with (a saved hudres=1
           // still works, shown as 1.5x).
           {"3DS (400x240)", "1.25x", "1.33x", "1.4x", "1.45x", "1.5x"},
#endif
           // List order against the stored value: 0 off, 3..6 = 1.25x..1.45x, 2 = 1.5x,
           // 1 = the screen (diagnostics only).
           [] {
               switch (GxmRenderer::Video().hudres) {
               case 0: return 0;
               case 3: return 1;
               case 4: return 2;
               case 5: return 3;
               case 6: return 4;
#ifdef VITA_DIAGNOSTICS
               case 1: return 6;
#endif
               default: return 5;
               }
           },
           [](int value) {
               static constexpr u32 stored[] = {0, 3, 4, 5, 6, 2, 1};
               GxmRenderer::Video().hudres = stored[value < 0 || value > 6 ? 0 : value];
               GxmRenderer::SaveVideoOptions();
           });
    settings.back().restart = true;
    toggle("FPS counter", "Shows the emulation speed and the frame rate in the top right corner.",
           true, [] { return GxmRenderer::Video().show_fps; },
           [](bool value) {
               GxmRenderer::Video().show_fps = value;
               GxmRenderer::SaveVideoOptions();
           });
    category = CategoryDisplay;
    choice("Picture shape",
           "3DS keeps the game's 5:3 picture with bars at the sides. Wide fills the screen; "
           "use it with the Heavenfall mod built with --cheat-aspect-ratio 16:9 (and 16:9 "
           "chosen in its Mod Settings), or the picture looks stretched.",
           true, {"3DS (5:3)", "Wide (16:9)"},
           [] { return GxmRenderer::Video().wide ? 1 : 0; },
           [](int value) {
               GxmRenderer::Video().wide = value == 1;
               Settings::values.aspect_ratio = value == 1 ? Settings::AspectRatio::Stretch
                                                          : Settings::AspectRatio::Default;
               GxmRenderer::SaveVideoOptions();
           });
    choice("Render scale",
           "The game's internal resolution against the 3DS's 400x240 (applies on the next "
           "launch). 0.75x is softer and faster; each step above 1x costs a lot of GPU time.",
           true, {"0.75x", "1x", "1.25x", "1.5x", "1.75x", "2x"},
           [] { return static_cast<int>(GxmRenderer::Video().scale_q) - 3; },
           [](int value) {
               GxmRenderer::Video().scale_q = static_cast<u32>(3 + (value < 0 || value > 5 ? 1 : value));
               GxmRenderer::SaveVideoOptions();
           });
    settings.back().restart = true;
    category = CategorySystem;
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

void Ui::DrawSetting(Setting& setting, std::size_t index, bool focus) {
    ImGui::PushID(static_cast<int>(index));
    // Where the row starts: a focused control brings its title and description into view too
    // (ImGui scrolls only far enough for the control, so going back up stopped one row short
    // and the first title never came back).
    const float row_top = ImGui::GetCursorPosY();
    const float control_x = ImGui::GetWindowContentRegionMax().x - ControlWidth;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(setting.title);
    ImGui::SameLine(control_x);
    ImGui::SetNextItemWidth(ControlWidth);

    switch (setting.kind) {
    case Setting::Kind::Toggle: {
        bool value = setting.get_bool();
        if (ImGui::Checkbox(value ? "On##toggle" : "Off##toggle", &value)) {
            setting.set_bool(value);
        }
        break;
    }
    case Setting::Kind::Choice: {
        int value = setting.get_choice();
        value = std::clamp(value, 0, static_cast<int>(setting.choices.size()) - 1);
        if (ImGui::BeginCombo("##choice", setting.choices[value])) {
            for (int i = 0; i < static_cast<int>(setting.choices.size()); i++) {
                if (ImGui::Selectable(setting.choices[i], i == value)) {
                    setting.set_choice(i);
                }
                // The list opens on the current choice, not the first one.
                if (i == value) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case Setting::Kind::Slider: {
        int value = setting.get_slider();
        if (ImGui::SliderInt("##slider", &value, setting.slider_min, setting.slider_max)) {
            setting.set_slider(value);
        }
        break;
    }
    }
    if (focus) {
        FocusLastItem();
    }
    if (ImGui::IsItemFocused() && row_top < ImGui::GetScrollY()) {
        ImGui::SetScrollY(row_top);
    }

    // The description a size smaller, in a muted parchment.
    ImGui::SetWindowFontScale(0.85f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.80f, 0.72f, 0.58f, 1.0f));
    ImGui::TextWrapped("%s", setting.description);
    ImGui::PopStyleColor();
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::PopID();
}

bool Ui::CategoryVisible(int category, bool runtime_only) const {
    for (const Setting& setting : settings) {
        if (setting.category == category && (!runtime_only || setting.runtime)) {
            return true;
        }
    }
    return false;
}

void Ui::DrawCategoryTabs(bool runtime_only) {
    static constexpr const char* names[CategoryCount] = {"Display", "HUD", "Audio", "System"};
    int visible[CategoryCount];
    int count = 0;
    for (int c = 0; c < CategoryCount; c++) {
        if (CategoryVisible(c, runtime_only)) {
            visible[count++] = c;
        }
    }
    if (count == 0) {
        return;
    }
    int pos = 0;
    for (int k = 0; k < count; k++) {
        if (visible[k] == settings_category) {
            pos = k;
        }
    }
    settings_category = visible[pos];
    // L and R step through the tabs, as the games' own menus page with the shoulder buttons.
    const ImGuiIO& io = ImGui::GetIO();
    int step = 0;
    if (io.NavInputsDownDuration[ImGuiNavInput_FocusPrev] == 0.0f) {
        step = -1;
    } else if (io.NavInputsDownDuration[ImGuiNavInput_FocusNext] == 0.0f) {
        step = 1;
    }
    ImGui::TextDisabled("L");
    ImGui::SameLine();
    for (int k = 0; k < count; k++) {
        const bool current = k == pos;
        ImGui::PushStyleColor(ImGuiCol_Button, current ? ImVec4(0.66f, 0.46f, 0.21f, 1.0f)
                                                       : ImVec4(0.24f, 0.16f, 0.09f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, current ? ImVec4(1.0f, 0.95f, 0.82f, 1.0f)
                                                     : ImVec4(0.75f, 0.66f, 0.52f, 1.0f));
        // Tabs are for touch; the pad pages with L/R and stays in the list.
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        if (ImGui::Button(names[visible[k]], ImVec2(150.0f, 36.0f)) && !current) {
            step = k - pos;
        }
        ImGui::PopItemFlag();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }
    ImGui::TextDisabled("R");
    if (step != 0) {
        pos = (pos + step + count) % count;
        settings_category = visible[pos];
        focus_first_setting = true;
        focus_delay_frames = 1;
        settings_scroll_reset = true;
    }
}

void Ui::DrawSettingsBody(bool runtime_only) {
    if (settings_scroll_reset) {
        ImGui::SetScrollY(0.0f);
        settings_scroll_reset = false;
    }
    // The first row takes the focus one frame after the list (re)appears: on the frame the
    // scrolling child is created ImGui's own navigation start overrode the request, the tab
    // looked selected and X did nothing until a D-pad press.
    bool focus_now = false;
    if (focus_first_setting) {
        if (focus_delay_frames > 0) {
            focus_delay_frames--;
        } else {
            focus_now = true;
            focus_first_setting = false;
        }
    }
    for (std::size_t i = 0; i < settings.size(); i++) {
        if ((runtime_only && !settings[i].runtime) || settings[i].category != settings_category) {
            continue;
        }
        DrawSetting(settings[i], i, focus_now);
        focus_now = false;
    }
}

int Ui::SettingValue(const Setting& setting) {
    switch (setting.kind) {
    case Setting::Kind::Toggle:
        return setting.get_bool() ? 1 : 0;
    case Setting::Kind::Choice:
        return setting.get_choice();
    default:
        return setting.get_slider();
    }
}

void Ui::OpenSettings(State settings_state) {
    state = settings_state;
    overlay_focused = false;
    focus_first_setting = true;
    focus_delay_frames = 1;
    settings_scroll_reset = true;
    // What the restart-only settings held on the way in, to ask on the way out.
    restart_snapshot.clear();
    for (const Setting& setting : settings) {
        restart_snapshot.push_back(setting.restart ? SettingValue(setting) : 0);
    }
}

bool Ui::RestartSettingChanged() const {
    for (std::size_t i = 0; i < settings.size() && i < restart_snapshot.size(); i++) {
        if (settings[i].restart && SettingValue(settings[i]) != restart_snapshot[i]) {
            return true;
        }
    }
    return false;
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
                       static_cast<unsigned>(games.size()), GameDirectory);
    ImGui::Separator();

    if (games.empty()) {
        ImGui::TextWrapped("No titles found. Copy .3ds or .cci files into %s and choose Rescan.",
                           GameDirectory);
    }

    const bool appearing = ImGui::IsWindowAppearing();
    ImGui::BeginChild("##list", ImVec2(0.0f, 380.0f));
    for (int i = 0; i < static_cast<int>(games.size()); i++) {
        if (ImGui::Selectable(games[i].name.c_str(), i == selected_game)) {
            selected_game = i;
            selected_game_path = games[i].path;
            action = Action::LaunchGame;
        }
        if (appearing && i == 0) {
            FocusLastItem();
        }
        if (ImGui::IsItemFocused()) {
            selected_game = i;
        }
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Settings", ImVec2(200.0f, 48.0f))) {
        OpenSettings(State::GameSettings);
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
    Heading("Settings");
    DrawCategoryTabs(false);
    ImGui::Separator();

    ImGui::BeginChild("##settings_scroll", ImVec2(0.0f, 380.0f), false, ImGuiWindowFlags_NavFlattened);
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
    BeginCenteredAuto("##pause", 360.0f);
    Heading("Paused");
    ImGui::Separator();

    const ImVec2 button_size{320.0f, 48.0f};
    // Every opening starts on Resume: the menu otherwise remembers the last button, and the
    // same presses land somewhere else each time.
    const bool appearing = ImGui::IsWindowAppearing() || focus_pause_resume;
    focus_pause_resume = false;
    if (ImGui::Button("Resume", button_size)) {
        action = Action::ResumeGame;
        window.UpdateCurrentFramebufferLayout();
    }
    if (appearing) {
        FocusLastItem();
    }
    if (ImGui::Button("Settings", button_size)) {
        OpenSettings(State::PauseSettings);
    }
    if (ImGui::Button("Save states", button_size)) {
        state = State::Savestates;
        overlay_focused = false;
    }
    if (ImGui::Button("Bug report", button_size)) {
        state = State::BugReport;
        report_message.clear();
        overlay_focused = false;
    }
#ifdef VITA_DIAGNOSTICS
    if (ImGui::Button("Dump frame", button_size)) {
        // The next presented frame, screens only, to ux0:data/mh4u/frame_NN.ppm, and every
        // surface the cache holds at that frame's end to ux0:data/mh4u/dump/.
        GxmPresent::DumpNextFrame();
        GxmRenderer::RequestSurfaceDump();
    }
#endif
    // One game: leaving it ends the app, back to the LiveArea.
    if (ImGui::Button("Exit game", button_size)) {
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
    BeginCentered("##pausesettings", 880.0f, 500.0f);
    Heading("Settings");
    DrawCategoryTabs(true);
    ImGui::Separator();

    ImGui::BeginChild("##pause_settings_scroll", ImVec2(0.0f, 330.0f), false, ImGuiWindowFlags_NavFlattened);
    // Only what can be changed under a running title; the rest would need a reboot.
    DrawSettingsBody(true);
    ImGui::EndChild();

    ImGui::Separator();
    const bool back = ImGui::Button("Back", ImVec2(200.0f, 40.0f)) || BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (back) {
        state = RestartSettingChanged() ? State::RestartPrompt : State::Pause;
        overlay_focused = false;
        focus_pause_resume = state == State::Pause;
    }
    return Action::None;
}

Ui::Action Ui::DrawRestartPrompt() {
    Action action = Action::None;
    BeginCenteredAuto("##restart", 600.0f);
    const bool appearing = ImGui::IsWindowAppearing();
    Heading("Restart needed");
    ImGui::Separator();
    ImGui::TextWrapped("A setting you changed takes effect after a restart (render scale, HUD "
                       "resolution, and with the sharp HUD on also the upscale filter and FXAA). "
                       "Restart now? Progress since your last in-game save (and since your "
                       "last save state) will be lost.");
    ImGui::Spacing();
    const ImVec2 button_size{560.0f, 44.0f};
    if (ImGui::Button("Later (keep playing)", button_size)) {
        state = State::Pause;
        focus_pause_resume = true;
    }
    // The safe choice is the default.
    if (appearing) {
        FocusLastItem();
    }
    if (ImGui::Button("Restart now", button_size)) {
        action = Action::RestartApplication;
    }
    if (BackPressed()) {
        state = State::Pause;
        focus_pause_resume = true;
    }
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    return action;
}

Ui::Action Ui::DrawSavestates() {
    Action action = Action::None;
    BeginCentered("##savestates", 560.0f, 528.0f);
    // Opening lands on slot 1's Load (Load comes first, so the default can't overwrite a slot).
    // No scrolling child: ImGui 1.79's pad navigation into one starts on its frame or on Back.
    const bool appearing = ImGui::IsWindowAppearing();
    Heading("Save states");
    ImGui::Separator();

    for (u32 slot = 1; slot <= 9; slot++) {
        ImGui::PushID(static_cast<int>(slot));
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Slot %u", slot);
        ImGui::SameLine(140.0f);
        if (ImGui::Button("Load", ImVec2(140.0f, 34.0f))) {
            selected_slot = slot;
            action = Action::LoadState;
        }
        if (appearing && slot == 1) {
            FocusLastItem();
        }
        ImGui::SameLine();
        if (ImGui::Button("Save", ImVec2(140.0f, 34.0f))) {
            selected_slot = slot;
            action = Action::SaveState;
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    const bool back = ImGui::Button("Back", ImVec2(200.0f, 40.0f)) || BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (back) {
        state = State::Pause;
        overlay_focused = false;
        focus_pause_resume = true;
    }
    return action;
}

Ui::Action Ui::DrawBugReport() {
    Action action = Action::None;
    if (report_pending) {
        // The frame before this one showed "Saving...": now the frontend writes it (a few
        // seconds, during which the screen holds that frame).
        report_pending = false;
        action = Action::SaveReport;
    }
    BeginCentered("##bugreport", 640.0f, 470.0f);
    const bool appearing = ImGui::IsWindowAppearing();
    Heading("Bug report");
    ImGui::Separator();
    ImGui::TextWrapped("Saves one zip to ux0:data/mh4u/reports (the newest 3 are kept) with what we need to look into a "
                       "problem: the app and game versions, your mod files list and settings, the "
                       "speed of the last two minutes, your in-game save (it shows your hunter's "
                       "name and guild cards) and the newest crash dump. Make it right after the "
                       "problem happens.");
    ImGui::Spacing();
    const ImVec2 button_size{600.0f, 44.0f};
    if (ImGui::Button("Save report", button_size) && !report_pending) {
        report_with_state = false;
        report_pending = true;
    }
    if (appearing) {
        FocusLastItem();
    }
    if (ImGui::Button("Save report with the newest save state (+30 MB)", button_size) &&
        !report_pending) {
        report_with_state = true;
        report_pending = true;
    }
    if (report_pending) {
        report_message = "Saving the report...";
    }
    if (!report_message.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", report_message.c_str());
    }
    ImGui::Separator();
    const bool back = ImGui::Button("Back", ImVec2(200.0f, 40.0f)) || BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (back && !report_pending && action == Action::None) {
        state = State::Pause;
        overlay_focused = false;
        focus_pause_resume = true;
    }
    return action;
}

void Ui::DrawStatusMessage() {
    BeginCentered("##status", 560.0f, 140.0f);
    ImGui::TextWrapped("%s", status_message.c_str());
    ImGui::End();
}

void Ui::DrawGpuHud() {
    static const bool on = GxmRenderer::GxmFlag("gpuhud");
    if (!on) {
        return;
    }
    const std::string& text = GxmPresent::GpuLiveText();
    ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##gpuhud", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextUnformatted(text.empty() ? "GPU live: waiting for the first second" : text.c_str());
    ImGui::End();
}

void Ui::DrawStatsHud() {
    // Top right, every frame while a title runs: what the console line prints once a second.
    // A dozen text lines of ImGui, so a few hundred vertices; `nostats` hides it.
    static const bool off = GxmRenderer::GxmFlag("nostats");
    if (off || !GxmRenderer::Video().show_fps ||
        !HudStats::valid.load(std::memory_order_acquire)) {
        return;
    }
    using namespace HudStats;
    const u32 shown = shown_fps.load(std::memory_order_relaxed);
    const u32 fps10 = game_fps10.load(std::memory_order_relaxed);
    ImGui::SetNextWindowPos(ImVec2(960.0f - 8.0f, 8.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##stats", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    // Three bare lines: speed/game fps, frame time/shown fps, guest core 0/core 1 busy.
    ImGui::Text("%u%%/%u", speed_percent.load(std::memory_order_relaxed), (fps10 + 5) / 10);
    ImGui::Text("%.1fms/%ufps", shown != 0 ? 1000.0f / static_cast<float>(shown) : 0.0f, shown);
    ImGui::End();
}

namespace {
/// ImGui 1.79 applies a pad/stick navigation move a frame after it was requested, and asserts
/// (`g.NavWindow && result->Window`, imgui.cpp:8873) that the window it moved in still exists.
/// The menus swap whole windows between frames (pause -> save states -> the "Saving..."
/// status), so a direction held while a screen changed ended the app from the render thread
/// (core dump 2026-11-18). A move whose window is gone is cancelled before the frame starts.
void CancelStaleNavMove() {
    ImGuiContext* ctx = ImGui::GetCurrentContext();
    if (ctx == nullptr || !ctx->NavMoveRequest) {
        return;
    }
    const auto gone = [](const ImGuiWindow* w) { return w == nullptr || !w->Active; };
    const bool local = ctx->NavMoveResultLocal.ID != 0;
    const bool other = ctx->NavMoveResultOther.ID != 0;
    if (gone(ctx->NavWindow) || (local && gone(ctx->NavMoveResultLocal.Window)) ||
        (other && gone(ctx->NavMoveResultOther.Window))) {
        ImGui::NavMoveRequestCancel();
        ctx->NavMoveResultLocal.Clear();
        ctx->NavMoveResultOther.Clear();
    }
}
} // Anonymous namespace

Ui::Action Ui::Update() {
    CancelStaleNavMove();
    ImGui_ImplGxm_NewFrame();

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
            DrawGpuHud();
            DrawStatsHud();
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
        case State::BugReport:
            action = DrawBugReport();
            break;
        case State::RestartPrompt:
            action = DrawRestartPrompt();
            break;
        }
    }

    if (action == Action::ResumeGame) {
        EnterGame();
    } else if (action == Action::QuitGame) {
        state = State::GameList;
        overlay_focused = false;
    }

    ImGui::Render();
    return action;
}

void Ui::Draw() {
    ImGui_ImplGxm_RenderDrawData(ImGui::GetDrawData());
}

} // namespace VitaFrontend

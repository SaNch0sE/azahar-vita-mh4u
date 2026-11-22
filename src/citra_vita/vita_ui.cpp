// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui_vita2d/imgui.h>
#include <imgui_vita2d/imgui_internal.h>
#include <psp2/ctrl.h>
#include <psp2/io/devctl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/touch.h>

#include "citra_vita/gxm_present.h"
#include "citra_vita/imgui_impl_gxm.h"
#include "citra_vita/imgui_impl_sgxd.h"
#include "citra_vita/imgui_impl_vita.h"
#include "citra_vita/livearea.h"

#include <fmt/format.h>

#include "citra_vita/config.h"
#include "citra_vita/crash_report.h"
#include "citra_vita/emu_window_vita.h"
#include "citra_vita/hud_stats.h"
#include "citra_vita/sgxd_present.h"
#include "citra_vita/stats_hud.h"
#include "citra_vita/vita_ui.h"
#include "common/assert.h"
#include "common/file_derived.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/file_sys/cia_container.h"
#include "core/file_sys/title_metadata.h"
#include "core/hle/service/am/am.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"
#include "core/movie.h"
#include "core/savestate.h"

namespace VitaFrontend {

namespace {
constexpr float ScreenWidth = 960.0f;
constexpr float ScreenHeight = 544.0f;

/// Where titles are looked for. One directory, no recursion: a console file manager is enough
/// host0 does not open and adds nothing.
constexpr char GameDirectory[] = "ux0:/data/azahar/roms";
#ifdef VITA_DIAGNOSTICS
constexpr char HostGameDirectory[] = "host0:roms";
#endif

/// Names in the order a person reads them: a run of digits by its number (clip_test_2 before
/// clip_test_10), letters without their case.
bool NaturalLess(const std::string& a, const std::string& b) {
    const auto digit = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (digit(a[i]) && digit(b[j])) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && digit(a[ie])) {
                ie++;
            }
            while (je < b.size() && digit(b[je])) {
                je++;
            }
            while (i + 1 < ie && a[i] == '0') {
                i++;
            }
            while (j + 1 < je && b[j] == '0') {
                j++;
            }
            if (ie - i != je - j) {
                return ie - i < je - j;
            }
            if (const int c = a.compare(i, ie - i, b, j, je - j); c != 0) {
                return c < 0;
            }
            i = ie;
            j = je;
            continue;
        }
        const int la = std::tolower(static_cast<unsigned char>(a[i]));
        const int lb = std::tolower(static_cast<unsigned char>(b[j]));
        if (la != lb) {
            return la < lb;
        }
        i++;
        j++;
    }
    return a.size() - i < b.size() - j;
}

/// A savestate's time (seconds from 1970, UTC) as the console's local date and time.
std::string LocalTimeText(u64 seconds) {
    SceDateTime utc{};
    sceRtcSetTime64_t(&utc, seconds);
    SceRtcTick utc_tick{}, local_tick{};
    sceRtcGetTick(&utc, &utc_tick);
    sceRtcConvertUtcToLocalTime(&utc_tick, &local_tick);
    SceDateTime local{};
    sceRtcSetTick(&local, &local_tick);
    return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}", local.year, local.month, local.day,
                       local.hour, local.minute);
}

/// Where Ui keeps the titles' names between runs (Ui::title_cache): a line a file, its size, path,
/// name, publisher and whether its icon is cached, tab-separated.
constexpr char TitleCacheFile[] = "ux0:data/azahar/titles.txt";
/// The titles' 48x48 icons, a file a title (IconFile), RGB565 rows as the SMDH decodes them.
constexpr char IconDirectory[] = "ux0:data/azahar/icons";
constexpr u32 IconSize = 48;
/// The font atlas's slots for icons: a screen shows at most a dozen, and the rest keep the
/// icons last shown, so that scrolling back finds them still there.
constexpr int IconSlots = 40;

std::string IconFile(const std::string& path) {
    u64 hash = 0xcbf29ce484222325ull; // FNV-1a over the path
    for (const unsigned char c : path) {
        hash = (hash ^ c) * 0x100000001b3ull;
    }
    return fmt::format("{}/{:016x}.rgb565", IconDirectory, hash);
}

/// One of the SMDH's English strings, on one line and trimmed.
template <std::size_t N>
std::string SmdhText(const std::array<char16_t, N>& field) {
    const std::u16string text(field.data(), std::find(field.begin(), field.end(), u'\0'));
    std::string out = Common::UTF16ToUTF8(text);
    std::replace_if(out.begin(), out.end(), [](char c) { return c == '\n' || c == '\t'; }, ' ');
    const auto first = out.find_first_not_of(' ');
    const auto last = out.find_last_not_of(' ');
    return first == std::string::npos ? std::string{} : out.substr(first, last - first + 1);
}

/// What the game list shows of a title, from its SMDH (a 3DSX's own, when it has one). The large
/// icon goes to the icon cache.
struct TitleInfo {
    std::string name;
    std::string publisher;
    bool has_icon{};
};

/// Whether the file is a CIA, plain or Z3DS-compressed: a title to install onto the emulated SD
/// card, which the list then shows as an installed title.
bool IsCiaFile(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = name.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".cia" || extension == ".zcia";
}

/// A CIA's header, ticket, TMD and SMDH, read through Z3DS where it is compressed. False when the
/// file is no CIA.
bool ReadCia(const std::string& path, FileSys::CIAContainer& container) {
    std::unique_ptr<FileUtil::IOFileBase> file = std::make_unique<FileUtil::IOFile>(path, "rb");
    if (FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get())) {
        file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
    }
    return container.Load(file.get()) == Loader::ResultStatus::Success;
}

TitleInfo ReadTitleInfo(const std::string& path) {
    std::vector<u8> smdh;
    if (IsCiaFile(path)) {
        // No loader opens a CIA: its SMDH is in its meta section.
        FileSys::CIAContainer container;
        if (ReadCia(path, container) && container.GetSMDH()) {
            const auto* const data = reinterpret_cast<const u8*>(container.GetSMDH().get());
            smdh.assign(data, data + sizeof(Loader::SMDH));
        }
    } else if (const std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(path)) {
        loader->ReadIcon(smdh);
    }
    if (!Loader::IsValidSMDH(smdh)) {
        return {};
    }
    Loader::SMDH data;
    std::memcpy(&data, smdh.data(), sizeof(data));
    const auto& title = data.titles[static_cast<int>(Loader::SMDH::TitleLanguage::English)];
    TitleInfo info{SmdhText(title.short_title), SmdhText(title.publisher), false};
    const std::vector<u16> icon = data.GetIcon(true);
    if (icon.size() == IconSize * IconSize) {
        FileUtil::CreateDir(IconDirectory);
        if (std::FILE* const f = std::fopen(IconFile(path).c_str(), "wb")) {
            info.has_icon = std::fwrite(icon.data(), 2, icon.size(), f) == icon.size();
            std::fclose(f);
        }
    }
    return info;
}

/// A cached icon as A8B8G8R8 texels for the font atlas; empty when the file is missing.
std::vector<u32> LoadIcon(const std::string& path) {
    std::vector<u16> rgb565(IconSize * IconSize);
    std::FILE* const f = std::fopen(IconFile(path).c_str(), "rb");
    if (f == nullptr) {
        return {};
    }
    const bool whole = std::fread(rgb565.data(), 2, rgb565.size(), f) == rgb565.size();
    std::fclose(f);
    if (!whole) {
        return {};
    }
    std::vector<u32> texels(rgb565.size());
    for (std::size_t i = 0; i < rgb565.size(); ++i) {
        const u32 c = rgb565[i];
        const u32 r = (c >> 11) * 255 / 31, g = (c >> 5 & 63) * 255 / 63, b = (c & 31) * 255 / 31;
        texels[i] = 0xff000000u | b << 16 | g << 8 | r;
    }
    return texels;
}

bool HasRomExtension(const std::string& name) {
    // Azahar's compressed forms too (Z3DS: .zcci, .zcxi, .z3dsx), which the loaders read as they
    // read the plain ones.
    static constexpr const char* extensions[] = {".3ds",  ".3dsx",  ".cci", ".cxi",
                                                 ".app",  ".elf",   ".axf", ".zcci",
                                                 ".zcxi", ".z3dsx", ".cia", ".zcia"};
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

/// The menus' colours, in two themes (Config::Option::Theme). The light one is the 3DS HOME Menu's:
/// light grey ground, white panels and tiles, dark grey text. The dark one keeps its roles on dark
/// grey, never black: the ground darkest, panels and tiles a step lighter each, light grey text.
/// Both mark what is selected in the HOME Menu's teal and a title's own setting in a warm amber.
struct Palette {
    ImVec4 Backdrop;
    ImVec4 Panel;
    ImVec4 Row;
    ImVec4 RowFocused;
    ImVec4 Accent;
    ImVec4 AccentDim;
    ImVec4 Icon;
    ImVec4 Own;
    ImVec4 Text;
    ImVec4 TextDim;
    ImVec4 Line;
    ImVec4 Shadow;
    ImVec4 SwitchOff;
    ImVec4 Bar;
    /// The pad buttons' discs (DrawGlyph): darker than the footer, for the face buttons' light
    /// colours to show on.
    ImVec4 ButtonDisc;
    /// The game list's speech bubble and its two lines: the theme's colours turned around, so
    /// that it stands out over the tiles it lies on.
    ImVec4 Bubble;
    ImVec4 BubbleText;
    ImVec4 BubbleTextDim;
};

const Palette LightPalette{
    .Backdrop{0.898f, 0.902f, 0.910f, 1.0f},
    .Panel{0.980f, 0.980f, 0.984f, 0.98f},
    .Row{1.0f, 1.0f, 1.0f, 1.0f},
    .RowFocused{0.925f, 0.984f, 0.969f, 1.0f},
    .Accent{0.196f, 0.784f, 0.659f, 1.0f},
    .AccentDim{0.196f, 0.784f, 0.659f, 0.30f},
    .Icon{0.282f, 0.569f, 0.859f, 1.0f},
    .Own{0.918f, 0.573f, 0.141f, 1.0f},
    .Text{0.275f, 0.282f, 0.302f, 1.0f},
    .TextDim{0.533f, 0.541f, 0.569f, 1.0f},
    .Line{0.0f, 0.0f, 0.0f, 0.09f},
    .Shadow{0.0f, 0.0f, 0.0f, 0.10f},
    .SwitchOff{0.780f, 0.788f, 0.808f, 1.0f},
    .Bar{0.886f, 0.890f, 0.898f, 1.0f},
    .ButtonDisc{0.333f, 0.345f, 0.373f, 1.0f},
    .Bubble{0.235f, 0.243f, 0.267f, 1.0f},
    .BubbleText{0.980f, 0.980f, 0.984f, 1.0f},
    .BubbleTextDim{0.733f, 0.745f, 0.769f, 1.0f},
};

/// The teal and the blue a little lighter than the light theme's, for their contrast on dark grey.
const Palette DarkPalette{
    .Backdrop{0.118f, 0.125f, 0.141f, 1.0f},
    .Panel{0.161f, 0.169f, 0.188f, 0.98f},
    .Row{0.212f, 0.224f, 0.247f, 1.0f},
    .RowFocused{0.165f, 0.286f, 0.263f, 1.0f},
    .Accent{0.290f, 0.835f, 0.710f, 1.0f},
    .AccentDim{0.290f, 0.835f, 0.710f, 0.30f},
    .Icon{0.420f, 0.663f, 0.925f, 1.0f},
    .Own{0.961f, 0.651f, 0.290f, 1.0f},
    .Text{0.902f, 0.910f, 0.925f, 1.0f},
    .TextDim{0.600f, 0.616f, 0.647f, 1.0f},
    .Line{1.0f, 1.0f, 1.0f, 0.08f},
    .Shadow{0.0f, 0.0f, 0.0f, 0.35f},
    .SwitchOff{0.341f, 0.353f, 0.384f, 1.0f},
    .Bar{0.090f, 0.094f, 0.106f, 1.0f},
    .ButtonDisc{0.067f, 0.071f, 0.078f, 1.0f},
    .Bubble{0.902f, 0.910f, 0.925f, 1.0f},
    .BubbleText{0.118f, 0.125f, 0.141f, 1.0f},
    .BubbleTextDim{0.380f, 0.392f, 0.420f, 1.0f},
};

const Palette* palette = &DarkPalette;

ImU32 Colour(const ImVec4& colour, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(colour.x, colour.y, colour.z, colour.w * alpha));
}

/// The screens' frame: the header with the screen's name, the footer with its buttons, the
/// margin around them, a row's height and rounding. A row is a finger high.
constexpr float Margin = 16.0f;
constexpr float HeaderHeight = 64.0f;
constexpr float FooterHeight = 44.0f;
/// The footer's buttons: the pad's (FooterButton) and the game list's tools (BarButton).
constexpr float FooterButtonHeight = 32.0f;
constexpr float TabsHeight = 44.0f;
constexpr float RowHeight = 56.0f;
constexpr float RowRounding = 12.0f;
/// Titles: the menus' font at its own 26 pixels (their text is at 3/4 of it).
constexpr float TitleSize = 26.0f;

void ApplyStyle() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 14.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 12.0f;
    style.PopupRounding = 12.0f;
    style.GrabRounding = 10.0f;
    style.ScrollbarRounding = 10.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 0.0f;
    style.WindowPadding = ImVec2(16.0f, 16.0f);
    style.FramePadding = ImVec2(14.0f, 10.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.ItemInnerSpacing = ImVec2(10.0f, 8.0f);
    // A finger's width: the scroll bar is what touch scrolls with.
    style.ScrollbarSize = 18.0f;
    style.GrabMinSize = 24.0f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = palette->Text;
    c[ImGuiCol_TextDisabled] = palette->TextDim;
    c[ImGuiCol_WindowBg] = palette->Panel;
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = palette->Panel;
    c[ImGuiCol_Border] = palette->Line;
    c[ImGuiCol_FrameBg] = palette->Row;
    c[ImGuiCol_FrameBgHovered] = palette->RowFocused;
    c[ImGuiCol_FrameBgActive] = palette->RowFocused;
    c[ImGuiCol_Button] = palette->Row;
    c[ImGuiCol_ButtonHovered] = palette->RowFocused;
    c[ImGuiCol_ButtonActive] = palette->AccentDim;
    c[ImGuiCol_Header] = palette->Row;
    c[ImGuiCol_HeaderHovered] = palette->RowFocused;
    c[ImGuiCol_HeaderActive] = palette->AccentDim;
    c[ImGuiCol_CheckMark] = palette->Accent;
    c[ImGuiCol_SliderGrab] = palette->Accent;
    c[ImGuiCol_SliderGrabActive] = palette->Accent;
    c[ImGuiCol_Separator] = palette->Line;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = palette->Row;
    c[ImGuiCol_ScrollbarGrabHovered] = palette->RowFocused;
    c[ImGuiCol_ScrollbarGrabActive] = palette->AccentDim;
    c[ImGuiCol_NavHighlight] = palette->Accent;
}

void UseTheme() {
    const Palette* const chosen =
        Config::Effective(Config::Option::Theme) == 0 ? &DarkPalette : &LightPalette;
    if (chosen != palette) {
        palette = chosen;
        ApplyStyle();
    }
}

/// The menus' icons, drawn from shapes: the font has ASCII alone, and an icon texture would
/// cost the atlas's CDRAM.
enum class Icon {
    Play,
    Cartridge,
    Gear,
    Disk,
    People,
    Camera,
    Home,
    Power,
    Screens,
    Gauge,
    Speaker,
    Globe,
    Install,
    Patch,
    Cheat,
};

/// An icon of `size` pixels centred at `c`. `under`: the colour behind it, for the cut-outs.
void DrawIcon(ImDrawList* dl, Icon icon, ImVec2 c, float size, ImU32 colour, ImU32 under) {
    const float s = size;
    const float line = std::max(2.0f, s * 0.08f);
    switch (icon) {
    case Icon::Play:
        dl->AddTriangleFilled(ImVec2(c.x - 0.28f * s, c.y - 0.38f * s), ImVec2(c.x + 0.4f * s, c.y),
                              ImVec2(c.x - 0.28f * s, c.y + 0.38f * s), colour);
        break;
    case Icon::Cartridge: {
        // A 3DS game card: its top right corner cut, a label.
        const float x0 = c.x - 0.32f * s, x1 = c.x + 0.32f * s;
        const float y0 = c.y - 0.42f * s, y1 = c.y + 0.42f * s;
        const float cut = 0.16f * s;
        dl->PathLineTo(ImVec2(x0, y0));
        dl->PathLineTo(ImVec2(x1 - cut, y0));
        dl->PathLineTo(ImVec2(x1, y0 + cut));
        dl->PathLineTo(ImVec2(x1, y1));
        dl->PathLineTo(ImVec2(x0, y1));
        dl->PathStroke(colour, true, line);
        dl->AddRectFilled(ImVec2(c.x - 0.2f * s, c.y - 0.04f * s),
                          ImVec2(c.x + 0.2f * s, c.y + 0.3f * s), colour, 2.0f);
        break;
    }
    case Icon::Gear:
        for (int i = 0; i < 8; i++) {
            const float a = static_cast<float>(i) * IM_PI / 4.0f;
            const ImVec2 dir(std::cos(a), std::sin(a));
            const ImVec2 side(-dir.y * 0.09f * s, dir.x * 0.09f * s);
            const ImVec2 inner = c + dir * (0.28f * s);
            const ImVec2 outer = c + dir * (0.45f * s);
            dl->AddQuadFilled(inner - side, outer - side, outer + side, inner + side, colour);
        }
        dl->AddCircle(c, 0.25f * s, colour, 20, 0.14f * s);
        break;
    case Icon::Disk:
        dl->AddRect(ImVec2(c.x - 0.38f * s, c.y - 0.38f * s),
                    ImVec2(c.x + 0.38f * s, c.y + 0.38f * s), colour, 3.0f, ImDrawCornerFlags_All,
                    line);
        dl->AddRectFilled(ImVec2(c.x - 0.2f * s, c.y - 0.38f * s),
                          ImVec2(c.x + 0.14f * s, c.y - 0.1f * s), colour);
        dl->AddRect(ImVec2(c.x - 0.24f * s, c.y + 0.04f * s),
                    ImVec2(c.x + 0.24f * s, c.y + 0.38f * s), colour, 2.0f, ImDrawCornerFlags_All,
                    line);
        break;
    case Icon::People:
        for (const float x : {c.x - 0.17f * s, c.x + 0.17f * s}) {
            dl->AddCircleFilled(ImVec2(x, c.y - 0.16f * s), 0.12f * s, colour, 16);
            dl->PathArcTo(ImVec2(x, c.y + 0.34f * s), 0.2f * s, IM_PI, 2.0f * IM_PI, 12);
            dl->PathFillConvex(colour);
        }
        break;
    case Icon::Camera:
        dl->AddRect(ImVec2(c.x - 0.4f * s, c.y - 0.22f * s), ImVec2(c.x + 0.4f * s, c.y + 0.3f * s),
                    colour, 3.0f, ImDrawCornerFlags_All, line);
        dl->AddRectFilled(ImVec2(c.x - 0.14f * s, c.y - 0.34f * s),
                          ImVec2(c.x + 0.14f * s, c.y - 0.22f * s), colour);
        dl->AddCircle(ImVec2(c.x, c.y + 0.04f * s), 0.15f * s, colour, 16, line);
        break;
    case Icon::Home:
        dl->AddTriangleFilled(ImVec2(c.x, c.y - 0.42f * s), ImVec2(c.x - 0.44f * s, c.y),
                              ImVec2(c.x + 0.44f * s, c.y), colour);
        dl->AddRectFilled(ImVec2(c.x - 0.3f * s, c.y - 0.04f * s),
                          ImVec2(c.x + 0.3f * s, c.y + 0.38f * s), colour);
        dl->AddRectFilled(ImVec2(c.x - 0.09f * s, c.y + 0.12f * s),
                          ImVec2(c.x + 0.09f * s, c.y + 0.38f * s), under);
        break;
    case Icon::Power:
        dl->PathArcTo(c, 0.34f * s, -0.5f * IM_PI + 0.65f, 1.5f * IM_PI - 0.65f, 20);
        dl->PathStroke(colour, false, 0.1f * s);
        dl->AddLine(ImVec2(c.x, c.y - 0.44f * s), ImVec2(c.x, c.y - 0.04f * s), colour, 0.1f * s);
        break;
    case Icon::Screens:
        // The 3DS's two screens, the top one wider.
        dl->AddRect(ImVec2(c.x - 0.44f * s, c.y - 0.42f * s),
                    ImVec2(c.x + 0.44f * s, c.y - 0.04f * s), colour, 2.0f, ImDrawCornerFlags_All,
                    line);
        dl->AddRect(ImVec2(c.x - 0.32f * s, c.y + 0.04f * s),
                    ImVec2(c.x + 0.32f * s, c.y + 0.4f * s), colour, 2.0f, ImDrawCornerFlags_All,
                    line);
        break;
    case Icon::Gauge: {
        const ImVec2 hub(c.x, c.y + 0.14f * s);
        dl->PathArcTo(hub, 0.42f * s, IM_PI, 2.0f * IM_PI, 20);
        dl->PathStroke(colour, false, 0.09f * s);
        const float a = 1.72f * IM_PI;
        dl->AddLine(hub, hub + ImVec2(std::cos(a), std::sin(a)) * (0.34f * s), colour, 0.08f * s);
        dl->AddCircleFilled(hub, 0.08f * s, colour, 12);
        break;
    }
    case Icon::Speaker:
        dl->AddRectFilled(ImVec2(c.x - 0.42f * s, c.y - 0.12f * s),
                          ImVec2(c.x - 0.24f * s, c.y + 0.12f * s), colour);
        dl->AddQuadFilled(ImVec2(c.x - 0.24f * s, c.y - 0.12f * s), ImVec2(c.x, c.y - 0.34f * s),
                          ImVec2(c.x, c.y + 0.34f * s), ImVec2(c.x - 0.24f * s, c.y + 0.12f * s),
                          colour);
        for (const float r : {0.18f * s, 0.34f * s}) {
            dl->PathArcTo(ImVec2(c.x + 0.04f * s, c.y), r, -0.3f * IM_PI, 0.3f * IM_PI, 10);
            dl->PathStroke(colour, false, 0.07f * s);
        }
        break;
    case Icon::Globe:
        dl->AddCircle(c, 0.4f * s, colour, 24, line);
        for (int i = 0; i < 24; i++) {
            const float a = static_cast<float>(i) * IM_PI / 12.0f;
            dl->PathLineTo(ImVec2(c.x + 0.17f * s * std::cos(a), c.y + 0.4f * s * std::sin(a)));
        }
        dl->PathStroke(colour, true, line);
        dl->AddLine(ImVec2(c.x - 0.4f * s, c.y), ImVec2(c.x + 0.4f * s, c.y), colour, line);
        break;
    case Icon::Install: {
        // An arrow down into a tray, as a CIA's badge in the grid.
        const float w = 0.09f * s;
        dl->AddLine(c + ImVec2(0.0f, -0.38f * s), c + ImVec2(0.0f, 0.02f * s), colour, w);
        dl->AddTriangleFilled(c + ImVec2(-0.2f * s, -0.02f * s), c + ImVec2(0.2f * s, -0.02f * s),
                              c + ImVec2(0.0f, 0.2f * s), colour);
        dl->PathLineTo(c + ImVec2(-0.36f * s, 0.1f * s));
        dl->PathLineTo(c + ImVec2(-0.36f * s, 0.36f * s));
        dl->PathLineTo(c + ImVec2(0.36f * s, 0.36f * s));
        dl->PathLineTo(c + ImVec2(0.36f * s, 0.1f * s));
        dl->PathStroke(colour, false, w);
        break;
    }
    case Icon::Patch: {
        // A sticking plaster across the icon, rising to the right, its pad outlined.
        const ImVec2 d(0.7071f, -0.7071f);
        const ImVec2 n(-d.y, d.x);
        const float half = 0.16f * s;
        const ImVec2 a = c - d * (0.28f * s);
        const ImVec2 b = c + d * (0.28f * s);
        dl->AddQuadFilled(a - n * half, b - n * half, b + n * half, a + n * half, colour);
        dl->AddCircleFilled(a, half, colour, 16);
        dl->AddCircleFilled(b, half, colour, 16);
        const float pad = 0.1f * s;
        dl->AddQuad(c - d * pad - n * pad, c + d * pad - n * pad, c + d * pad + n * pad,
                    c - d * pad + n * pad, under, line * 0.75f);
        break;
    }
    case Icon::Cheat: {
        // A four-pointed sparkle, and a small one at its top right.
        const auto sparkle = [&](ImVec2 at, float r) {
            const float w = 0.28f * r;
            for (const ImVec2 d : {ImVec2(0, -1), ImVec2(1, 0), ImVec2(0, 1), ImVec2(-1, 0)}) {
                const ImVec2 n(-d.y, d.x);
                dl->AddTriangleFilled(at + d * r, at + n * w, at - n * w, colour);
            }
        };
        sparkle(c + ImVec2(-0.08f, 0.08f) * s, 0.36f * s);
        sparkle(c + ImVec2(0.3f, -0.3f) * s, 0.14f * s);
        break;
    }
    }
}

/// A toggle's switch, its right end at `right` and centred on the row's `mid`: the accent
/// colour when on, the knob at that end. Returns the knob's x.
float DrawSwitch(ImDrawList* dl, float right, float mid, bool on, float alpha) {
    constexpr float Width = 56.0f;
    const ImVec2 min(right - Width, mid - 15.0f);
    const ImVec2 max(right, mid + 15.0f);
    dl->AddRectFilled(min, max, Colour(on ? palette->Accent : palette->SwitchOff, alpha), 15.0f);
    const float knob = on ? max.x - 15.0f : min.x + 15.0f;
    dl->AddCircleFilled(ImVec2(knob, mid), 11.0f, Colour(palette->Text, alpha), 16);
    return knob;
}

enum class Glyph {
    Cross,
    Circle,
    Square,
    Triangle,
    LeftRight,
    Shoulders,
};

/// A pad button's glyph centred at `c`, `r` its radius: the face buttons in their own colours on
/// a dark disc, as the console's own menus show them.
void DrawGlyph(ImDrawList* dl, Glyph glyph, ImVec2 c, float r) {
    const ImU32 disc = Colour(palette->ButtonDisc);
    const ImU32 mark = Colour(palette->Row);
    const float line = 2.0f;
    switch (glyph) {
    case Glyph::Cross: {
        dl->AddCircleFilled(c, r, disc, 20);
        const float d = 0.4f * r;
        const ImU32 blue = Colour(ImVec4(0.55f, 0.72f, 0.97f, 1.0f));
        dl->AddLine(c - ImVec2(d, d), c + ImVec2(d, d), blue, line);
        dl->AddLine(c + ImVec2(-d, d), c + ImVec2(d, -d), blue, line);
        break;
    }
    case Glyph::Circle:
        dl->AddCircleFilled(c, r, disc, 20);
        dl->AddCircle(c, 0.45f * r, Colour(ImVec4(0.96f, 0.45f, 0.48f, 1.0f)), 16, line);
        break;
    case Glyph::Square: {
        dl->AddCircleFilled(c, r, disc, 20);
        const float d = 0.38f * r;
        dl->AddRect(c - ImVec2(d, d), c + ImVec2(d, d), Colour(ImVec4(0.93f, 0.55f, 0.84f, 1.0f)),
                    0.0f, ImDrawCornerFlags_All, line);
        break;
    }
    case Glyph::Triangle:
        dl->AddCircleFilled(c, r, disc, 20);
        dl->AddTriangle(ImVec2(c.x, c.y - 0.48f * r), ImVec2(c.x - 0.46f * r, c.y + 0.32f * r),
                        ImVec2(c.x + 0.46f * r, c.y + 0.32f * r),
                        Colour(ImVec4(0.42f, 0.86f, 0.72f, 1.0f)), line);
        break;
    case Glyph::LeftRight:
        dl->AddCircleFilled(c, r, disc, 20);
        dl->AddTriangleFilled(ImVec2(c.x - 0.6f * r, c.y), ImVec2(c.x - 0.12f * r, c.y - 0.4f * r),
                              ImVec2(c.x - 0.12f * r, c.y + 0.4f * r), mark);
        dl->AddTriangleFilled(ImVec2(c.x + 0.6f * r, c.y), ImVec2(c.x + 0.12f * r, c.y - 0.4f * r),
                              ImVec2(c.x + 0.12f * r, c.y + 0.4f * r), mark);
        break;
    case Glyph::Shoulders: {
        // L and R, two pills.
        const ImVec2 half(0.95f * r, 0.7f * r);
        const std::pair<float, const char*> pills[] = {{c.x - 1.05f * r, "L"},
                                                       {c.x + 1.05f * r, "R"}};
        for (const auto& [x, letter] : pills) {
            dl->AddRectFilled(ImVec2(x, c.y) - half, ImVec2(x, c.y) + half, disc, 0.5f * r);
            const ImVec2 size = ImGui::CalcTextSize(letter);
            dl->AddText(ImVec2(x - size.x * 0.5f, c.y - size.y * 0.5f), mark, letter);
        }
        break;
    }
    }
}

/// What a glyph's width is beside its radius (the shoulders' two pills are wider).
float GlyphWidth(Glyph glyph, float r) {
    return glyph == Glyph::Shoulders ? 4.0f * r : 2.0f * r;
}

/// A centred panel over whatever is behind it, for the pause menu.
bool BeginCentered(const char* id, float width, float height) {
    ImGui::SetNextWindowPos(ImVec2((ScreenWidth - width) * 0.5f, (ScreenHeight - height) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    return ImGui::Begin(id, nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
}

/// `text` shortened with "..." to fit `lines` lines of `width` at the current font (wrapped where
/// there is more than one), for a game's name in a header. Shortened a character at a time, not a
/// byte: a name's UTF-8 cut inside a character would show a question mark.
std::string Fit(const std::string& text, float width, int lines = 1) {
    const auto fits = [&](const std::string& candidate) {
        const ImVec2 size =
            ImGui::CalcTextSize(candidate.c_str(), nullptr, false, lines == 1 ? -1.0f : width);
        return lines == 1 ? size.x <= width
                          : size.y <= static_cast<float>(lines) * ImGui::GetFontSize() + 0.5f;
    };
    if (fits(text)) {
        return text;
    }
    std::string fitted = text;
    while (!fitted.empty() && !fits(fitted + "...")) {
        while (!fitted.empty() && (static_cast<u8>(fitted.back()) & 0xc0) == 0x80) {
            fitted.pop_back();
        }
        if (!fitted.empty()) {
            fitted.pop_back();
        }
    }
    return fitted + "...";
}

/**
 * The full-screen backdrop a screen's windows sit on: the header with the screen's icon, title
 * and a note at its right, and the footer, whose buttons the caller adds (FooterButton) before
 * EndBackdrop. Its own window, behind the screen's others and out of the pad's navigation: what
 * it holds is for touch, and the pad reaches the same through the buttons its footer names.
 */
void BeginBackdrop(const char* id, Icon icon, const char* title, const std::string& note) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth, ScreenHeight));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette->Backdrop);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImDrawList* const dl = ImGui::GetWindowDrawList();
    // The header and the footer span the display: inside the window's clip (half its padding in
    // from each side) they stopped 8 pixels short of each edge.
    dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, ScreenHeight), false);
    const ImU32 panel = Colour(palette->Panel, 1.0f / palette->Panel.w);
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, HeaderHeight), panel);
    dl->AddLine(ImVec2(0.0f, HeaderHeight), ImVec2(ScreenWidth, HeaderHeight),
                Colour(palette->Line));
    DrawIcon(dl, icon, ImVec2(Margin + 16.0f, HeaderHeight * 0.5f), 30.0f, Colour(palette->Accent),
             panel);
    const float title_x = Margin + 44.0f;
    dl->AddText(ImGui::GetFont(), TitleSize, ImVec2(title_x, (HeaderHeight - TitleSize) * 0.5f),
                Colour(palette->Text), title);
    const float title_width = ImGui::GetFont()->CalcTextSizeA(TitleSize, FLT_MAX, 0.0f, title).x;
    // The note at the header's right on one line where it fits; else (a long game's name) over
    // two lines from the title on, and shortened only past those.
    const float note_x = title_x + title_width + 32.0f;
    const float note_room = ScreenWidth - Margin - note_x;
    if (const ImVec2 size = ImGui::CalcTextSize(note.c_str()); size.x <= note_room) {
        dl->AddText(ImVec2(ScreenWidth - Margin - size.x, (HeaderHeight - size.y) * 0.5f),
                    Colour(palette->TextDim), note.c_str());
    } else {
        const std::string shown = Fit(note, note_room, 2);
        const ImVec2 wrapped = ImGui::CalcTextSize(shown.c_str(), nullptr, false, note_room);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(note_x, (HeaderHeight - wrapped.y) * 0.5f), Colour(palette->TextDim),
                    shown.c_str(), nullptr, note_room);
    }

    const float footer = ScreenHeight - FooterHeight;
    dl->AddRectFilled(ImVec2(0.0f, footer), ImVec2(ScreenWidth, ScreenHeight), panel);
    dl->AddLine(ImVec2(0.0f, footer), ImVec2(ScreenWidth, footer), Colour(palette->Line));
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(Margin, footer + (FooterHeight - FooterButtonHeight) * 0.5f));
}

void EndBackdrop() {
    ImGui::End();
}

/// One of the footer's buttons: the pad button's glyph and what it does, a touch target for the
/// same, which never takes the pad's selection. Laid out left to right.
bool FooterButton(Glyph glyph, const char* label) {
    constexpr float R = 13.0f;
    const ImVec2 text = ImGui::CalcTextSize(label);
    const float glyph_width = GlyphWidth(glyph, R);
    const ImVec2 size(glyph_width + 10.0f + text.x + 16.0f, FooterButtonHeight);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_NoNavFocus);
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemActive()) {
        dl->AddRectFilled(at, at + size, Colour(palette->AccentDim), size.y * 0.5f);
    }
    DrawGlyph(dl, glyph, ImVec2(at.x + 10.0f + glyph_width * 0.5f, at.y + size.y * 0.5f), R);
    dl->AddText(ImVec2(at.x + 10.0f + glyph_width + 10.0f, at.y + (size.y - text.y) * 0.5f),
                Colour(palette->Text), label);
    ImGui::SameLine(0.0f, 8.0f);
    return pressed;
}

/**
 * The window the pad navigates on a full-screen menu, between the header (or the tabs) and the
 * footer: transparent, its items drawing their own rows. Kept focused, so that the pad never
 * lands in the backdrop or a side panel; not while a touch is held or an item is being pressed,
 * which would take the press from a button elsewhere (ImGui drops the active item of a window
 * that loses focus).
 */
void BeginList(const char* id, ImVec2 min, ImVec2 max) {
    ImGui::SetNextWindowPos(min);
    ImGui::SetNextWindowSize(max - min);
    if (!ImGui::GetIO().MouseDown[0] && ImGui::GetActiveID() == 0) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    // Room for the selected row's outline.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 8.0f));
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    // A touch drawn up or down from inside the list scrolls it, as the Vita's own lists do, and
    // does not press the item it began on. A sideways drag stays a slider's.
    const ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDown[0] && ImGui::GetCurrentWindow()->Rect().Contains(io.MouseClickedPos[0])) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(0, 0.0f);
        if (std::abs(drag.y) > 12.0f && std::abs(drag.y) > std::abs(drag.x)) {
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
            if (ImGui::GetActiveID() != 0) {
                ImGui::ClearActiveID();
            }
        }
    }
}

/// A card: a rounded row, tile or button the pad selects and cross or a touch presses, outlined
/// in the accent colour while selected. The caller draws what it holds inside `bb`.
struct Card {
    ImGuiID id;
    ImRect bb;
    bool visible;
    bool focused;
    bool pressed;
    /// Pressed with the pad's cross rather than a touch.
    bool by_pad;
};

/// A card's item alone, which the caller draws: the game row's tiles.
Card CardBehavior(const char* str_id, ImVec2 size, bool* held_out = nullptr) {
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    Card card{};
    card.id = window->GetID(str_id);
    card.bb = ImRect(window->DC.CursorPos, window->DC.CursorPos + size);
    ImGui::ItemSize(card.bb);
    card.visible = ImGui::ItemAdd(card.bb, card.id);
    card.focused = ImGui::GetFocusID() == card.id;
    card.by_pad = ImGui::GetCurrentContext()->NavActivateId == card.id;
    bool hovered = false, held = false;
    card.pressed = card.visible && ImGui::ButtonBehavior(card.bb, card.id, &hovered, &held);
    if (held_out) {
        *held_out = held;
    }
    return card;
}

Card CardItem(const char* str_id, ImVec2 size) {
    bool held = false;
    const Card card = CardBehavior(str_id, size, &held);
    if (card.visible) {
        ImGuiWindow* const window = ImGui::GetCurrentWindow();
        ImDrawList* const dl = window->DrawList;
        dl->AddRectFilled(card.bb.Min, card.bb.Max,
                          Colour(card.focused || held ? palette->RowFocused : palette->Row),
                          RowRounding);
        if (card.focused) {
            dl->AddRect(card.bb.Min + ImVec2(1.0f, 1.0f), card.bb.Max - ImVec2(1.0f, 1.0f),
                        Colour(palette->Accent), RowRounding, ImDrawCornerFlags_All, 2.5f);
        }
    }
    return card;
}

/// A card with an icon and a label, for the pause menu's tiles and the multiplayer screen's
/// buttons.
bool IconCard(const char* label, Icon icon, ImVec2 size) {
    const Card card = CardItem(label, size);
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float icon_size = std::min(size.y * 0.5f, 30.0f);
        const ImVec2 icon_at(card.bb.Min.x + 14.0f + icon_size * 0.5f, card.bb.GetCenter().y);
        DrawIcon(dl, icon, icon_at, icon_size, Colour(palette->Accent),
                 Colour(card.focused ? palette->RowFocused : palette->Row));
        const ImVec2 text = ImGui::CalcTextSize(label);
        dl->AddText(
            ImVec2(icon_at.x + icon_size * 0.5f + 14.0f, card.bb.GetCenter().y - text.y * 0.5f),
            Colour(palette->Text), label);
    }
    return card.pressed;
}

float BarButtonWidth(const char* label) {
    return 10.0f + 20.0f + 8.0f + ImGui::CalcTextSize(label).x + 10.0f;
}

/// One of the game list's tools in its footer: an icon and a label on a pill, which the pad
/// selects and cross or a touch presses.
bool BarButton(const char* label, Icon icon) {
    bool held = false;
    const Card card = CardBehavior(label, ImVec2(BarButtonWidth(label), FooterButtonHeight), &held);
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float round = card.bb.GetHeight() * 0.5f;
        const ImU32 fill = Colour(card.focused || held ? palette->RowFocused : palette->Backdrop);
        dl->AddRectFilled(card.bb.Min, card.bb.Max, fill, round);
        if (card.focused) {
            dl->AddRect(card.bb.Min + ImVec2(1.0f, 1.0f), card.bb.Max - ImVec2(1.0f, 1.0f),
                        Colour(palette->Accent), round, ImDrawCornerFlags_All, 2.5f);
        }
        const ImVec2 icon_at(card.bb.Min.x + 20.0f, card.bb.GetCenter().y);
        DrawIcon(dl, icon, icon_at, 20.0f, Colour(palette->Accent), fill);
        const ImVec2 text = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(icon_at.x + 18.0f, card.bb.GetCenter().y - text.y * 0.5f),
                    Colour(palette->Text), label);
    }
    return card.pressed;
}

/// The HOME Menu's mark of the selected tile: a rounded corner at each of the tile's corners,
/// breathing in and out.
void DrawBrackets(ImDrawList* dl, const ImRect& tile, ImU32 colour) {
    const float breath = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 5.0f);
    const float out = 5.0f + 4.0f * breath;
    const ImRect r(tile.Min - ImVec2(out, out), tile.Max + ImVec2(out, out));
    constexpr float Length = 24.0f, Radius = 10.0f, Thickness = 6.0f;
    // Each corner from its horizontal end round to its vertical end.
    const struct {
        ImVec2 corner;
        float sx, sy, from, to;
    } corners[] = {
        {r.Min, 1.0f, 1.0f, 1.5f * IM_PI, IM_PI},
        {ImVec2(r.Max.x, r.Min.y), -1.0f, 1.0f, 1.5f * IM_PI, 2.0f * IM_PI},
        {r.Max, -1.0f, -1.0f, 0.5f * IM_PI, 0.0f},
        {ImVec2(r.Min.x, r.Max.y), 1.0f, -1.0f, 0.5f * IM_PI, IM_PI},
    };
    for (const auto& c : corners) {
        dl->PathLineTo(c.corner + ImVec2(c.sx * Length, 0.0f));
        dl->PathArcTo(c.corner + ImVec2(c.sx * Radius, c.sy * Radius), Radius, c.from, c.to, 6);
        dl->PathLineTo(c.corner + ImVec2(0.0f, c.sy * Length));
        dl->PathStroke(colour, false, Thickness);
    }
}

/// The HOME Menu's status bar: the app's name and `note` at the left, the time in the middle, the
/// battery at the right.
void DrawStatusBar(ImDrawList* dl, float height, const std::string& note) {
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, height), Colour(palette->Bar));
    dl->AddLine(ImVec2(0.0f, height), ImVec2(ScreenWidth, height), Colour(palette->Line));
    const float mid = height * 0.5f;
    const float text_y = mid - ImGui::GetFontSize() * 0.5f;
    dl->AddText(ImVec2(Margin, text_y), Colour(palette->TextDim), "Azahar");
    if (!note.empty()) {
        const float x = Margin + ImGui::CalcTextSize("Azahar").x + 24.0f;
        dl->AddText(ImVec2(x, text_y), Colour(palette->TextDim), note.c_str());
    }
    SceDateTime now{};
    if (sceRtcGetCurrentClockLocalTime(&now) >= 0) {
        const std::string time = fmt::format("{:02}:{:02}", now.hour, now.minute);
        const ImVec2 size = ImGui::CalcTextSize(time.c_str());
        dl->AddText(ImVec2((ScreenWidth - size.x) * 0.5f, mid - size.y * 0.5f),
                    Colour(palette->Text), time.c_str());
    }
    const int percent = scePowerGetBatteryLifePercent();
    if (percent >= 0) {
        const ImVec2 body(34.0f, 16.0f);
        const ImVec2 at(ScreenWidth - Margin - body.x - 4.0f, mid - body.y * 0.5f);
        dl->AddRect(at, at + body, Colour(palette->Text), 3.0f, ImDrawCornerFlags_All, 2.0f);
        dl->AddRectFilled(ImVec2(at.x + body.x, mid - 4.0f),
                          ImVec2(at.x + body.x + 4.0f, mid + 4.0f), Colour(palette->Text), 1.0f);
        const float fill = (body.x - 6.0f) * static_cast<float>(std::min(percent, 100)) / 100.0f;
        const bool low = percent <= 15 && !scePowerIsBatteryCharging();
        dl->AddRectFilled(at + ImVec2(3.0f, 3.0f), ImVec2(at.x + 3.0f + fill, at.y + body.y - 3.0f),
                          Colour(low ? palette->Own : palette->Accent), 1.5f);
        const std::string text = fmt::format("{}%", percent);
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(at.x - 8.0f - size.x, mid - size.y * 0.5f), Colour(palette->TextDim),
                    text.c_str());
    }
}

/// The system volume over whichever menu is open, for two seconds after a volume key changed
/// it (EmuWindowVita::VolumeKeys; in game the stats overlay shows it): a pill under the status
/// bar in the bubble's colours, a speaker, a step of the bar for each of the 30 levels and the
/// level's number. It fades in its last 0.3 seconds.
void DrawVolume() {
    constexpr u32 Levels = 30;
    const u64 until = HudStats::volume_until_us.load(std::memory_order_acquire);
    const u64 now = sceKernelGetProcessTimeWide();
    if (now >= until) {
        return;
    }
    const u32 level = std::min(HudStats::volume_level.load(std::memory_order_relaxed), Levels);
    const float alpha = std::min(1.0f, static_cast<float>(until - now) / 300000.0f);
    ImDrawList* const dl = ImGui::GetForegroundDrawList();
    const std::string number = fmt::format("{}", level);
    const ImVec2 number_size = ImGui::CalcTextSize("30");
    constexpr float Step = 6.0f, StepGap = 2.0f, Height = 40.0f;
    const float bar = static_cast<float>(Levels) * (Step + StepGap) - StepGap;
    const float width = 16.0f + 28.0f + 12.0f + bar + 14.0f + number_size.x + 18.0f;
    const ImRect pill(ImVec2((ScreenWidth - width) * 0.5f, 36.0f),
                      ImVec2((ScreenWidth + width) * 0.5f, 36.0f + Height));
    dl->AddRectFilled(pill.Min + ImVec2(0.0f, 3.0f), pill.Max + ImVec2(0.0f, 3.0f),
                      Colour(palette->Shadow, alpha), Height * 0.5f);
    dl->AddRectFilled(pill.Min, pill.Max, Colour(palette->Bubble, alpha), Height * 0.5f);
    const float mid = pill.GetCenter().y;
    float x = pill.Min.x + 16.0f;
    DrawIcon(dl, Icon::Speaker, ImVec2(x + 14.0f, mid), 28.0f, Colour(palette->BubbleText, alpha),
             Colour(palette->Bubble, alpha));
    x += 28.0f + 12.0f;
    for (u32 i = 0; i < Levels; i++) {
        const ImVec4& c = i < level ? palette->BubbleText : palette->BubbleTextDim;
        dl->AddRectFilled(ImVec2(x, mid - 7.0f), ImVec2(x + Step, mid + 7.0f),
                          Colour(c, i < level ? alpha : alpha * 0.45f), 1.5f);
        x += Step + StepGap;
    }
    x += 14.0f - StepGap;
    const ImVec2 size = ImGui::CalcTextSize(number.c_str());
    dl->AddText(ImVec2(x + number_size.x - size.x, mid - size.y * 0.5f),
                Colour(palette->BubbleText, alpha), number.c_str());
}

/// Makes the item just added the pad's selection. Over the first item, which ImGui would select
/// in a window that has just appeared (its navigation's init request, applied at the next frame).
void FocusLastItem() {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* const window = ImGui::GetCurrentWindow();
    ImGui::SetFocusID(window->DC.LastItemId, window);
    g.NavDisableHighlight = false;
    g.NavInitRequest = false;
    g.NavInitResultId = 0;
    ImGui::SetScrollHereY(0.5f);
}

/// A bullet of the settings' side panel: a coloured dot and wrapped text.
void Note(const ImVec4& dot, const std::string& text) {
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddCircleFilled(
        ImVec2(at.x + 5.0f, at.y + ImGui::GetFontSize() * 0.5f), 4.5f, Colour(dot), 12);
    ImGui::Indent(18.0f);
    ImGui::TextWrapped("%s", text.c_str());
    ImGui::Unindent(18.0f);
}

const char* CategoryName(int category) {
    static constexpr const char* names[] = {"Display", "Speed",  "Sound",    "System",
                                            "Patches", "Cheats", "Installed"};
    return names[category];
}

Icon CategoryIcon(int category) {
    static constexpr Icon icons[] = {Icon::Screens, Icon::Gauge, Icon::Speaker, Icon::Globe,
                                     Icon::Patch,   Icon::Cheat, Icon::Disk};
    return icons[category];
}

u64 TreeSize(const FileUtil::FSTEntry& entry) {
    u64 size = 0;
    for (const FileUtil::FSTEntry& child : entry.children) {
        size += child.isDirectory ? TreeSize(child) : child.size;
    }
    return size;
}

/// The title's program ID, as its file names it; 0 for a file without one (a 3DSX).
u64 ProgramIdOf(const std::string& path) {
    u64 program_id = 0;
    if (const std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(path)) {
        loader->ReadProgramId(program_id);
    }
    return program_id;
}

/// A new ImGui context for the menus, made current: their style in the theme the settings choose,
/// and their fonts.
void CreateMenuContext() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    // A new context starts in ImGui's own style, whichever palette was chosen last.
    palette = Config::Effective(Config::Option::Theme) == 0 ? &DarkPalette : &LightPalette;
    ApplyStyle();

    ImGuiIO& io = ImGui::GetIO();
    // Nothing to remember between sessions: every window here is placed by the code.
    io.IniFilename = nullptr;
    // Two sizes of ImGui's own font (ProggyClean): 13 pixels for the stats overlay (StatsHud), and
    // 26 for the menus, drawn at 3/4 for their text and whole for titles. A bitmap font rasterized
    // at a size not a multiple of its own comes out uneven, and twice its size sampled down stays
    // even.
    // The overlay's has Basic Latin alone, the menus' Latin-1 as well (a ROM's file name may have
    // accents), and there are no software cursors, so that both fit the 512-wide atlas in one 256
    // KiB unit of CDRAM.
    static constexpr ImWchar Ascii[] = {0x20, 0x7e, 0};
    static constexpr ImWchar Latin1[] = {0x20, 0x7e, 0xa0, 0xff, 0};
    ImFontConfig font{};
    font.GlyphRanges = Ascii;
    // As AddFontDefault sets them with no configuration of the caller's: the font's pixels as they
    // are, not oversampled three times across.
    font.OversampleH = 1;
    font.OversampleV = 1;
    font.PixelSnapH = true;
    io.Fonts->AddFontDefault(&font);
    font.SizePixels = 26.0f;
    font.GlyphRanges = Latin1;
    io.FontDefault = io.Fonts->AddFontDefault(&font);
    io.Fonts->Flags |= ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoPowerOfTwoHeight;
    io.FontGlobalScale = 0.75f;
}
} // Anonymous namespace

void ShowGxmNotice(const char* title, const std::string& text, const std::string& hint,
                   const char* button) {
    if (!GxmPresent::Initialize()) {
        LOG_CRITICAL(Frontend, "Could not bring up GXM for: {}", text);
        return;
    }
    CreateMenuContext();
    ImGui_ImplVita_Init();
    const bool drawable = ImGui_ImplGxm_Init();
    ASSERT_MSG(drawable, "the notice's GXM programs or font atlas failed");
    // The pad and the front panel sampled as EmuWindowVita samples them, which comes later: the
    // panel reports nothing until its sampling starts.
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    // The text in the middle of the screen between the header and the footer, the hint under it
    // in the dimmer colour.
    constexpr float Width = 720.0f;
    constexpr float HintGap = 28.0f;
    // A press of cross counts from up to down: one still held from the launch does not pass it.
    bool cross_was_down = true;
    for (bool done = false; !done;) {
        ImGui_ImplVita_NewFrame();
        // Read before the frame ends, which clears ImGui's pad inputs.
        const bool cross_down = ImGui::GetIO().NavInputs[ImGuiNavInput_Activate] > 0.0f;
        BeginBackdrop("##notice", Icon::Cartridge, title, "");
        const bool touched = FooterButton(Glyph::Cross, button);
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        ImFont* const font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const ImVec2 body = ImGui::CalcTextSize(text.c_str(), nullptr, false, Width);
        const ImVec2 note = ImGui::CalcTextSize(hint.c_str(), nullptr, false, Width);
        const float height = body.y + (hint.empty() ? 0.0f : HintGap + note.y);
        const float x = (ScreenWidth - Width) * 0.5f;
        const float y =
            std::max(HeaderHeight + Margin,
                     HeaderHeight + (ScreenHeight - FooterHeight - HeaderHeight - height) * 0.5f);
        dl->AddText(font, size, ImVec2(x, y), Colour(palette->Text), text.c_str(), nullptr, Width);
        if (!hint.empty()) {
            dl->AddText(font, size, ImVec2(x, y + body.y + HintGap), Colour(palette->TextDim),
                        hint.c_str(), nullptr, Width);
        }
        EndBackdrop();
        ImGui::Render();

        done = touched || (cross_down && !cross_was_down);
        cross_was_down = cross_down;
        // No clear: the backdrop covers every pixel.
        GxmPresent::BeginFrame();
        ImGui_ImplGxm_RenderDrawData(ImGui::GetDrawData());
        GxmPresent::EndFrame();
    }

    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    ImGui_ImplGxm_Shutdown();
    ImGui::DestroyContext();
    GxmPresent::Shutdown();
}

Ui::Ui(Core::System& system_, EmuWindowVita& window_) : system{system_}, window{window_} {
    CreateMenuContext();
    ImGuiIO& io = ImGui::GetIO();
    // The titles' icons get slots of the atlas, written as the game list shows them (IconFor):
    // the menus draw with one texture, and a slot is a rectangle of it like a glyph.
    for (int i = 0; i < IconSlots; ++i) {
        icon_rects.push_back(io.Fonts->AddCustomRectRegular(IconSize, IconSize));
    }

    ImGui_ImplVita_Init();
    ImGui_ImplSgxd_Init();
    for (const int rect : icon_rects) {
        const ImFontAtlasCustomRect* r = io.Fonts->GetCustomRectByIndex(rect);
        IconSlot slot{};
        slot.x = r->X;
        slot.y = r->Y;
        // Half a texel in from each edge: the filter never reaches a neighbouring slot or glyph.
        const ImVec2 texel(1.0f / io.Fonts->TexWidth, 1.0f / io.Fonts->TexHeight);
        io.Fonts->CalcCustomRectUV(r, &slot.uv0, &slot.uv1);
        slot.uv0 = slot.uv0 + texel * 0.5f;
        slot.uv1 = slot.uv1 - texel * 0.5f;
        icon_slots.push_back(slot);
    }
    StatsHud::Init();
    // The backend has copied the font atlas into GPU memory by now, and the pixels it was
    // built from are heap nothing reads again. ImGui rebuilds them on demand if anything ever
    // does.
    io.Fonts->ClearTexData();
    ImGui_ImplVita_TouchUsage(true);
    ImGui_ImplVita_GamepadUsage(true);
    ImGui_ImplVita_MouseStickUsage(true);

    BuildRows();
    ScanGames();
    focus_first_row = true;
}

const Ui::IconSlot* Ui::IconFor(const GameEntry& game) {
    if (!game.has_icon || icon_slots.empty() || icon_missing.contains(game.path)) {
        return nullptr;
    }
    for (IconSlot& slot : icon_slots) {
        if (slot.path == game.path) {
            slot.used = frame;
            return &slot;
        }
    }
    // The slot drawn longest ago; not one a frame still in flight may sample.
    IconSlot* oldest = &icon_slots.front();
    for (IconSlot& slot : icon_slots) {
        if (slot.used < oldest->used) {
            oldest = &slot;
        }
    }
    if (!oldest->path.empty() && oldest->used + 4 > frame) {
        return nullptr;
    }
    const std::vector<u32> texels = LoadIcon(game.path);
    if (texels.empty()) {
        icon_missing.insert(game.path);
        return nullptr;
    }
    if (!SgxdPresent::UpdateTexture(static_cast<u32>(oldest->x), static_cast<u32>(oldest->y),
                                    IconSize, IconSize, texels.data())) {
        return nullptr;
    }
    oldest->path = game.path;
    oldest->used = frame;
    return oldest;
}

std::string Ui::TogglePin() {
    const std::string& path = selected_game_path;
    const bool pinned = LiveArea::IsPinned(path);
    std::string failure;
    if (pinned) {
        failure = LiveArea::Unpin(path);
    } else {
        // The bubble takes the title's own name, else its file's.
        const auto cached = title_cache.find(path);
        const std::string name = cached != title_cache.end() && !cached->second.name.empty()
                                     ? cached->second.name
                                     : std::string(FileUtil::GetFilename(path));
        const auto found = std::find_if(games.begin(), games.end(),
                                        [&](const GameEntry& g) { return g.path == path; });
        const bool icon = found != games.end() && found->has_icon;
        failure = LiveArea::Pin(path, name, icon ? LoadIcon(path) : std::vector<u32>{});
    }
    pin_checked_path.clear();
    if (!failure.empty()) {
        return failure;
    }
    return pinned ? "The title's bubble is off the LiveArea."
                  : "Pinned: its LiveArea bubble starts the game in Azahar.";
}

Ui::~Ui() {
    ImGui_ImplSgxd_Shutdown();
    ImGui::DestroyContext();
}

void Ui::LoadTitleCache() {
    title_cache_loaded = true;
    std::FILE* const f = std::fopen(TitleCacheFile, "rb");
    if (f == nullptr) {
        return;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string text{line};
        if (!text.empty() && text.back() == '\n') {
            text.pop_back();
        }
        const auto tab1 = text.find('\t');
        const auto tab2 = tab1 == std::string::npos ? tab1 : text.find('\t', tab1 + 1);
        if (tab2 == std::string::npos) {
            continue;
        }
        // size, path, name[, publisher, icon]: a line of before publishers and icons is read
        // again from its file.
        const auto tab3 = text.find('\t', tab2 + 1);
        const auto tab4 = tab3 == std::string::npos ? tab3 : text.find('\t', tab3 + 1);
        CachedTitle title{std::strtoull(text.c_str(), nullptr, 10), {}, {}, false, false};
        if (tab4 == std::string::npos) {
            title.name = text.substr(tab2 + 1);
            title.stale = true;
        } else {
            title.name = text.substr(tab2 + 1, tab3 - tab2 - 1);
            title.publisher = text.substr(tab3 + 1, tab4 - tab3 - 1);
            title.has_icon = text.substr(tab4 + 1) == "1";
        }
        title_cache[text.substr(tab1 + 1, tab2 - tab1 - 1)] = std::move(title);
    }
    std::fclose(f);
}

void Ui::SaveTitleCache() const {
    std::FILE* const f = std::fopen(TitleCacheFile, "wb");
    if (f == nullptr) {
        LOG_WARNING(Frontend, "Could not write {}", TitleCacheFile);
        return;
    }
    for (const auto& [path, title] : title_cache) {
        std::fprintf(f, "%llu\t%s\t%s\t%s\t%d\n", static_cast<unsigned long long>(title.size),
                     path.c_str(), title.name.c_str(), title.publisher.c_str(),
                     title.has_icon ? 1 : 0);
    }
    std::fclose(f);
}

void Ui::ScanGames() {
    if (!title_cache_loaded) {
        LoadTitleCache();
    }
    // A rescan keeps the title selected, wherever titles added or gone put it in the list.
    const std::string selected_path =
        selected_game >= 0 && selected_game < static_cast<int>(games.size())
            ? games[static_cast<std::size_t>(selected_game)].path
            : std::string{};
    games.clear();
    bool cache_changed = false;
    // A title's entry, `file` naming it where its SMDH has no name.
    const auto add = [&](const std::string& path, u64 size, const std::string& file,
                         const char* suffix) {
        auto cached = title_cache.find(path);
        if (cached == title_cache.end() || cached->second.size != size || cached->second.stale) {
            TitleInfo info = ReadTitleInfo(path);
            cached = title_cache
                         .insert_or_assign(path, CachedTitle{size, std::move(info.name),
                                                             std::move(info.publisher),
                                                             info.has_icon, false})
                         .first;
            cache_changed = true;
        }
        const CachedTitle& title = cached->second;
        games.push_back({path, title.name.empty() ? file + suffix : title.name + suffix,
                         title.name.empty() ? std::string{} : file, title.publisher,
                         title.has_icon, IsCiaFile(path)});
    };
    const auto scan = [&](const char* directory, const char* suffix) {
        FileUtil::FSTEntry root;
        if (!FileUtil::ScanDirectoryTree(directory, root)) {
            LOG_WARNING(Frontend, "No game directory at {}", directory);
            return;
        }
        for (const auto& entry : root.children) {
            if (!entry.isDirectory && HasRomExtension(entry.virtualName)) {
                add(entry.physicalName, entry.size, entry.virtualName, suffix);
            }
        }
    };
    scan(GameDirectory, "");
    for (const std::string& folder : Config::RomFolders()) {
        scan(folder.c_str(), "");
    }
#ifdef VITA_DIAGNOSTICS
    scan(HostGameDirectory, " (host0)");
#endif
    // The titles installed from CIAs onto the emulated SD card, applications and demos, each by
    // its boot content, named by its title ID where it has no SMDH.
    const std::string title_root = Service::AM::GetMediaTitlePath(Service::FS::MediaType::SDMC);
    for (const u32 high : {0x00040000u, 0x00040002u}) {
        FileUtil::FSTEntry titles;
        if (!FileUtil::ScanDirectoryTree(fmt::format("{}{:08x}", title_root, high), titles)) {
            continue;
        }
        for (const auto& title : titles.children) {
            if (!title.isDirectory) {
                continue;
            }
            const u64 title_id =
                u64{high} << 32 | std::strtoul(title.virtualName.c_str(), nullptr, 16);
            const std::string content =
                Service::AM::GetTitleContentPath(Service::FS::MediaType::SDMC, title_id);
            if (const u64 size = content.empty() ? 0 : FileUtil::GetSize(content); size != 0) {
                add(content, size, fmt::format("{:016X}", title_id), "");
            }
        }
    }
    if (cache_changed) {
        SaveTitleCache();
    }
    std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) {
        return NaturalLess(a.name, b.name) ||
               (!NaturalLess(b.name, a.name) && NaturalLess(a.file, b.file));
    });
    const auto kept = std::find_if(games.begin(), games.end(), [&](const GameEntry& game) {
        return game.path == selected_path;
    });
    selected_game =
        kept != games.end()
            ? static_cast<int>(kept - games.begin())
            : std::clamp(selected_game, 0, std::max(static_cast<int>(games.size()) - 1, 0));
    // The grid's focus is a tile's place in the list: it moves to the title's new place at the
    // next frame, as the page arrows' does (not off the tools, where the pad is).
    if (!tools_focused) {
        carousel_focus = selected_game;
    }
    LOG_INFO(Frontend, "Found {} titles", games.size());
}

std::string Ui::SettingTitle(Config::Option option) const {
    const auto row = std::find_if(rows.begin(), rows.end(),
                                  [option](const Row& row) { return row.option == option; });
    ASSERT_MSG(row != rows.end(), "settings: option {} has no row", static_cast<int>(option));
    return Tr(row->title);
}

std::string Ui::SettingDefault(Config::Option option) const {
    const auto row = std::find_if(rows.begin(), rows.end(),
                                  [option](const Row& row) { return row.option == option; });
    ASSERT_MSG(row != rows.end(), "settings: option {} has no row", static_cast<int>(option));
    return ValueText(*row, Config::Default(option));
}

void Ui::BuildRows() {
    using Config::Option;
    using Kind = Row::Kind;
    const auto add = [this](Row row) {
        ASSERT_MSG(row.kind != Kind::Choice ||
                       static_cast<int>(row.choices.size()) == Config::Maximum(row.option) + 1,
                   "settings: {}'s choices do not match its option", row.title);
        rows.push_back(std::move(row));
    };

    // Display: what is on the screen and where.
    add({Option::ScreenLayout,
         Category::Display,
         Kind::Choice,
         "Screen layout",
         "How the two screens are arranged. Overlay: one screen fills the display and the other "
         "sits small and see-through in the bottom-right corner, where you can still touch it. "
         "Large overlay: the same with a bigger corner screen.",
         {"Default", "Single screen", "Large screen", "Side by side", "Hybrid", "Overlay",
          "Large overlay"},
         0,
         nullptr,
         nullptr});
    add({Option::SwapScreens,
         Category::Display,
         Kind::Toggle,
         "Swap screens",
         "Puts the touch screen where the top screen would be.",
         {},
         0,
         nullptr,
         nullptr});
    add({Option::Widescreen,
         Category::Display,
         Kind::Choice,
         "Widescreen",
         "Shows more of the scene at the top screen's sides instead of stretching it, in the "
         "Single screen and overlay layouts with Swap screens off. It can cause glitches: some "
         "games leave the new edges empty or draw them wrong.",
         {},
         0,
         nullptr,
         nullptr});
    add({Option::OverlayOpacity,
         Category::Display,
         Kind::Slider,
         "Overlay opacity",
         "How opaque the small screen of the overlay layouts is.",
         {},
         5,
         "%",
         nullptr});
    add({Option::TopScale,
         Category::Display,
         Kind::Choice,
         "Top screen resolution",
         "Draws the top screen at a higher resolution: sharper in the Single screen and Large "
         "screen layouts, but much more GPU work (about 2.25 times at 1.5x), so slow games get "
         "slower. Can crash games that use a lot of video memory.",
         {"Native", "1.25x", "1.5x"},
         0,
         nullptr,
         nullptr});
    add({Option::Msaa,
         Category::Display,
         Kind::Toggle,
         "Anti-aliasing (2x MSAA)",
         "Smooths jagged edges on the top screen. It costs GPU time, so slow games get slower, and "
         "the top screen stays at Native resolution while it is on. Can crash games that use a lot "
         "of video memory.",
         {},
         0,
         nullptr,
         nullptr});

    // Speed: how fast and how smoothly.
    add({Option::Vsync,
         Category::Speed,
         Kind::Toggle,
         "Vsync",
         "Shows each picture whole at the display's refresh and holds the game to the display's "
         "rate. Off: pictures show at once and may tear, and Speed limit sets the pace.",
         {},
         0,
         nullptr,
         nullptr});
    add({Option::SpeedLimit,
         Category::Speed,
         Kind::Slider,
         "Speed limit",
         "How fast the game may run, in percent of full speed. No limit runs it as fast as the "
         "console can. Lower it to save battery.",
         {},
         5,
         "%",
         "No limit"});
    add({Option::FrameSkipLimit,
         Category::Speed,
         Kind::Choice,
         "Frame skip limit",
         "When the GPU falls behind, the busier screen skips frames so the game keeps its speed. "
         "This sets how many frames in a row it may skip: higher keeps heavy games faster but less "
         "smooth. Off draws every frame and the game slows down instead.",
         {"Off", "1 frame", "2 frames", "3 frames", "4 frames", "No limit"},
         0,
         nullptr,
         nullptr});
    add({Option::WaitForGpu,
         Category::Speed,
         Kind::Toggle,
         "Wait for GPU results",
         "The game waits for the GPU to finish a picture before reading it, as on a 3DS. Off can "
         "run faster, but may cause flickering, outdated or missing pictures and broken effects.",
         {},
         0,
         nullptr,
         nullptr});
    add({Option::SkipRightEye,
         Category::Speed,
         Kind::Toggle,
         Translatable("Skip the 3D right eye"),
         Translatable("Games made for 3D draw each picture twice, once for each eye, and the Vita "
                      "shows one. On leaves the right eye's picture undrawn, which is faster. Turn "
                      "it off for a game whose top screen stays grey, black or still."),
         {},
         0,
         nullptr,
         nullptr});

    // Sound.
    add({Option::StretchAudio,
         Category::Sound,
         Kind::Toggle,
         "Stretch audio",
         "Below full speed, slows the sound down with the game instead of letting it break up.",
         {},
         0,
         nullptr,
         nullptr});

    // System: the console and the menus.
    add({Option::Theme,
         Category::System,
         Kind::Choice,
         "Menu theme",
         "The menus' colours: dark grey, or the light grey of the 3DS HOME Menu.",
         {"Dark", "Light"},
         0,
         nullptr,
         nullptr});
    add({Option::Region,
         Category::System,
         Kind::Choice,
         "Region",
         "The console region reported to the game. Auto picks one the game accepts.",
         {"Auto", "Japan", "USA", "Europe", "Australia", "China", "Korea", "Taiwan"},
         0,
         nullptr,
         nullptr});
    add({Option::StartupNotice,
         Category::System,
         Kind::Toggle,
         "Startup notice",
         "Shows a notice as Azahar starts, while the quick menu, notifications and plugin menus "
         "such as PSVshell's still work. Off: Azahar takes over the GPU at once.",
         {},
         0,
         nullptr,
         nullptr});
}

bool Ui::BackPressed() const {
    // ImGui has already handled the press by the time this is asked, so a cancel that left a
    // child must not also close the overlay. It only counts when the overlay itself held focus
    // at the end of the previous frame.
    //
    // This version of ImGui has no key enum for a pad button; navigation arrives as the
    // NavInputs array, and a duration of exactly zero is the frame the press landed on.
    const ImGuiIO& io = ImGui::GetIO();
    const bool cancel_pressed = io.NavInputsDownDuration[ImGuiNavInput_Cancel] == 0.0f;
    return overlay_focused && cancel_pressed;
}

void Ui::SetGame(const std::string& path) {
    game_key = Config::RunningTitle();
    // The list's name for it: the title's own where the list read one.
    const auto listed = std::find_if(games.begin(), games.end(),
                                     [&](const GameEntry& game) { return game.path == path; });
    game_name = listed != games.end() ? listed->name : std::string{FileUtil::GetFilename(path)};
}

void Ui::EnterGame() {
    state = State::InGame;
    window.SetInputToGuest(true);
}

void Ui::LeaveGame() {
    state = State::GameList;
    overlay_focused = false;
    focus_first_row = true;
    game_key.clear();
    game_name.clear();
}

void Ui::OpenPauseMenu() {
    state = State::Pause;
    focus_first_row = true;
    window.SetInputToGuest(false);
}

void Ui::SetStatusMessage(std::string message) {
    status_message = std::move(message);
}

void Ui::SetProgress(float fraction) {
    status_progress = std::clamp(fraction, 0.0f, 1.0f);
}

void Ui::ClearStatusMessage() {
    status_message.clear();
    status_progress = -1.0f;
}

void Ui::OpenSettings(std::string title, std::string name, State back, u64 program_id) {
    settings_title = std::move(title);
    settings_name = std::move(name);
    settings_back = back;
    // The title's built-in patches; the running title's as its load left them (a loader that
    // applies none: none).
    settings_patches = Loader::TitlePatchesOf(program_id);
    settings_patches_in.clear();
    if (!settings_title.empty() && settings_title == game_key) {
        settings_patches_in = system.GetAppLoader().TitlePatchesIn();
        if (settings_patches_in.empty()) {
            settings_patches.clear();
        }
        ASSERT_MSG(settings_patches_in.size() == settings_patches.size(),
                   "settings: {} patches built in for {:016X}, {} applied or left out",
                   settings_patches.size(), program_id, settings_patches_in.size());
    }
    // The title's cheats, its file's as the cheat engine reads it: the running title's are the
    // engine's already.
    settings_program_id = settings_title.empty() ? 0 : program_id;
    settings_cheats.clear();
    if (settings_program_id != 0) {
        Cheats::CheatEngine& engine = system.CheatEngine();
        engine.LoadCheatFile(settings_program_id);
        const auto cheats = engine.GetCheats();
        settings_cheats.assign(cheats.begin(), cheats.end());
    }
    // What of it is installed: from the game list only, never under a running title.
    settings_installed.clear();
    if (back == State::GameList && settings_program_id != 0) {
        const u64 low = settings_program_id & 0xFFFFFFFF;
        const bool application = settings_program_id >> 32 == 0x00040000;
        const std::pair<const char*, u64> parts[] = {
            {"Installed game", settings_program_id},
            {"Update", application ? 0x0004000Eull << 32 | low : 0},
            {"Downloadable content", application ? 0x0004008Cull << 32 | low : 0},
        };
        for (const auto& [kind, title_id] : parts) {
            const std::string content =
                Service::AM::GetTitlePath(Service::FS::MediaType::SDMC, title_id) + "content";
            FileUtil::FSTEntry tree;
            if (title_id != 0 && FileUtil::ScanDirectoryTree(content, tree, 2) != 0) {
                settings_installed.push_back({kind, title_id, TreeSize(tree)});
            }
        }
    }
    category = SettingsCategories().front();
    focused_row = -1;
    focused_patch = -1;
    focused_cheat = -1;
    focused_installed = armed_installed = -1;
    focus_first_row = true;
    state = State::Settings;
    overlay_focused = false;
}

std::vector<Ui::Category> Ui::SettingsCategories() const {
    std::vector<Category> shown;
    for (int c = 0; c < static_cast<int>(Category::Count); c++) {
        const auto tab = static_cast<Category>(c);
        // Patches and Cheats even with none: the engine settings are looked for when the tab
        // opens, and the Cheats tab says where the title's cheat file goes.
        const bool has_rows =
            tab == Category::Patches  ? !settings_patches.empty()
            : tab == Category::Cheats ? settings_program_id != 0
            : tab == Category::Installed
                ? !settings_installed.empty()
                : std::any_of(rows.begin(), rows.end(), [&](const Row& row) {
                      return row.category == tab &&
                             (settings_title.empty() || Config::PerTitle(row.option));
                  });
        if (has_rows) {
            shown.push_back(tab);
        }
    }
    return shown;
}

int Ui::RowValue(const Row& row) const {
    return settings_title.empty() ? Config::Global(row.option)
                                  : Config::ForTitle(settings_title, row.option);
}

void Ui::SetRowValue(const Row& row, int value) {
    if (settings_title.empty()) {
        Config::SetGlobal(row.option, value);
    } else {
        Config::SetTitleValue(settings_title, row.option, value);
    }
}

void Ui::StepRow(const Row& row, int delta) {
    const int value = RowValue(row);
    switch (row.kind) {
    case Row::Kind::Toggle:
        SetRowValue(row, value != 0 ? 0 : 1);
        break;
    case Row::Kind::Choice: {
        const int count = static_cast<int>(row.choices.size());
        SetRowValue(row, ((value + delta) % count + count) % count);
        break;
    }
    case Row::Kind::Slider:
        SetRowValue(row, std::clamp(value + delta * row.step, Config::Minimum(row.option),
                                    Config::Maximum(row.option)));
        break;
    }
}

const char* Ui::DisabledReason(const Row& row) const {
    // The other option's value where this row's is edited: all games' or the title's.
    const auto other = [this](Config::Option option) {
        return settings_title.empty() ? Config::Global(option)
                                      : Config::ForTitle(settings_title, option);
    };
    if (row.option == Config::Option::SpeedLimit && other(Config::Option::Vsync) != 0) {
        return "Off while Vsync is on. Turn Vsync off to set a speed limit.";
    }
    if (row.option == Config::Option::TopScale && other(Config::Option::Msaa) != 0) {
        return "Native while Anti-aliasing is on. Turn Anti-aliasing off to choose a resolution.";
    }
    return nullptr;
}

std::string Ui::ValueText(const Row& row, int value) const {
    switch (row.kind) {
    case Row::Kind::Toggle:
        return value != 0 ? "On" : "Off";
    case Row::Kind::Choice:
        return row.choices[static_cast<std::size_t>(value)];
    case Row::Kind::Slider:
        if (value == 0 && row.zero != nullptr) {
            return row.zero;
        }
        return fmt::format("{}{}", value, row.unit);
    }
    UNREACHABLE();
}

void Ui::DrawRow(const Row& row, std::size_t index) {
    constexpr float Pad = 18.0f;
    constexpr float SwitchWidth = 56.0f;
    constexpr float BarWidth = 150.0f;
    constexpr float Arrow = 10.0f;

    ImGui::PushID(static_cast<int>(index));
    const bool for_title = !settings_title.empty();
    const int value = RowValue(row);
    const std::string text = ValueText(row, value);
    const char* const disabled = DisabledReason(row);
    // The title edited sets this itself; or, on all games' values, the running title does, and a
    // change here does not reach it. A game sets every option itself once it sets one
    // (Config::SetTitleValue): the amber marks those where its value is not all games'.
    const std::optional<int> own_value =
        for_title ? Config::TitleValue(settings_title, row.option)
                  : (game_key.empty() ? std::nullopt : Config::TitleValue(game_key, row.option));
    const bool own = own_value.has_value();
    const bool marked = own && *own_value != Config::Global(row.option);

    // The value's room at the right: a switch, a choice's widest label between its arrows, a
    // slider's bar and its widest number. The label wraps in what is left.
    float value_width = SwitchWidth;
    float number_width = 0.0f;
    if (row.kind == Row::Kind::Choice) {
        value_width = 0.0f;
        for (const char* choice : row.choices) {
            value_width = std::max(value_width, ImGui::CalcTextSize(choice).x);
        }
        value_width += 2.0f * (Arrow + 14.0f);
    } else if (row.kind == Row::Kind::Slider) {
        number_width =
            ImGui::CalcTextSize(fmt::format("{}{}", Config::Maximum(row.option), row.unit).c_str())
                .x;
        if (row.zero != nullptr) {
            number_width = std::max(number_width, ImGui::CalcTextSize(row.zero).x);
        }
        value_width = BarWidth + 14.0f + number_width;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const float label_width = width - value_width - 3.0f * Pad;
    const ImVec2 label = ImGui::CalcTextSize(row.title, nullptr, false, label_width);
    const Card card = CardItem("##row", ImVec2(width, std::max(RowHeight, label.y + 28.0f)));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    const bool focused = ImGui::GetFocusID() == card.id;
    if (focused) {
        focused_row = static_cast<int>(index);
    }
    const ImRect value_bb(card.bb.Max.x - Pad - value_width, card.bb.Min.y, card.bb.Max.x - Pad,
                          card.bb.Max.y);

    if (disabled == nullptr) {
        int delta = 0;
        if (focused && ImGui::IsNavInputTest(ImGuiNavInput_DpadLeft, ImGuiInputReadMode_Repeat)) {
            delta = -1;
        } else if (focused &&
                   ImGui::IsNavInputTest(ImGuiNavInput_DpadRight, ImGuiInputReadMode_Repeat)) {
            delta = 1;
        }
        if (card.pressed) {
            const float x = ImGui::GetIO().MousePos.x;
            const bool on_value = !card.by_pad && x >= value_bb.Min.x;
            if (on_value && row.kind == Row::Kind::Slider) {
                // A touch on the bar sets the value where it lands, in whole steps.
                const float t = std::clamp((x - value_bb.Min.x) / BarWidth, 0.0f, 1.0f);
                const int low = Config::Minimum(row.option);
                const int steps = static_cast<int>(
                    std::lround(t * static_cast<float>(Config::Maximum(row.option) - low) /
                                static_cast<float>(row.step)));
                SetRowValue(row, low + steps * row.step);
            } else if (on_value && row.kind == Row::Kind::Choice) {
                delta = x < value_bb.GetCenter().x ? -1 : 1;
            } else if (row.kind != Row::Kind::Slider) {
                // Cross, or a touch on the label: a toggle flips, a choice goes on.
                delta = 1;
            }
        }
        if (delta != 0) {
            StepRow(row, delta);
        }
    }
    // Triangle: the title's own value goes, all games' applies again.
    if (focused && for_title && own &&
        ImGui::IsNavInputTest(ImGuiNavInput_Input, ImGuiInputReadMode_Pressed)) {
        Config::SetTitleValue(settings_title, row.option, std::nullopt);
    }

    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float alpha = disabled != nullptr ? 0.4f : 1.0f;
        const float mid = card.bb.GetCenter().y;
        if (marked) {
            // A title's own value, not all games': an amber mark at the row's left edge.
            dl->AddRectFilled(ImVec2(card.bb.Min.x + 6.0f, card.bb.Min.y + 14.0f),
                              ImVec2(card.bb.Min.x + 10.0f, card.bb.Max.y - 14.0f),
                              Colour(palette->Own), 2.0f);
        }
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text, alpha),
                    row.title, nullptr, label_width);
        // On a title's screen its own value is amber where it is not all games', else plain.
        const ImU32 value_colour =
            Colour(for_title && marked ? palette->Own : palette->Text, alpha);
        switch (row.kind) {
        case Row::Kind::Toggle: {
            const float knob = DrawSwitch(dl, value_bb.Max.x, mid, value != 0, alpha);
            if (for_title && marked) {
                dl->AddCircle(ImVec2(knob, mid), 11.0f, Colour(palette->Own, alpha), 16, 2.0f);
            }
            break;
        }
        case Row::Kind::Choice: {
            const ImU32 arrows = Colour(focused ? palette->Accent : palette->TextDim, alpha);
            dl->AddTriangleFilled(ImVec2(value_bb.Min.x, mid),
                                  ImVec2(value_bb.Min.x + Arrow, mid - Arrow),
                                  ImVec2(value_bb.Min.x + Arrow, mid + Arrow), arrows);
            dl->AddTriangleFilled(ImVec2(value_bb.Max.x, mid),
                                  ImVec2(value_bb.Max.x - Arrow, mid - Arrow),
                                  ImVec2(value_bb.Max.x - Arrow, mid + Arrow), arrows);
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            dl->AddText(ImVec2(value_bb.GetCenter().x - size.x * 0.5f, mid - size.y * 0.5f),
                        value_colour, text.c_str());
            break;
        }
        case Row::Kind::Slider: {
            const int low = Config::Minimum(row.option);
            const float t = static_cast<float>(value - low) /
                            static_cast<float>(Config::Maximum(row.option) - low);
            const ImVec2 min(value_bb.Min.x, mid - 4.0f);
            dl->AddRectFilled(min, ImVec2(min.x + BarWidth, mid + 4.0f),
                              Colour(palette->SwitchOff, alpha), 4.0f);
            dl->AddRectFilled(min, ImVec2(min.x + BarWidth * t, mid + 4.0f),
                              Colour(palette->Accent, alpha), 4.0f);
            dl->AddCircleFilled(ImVec2(min.x + BarWidth * t, mid), 10.0f,
                                Colour(palette->Text, alpha), 16);
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            dl->AddText(ImVec2(value_bb.Max.x - size.x, mid - size.y * 0.5f), value_colour,
                        text.c_str());
            break;
        }
        }
    }
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
        Note(palette->TextDim,
             "Applied when the game starts, if your copy is the version it was made for.");
    } else if (settings_patches_in[index]) {
        Note(palette->Accent, "Applied to the running game.");
    } else {
        Note(palette->TextDim,
             "Left out: your copy is a different version from the one it was made for.");
    }
    Note(palette->TextDim, "Built into Azahar for this game. It can't be turned off.");
}

void Ui::DrawCheatRow(std::size_t index) {
    constexpr float Pad = 18.0f;
    constexpr float SwitchWidth = 56.0f;

    ImGui::PushID(static_cast<int>(index));
    Cheats::CheatBase& cheat = *settings_cheats[index];
    const std::string name = cheat.GetName();
    const float width = ImGui::GetContentRegionAvail().x;
    const float label_width = width - SwitchWidth - 3.0f * Pad;
    const ImVec2 label = ImGui::CalcTextSize(name.c_str(), nullptr, false, label_width);
    const Card card = CardItem("##cheat", ImVec2(width, std::max(RowHeight, label.y + 28.0f)));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    const bool focused = ImGui::GetFocusID() == card.id;
    if (focused) {
        focused_cheat = static_cast<int>(index);
    }
    // The running title's engine reads the switch at its next run (the guest waits while the
    // menu is open); the file keeps it for the title's next start.
    if (card.pressed ||
        (focused && (ImGui::IsNavInputTest(ImGuiNavInput_DpadLeft, ImGuiInputReadMode_Pressed) ||
                     ImGui::IsNavInputTest(ImGuiNavInput_DpadRight, ImGuiInputReadMode_Pressed)))) {
        cheat.SetEnabled(!cheat.IsEnabled());
        system.CheatEngine().SaveCheatFile(settings_program_id);
        if (!game_key.empty()) {
            CrashReport::NoteCheats(&system.CheatEngine());
        }
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float mid = card.bb.GetCenter().y;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text),
                    name.c_str(), nullptr, label_width);
        DrawSwitch(dl, card.bb.Max.x - Pad, mid, cheat.IsEnabled(), 1.0f);
    }
    ImGui::PopID();
}

void Ui::DrawCheatInfo() {
    const std::string file = fmt::format(
        "{}{:016X}.txt", FileUtil::GetUserPath(FileUtil::UserPath::CheatsDir), settings_program_id);
    if (settings_cheats.empty()) {
        ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
        ImGui::TextWrapped("No cheats");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        Note(palette->TextDim,
             fmt::format("Put this game's Gateway codes in {}: a [name] line for each cheat, then "
                         "its code lines. Reopen this screen to see them.",
                         file));
        return;
    }
    if (focused_cheat < 0) {
        return;
    }
    const Cheats::CheatBase& cheat = *settings_cheats[static_cast<std::size_t>(focused_cheat)];

    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", cheat.GetName().c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    if (const std::string notes = cheat.GetComments(); !notes.empty()) {
        ImGui::TextWrapped("%s", notes.c_str());
        ImGui::Spacing();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, palette->TextDim);
    ImGui::TextUnformatted(cheat.GetCode().c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const bool running = !settings_title.empty() && settings_title == game_key;
    if (cheat.IsEnabled()) {
        Note(palette->Accent, running ? "On: active now." : "On: applied while the game runs.");
    } else {
        Note(palette->TextDim, "Off.");
    }
    Note(palette->TextDim, fmt::format("From {}.", file));
}

void Ui::DrawInstalledRow(std::size_t index) {
    constexpr float Pad = 18.0f;

    ImGui::PushID(static_cast<int>(index));
    const InstalledPart part = settings_installed[index];
    const bool armed = armed_installed == static_cast<int>(index);
    const std::string size_text =
        armed ? std::string{"Cross again to remove"}
              : fmt::format("{} MiB", (part.size + 1024 * 1024 - 1) / (1024 * 1024));
    const float width = ImGui::GetContentRegionAvail().x;
    const Card card = CardItem("##installed", ImVec2(width, RowHeight));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    if (ImGui::GetFocusID() == card.id) {
        focused_installed = static_cast<int>(index);
    } else if (armed) {
        // Moving off a row disarms it.
        armed_installed = -1;
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float mid = card.bb.GetCenter().y;
        const ImVec2 label = ImGui::CalcTextSize(part.kind);
        const ImVec2 size_size = ImGui::CalcTextSize(size_text.c_str());
        dl->AddText(ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text),
                    part.kind);
        dl->AddText(ImVec2(card.bb.Max.x - Pad - size_size.x, mid - size_size.y * 0.5f),
                    Colour(armed ? palette->Accent : palette->TextDim), size_text.c_str());
    }
    ImGui::PopID();
    if (!card.pressed) {
        return;
    }
    if (!armed) {
        armed_installed = static_cast<int>(index);
        return;
    }
    const Result result =
        Service::AM::UninstallProgram(Service::FS::MediaType::SDMC, part.title_id);
    LOG_INFO(Frontend, "Removed {} {:016X}: {:08X}", part.kind, part.title_id, result.raw);
    settings_installed.erase(settings_installed.begin() + static_cast<std::ptrdiff_t>(index));
    focused_installed = armed_installed = -1;
    focus_first_row = true;
    ScanGames();
    // The installed game whose screen this is has left the list: back to it.
    if (part.title_id == settings_program_id &&
        settings_title == fmt::format("{:016x}", part.title_id)) {
        state = settings_back;
        overlay_focused = false;
        return;
    }
    if (settings_installed.empty()) {
        category = SettingsCategories().front();
    }
}

void Ui::DrawInstalledInfo() {
    if (focused_installed < 0) {
        return;
    }
    const InstalledPart& part = settings_installed[static_cast<std::size_t>(focused_installed)];
    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", part.kind);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    const u32 high = static_cast<u32>(part.title_id >> 32);
    ImGui::TextWrapped(
        "%s", high == 0x0004000E
                  ? "Installed from a CIA. Removing it takes the game back to its own version."
              : high == 0x0004008C
                  ? "Installed from a CIA. Removing it takes this content out of the game."
                  : "Installed from a CIA. Removing it takes the game off the list.");
    ImGui::Spacing();
    Note(palette->TextDim, "Removing it keeps the game's save data.");
    Note(palette->TextDim, fmt::format("Title {:016X}.", part.title_id));
}

void Ui::DrawRomFolderRows() {
    constexpr float Pad = 18.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const std::vector<std::string> folders = Config::RomFolders();
    std::optional<std::size_t> remove;
    for (std::size_t i = 0; i < folders.size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        const bool armed = armed_rom_folder == static_cast<int>(i);
        const Card card = CardItem("##rom_folder", ImVec2(width, RowHeight));
        if (ImGui::GetFocusID() == card.id) {
            focused_row = RomFolderRow;
            focused_rom_folder = static_cast<int>(i);
        } else if (armed) {
            // Moving off a row disarms it.
            armed_rom_folder = -1;
        }
        if (card.visible) {
            ImDrawList* const dl = ImGui::GetWindowDrawList();
            const float y = card.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
            const char* const label = Tr("ROM folder");
            const float label_width = ImGui::CalcTextSize(label).x;
            dl->AddText(ImVec2(card.bb.Min.x + Pad, y), Colour(palette->Text), label);
            const std::string value = armed ? std::string{Tr("Cross again to remove")}
                                            : Fit(folders[i], width - 3.0f * Pad - label_width);
            const float value_width = ImGui::CalcTextSize(value.c_str()).x;
            dl->AddText(ImVec2(card.bb.Max.x - Pad - value_width, y),
                        Colour(armed ? palette->Accent : palette->Text), value.c_str());
        }
        if (card.pressed) {
            if (armed) {
                remove = i;
            } else {
                armed_rom_folder = static_cast<int>(i);
            }
        }
        ImGui::PopID();
    }
    const Card add = CardItem("##add_rom_folder", ImVec2(width, RowHeight));
    if (ImGui::GetFocusID() == add.id) {
        focused_row = AddRomFolderRow;
    }
    if (add.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float y = add.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(add.bb.Min.x + Pad, y), Colour(palette->Text), Tr("Add ROM folder"));
    }
    if (remove) {
        // The folder's files stay where they are: the game list leaves them out.
        LOG_INFO(Frontend, "ROM folder {} removed", folders[*remove]);
        Config::RemoveRomFolder(*remove);
        Config::Save();
        focused_rom_folder = armed_rom_folder = -1;
        focus_first_row = true;
        ScanGames();
    } else if (add.pressed) {
        OpenRomFolderPicker();
    }
}

void Ui::DrawRomFolderInfo() {
    const bool add = focused_row == AddRomFolderRow;
    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", add ? Tr("Add ROM folder") : Tr("ROM folder"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", Tr("The game list also shows the titles in these folders, besides "
                                "ux0:data/azahar/roms: on a USB or SD2Vita card (uma0:), for "
                                "example. Only the files in the folder itself, not in folders "
                                "inside it."));
    ImGui::Spacing();
    if (add) {
        Note(palette->TextDim, Tr("Open the folder, then press square to add it."));
        return;
    }
    const std::vector<std::string> folders = Config::RomFolders();
    if (focused_rom_folder >= 0 && static_cast<std::size_t>(focused_rom_folder) < folders.size()) {
        Note(palette->TextDim, folders[static_cast<std::size_t>(focused_rom_folder)]);
        Note(palette->TextDim, Tr("Removing it from the list keeps its files."));
    }
}

void Ui::DrawRowInfo() {
    if (focused_row < 0) {
        return;
    }
    const Row& row = rows[static_cast<std::size_t>(focused_row)];
    const bool for_title = !settings_title.empty();
    const std::optional<int> own =
        Config::TitleValue(for_title ? settings_title : game_key, row.option);

    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", row.title);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", row.description);
    ImGui::Spacing();

    if (const char* reason = DisabledReason(row)) {
        Note(palette->Accent, reason);
    }
    const int all_games = Config::Global(row.option);
    if (for_title && own && *own != all_games) {
        Note(palette->Own,
             fmt::format("Set for this game. All games: {}.", ValueText(row, all_games)));
    } else if (for_title && own) {
        Note(palette->TextDim, "Set for this game, the same as all games for now. Changing it for "
                               "all games won't change it here.");
    } else if (for_title) {
        Note(palette->TextDim, "Follows the setting for all games.");
    } else if (!game_key.empty() && own) {
        Note(*own != all_games ? palette->Own : palette->TextDim,
             fmt::format("{} sets its own: {}. Change it in Game settings.", game_name,
                         ValueText(row, *own)));
    }
    if (const std::optional<int> pin = Config::PinOf(row.option)) {
        Note(palette->Accent, fmt::format("A launch argument holds this at {} until Azahar exits.",
                                          ValueText(row, *pin)));
    }
    if (!Config::Runtime(row.option) && !game_key.empty()) {
        Note(palette->Accent, "Takes effect when a game starts.");
    }
}

Ui::Action Ui::DrawSettings() {
    const std::vector<Category> categories = SettingsCategories();
    const bool for_title = !settings_title.empty();
    const auto current = std::find(categories.begin(), categories.end(), category);
    ASSERT_MSG(current != categories.end(), "settings: tab {} is not this screen's",
               static_cast<int>(category));

    // L and R: the tab before and after, around.
    const ImGuiIO& io = ImGui::GetIO();
    int tab_step = 0;
    if (io.NavInputsDownDuration[ImGuiNavInput_FocusPrev] == 0.0f) {
        tab_step = -1;
    } else if (io.NavInputsDownDuration[ImGuiNavInput_FocusNext] == 0.0f) {
        tab_step = 1;
    }
    std::optional<Category> next_category;
    if (tab_step != 0) {
        const int count = static_cast<int>(categories.size());
        const int at = static_cast<int>(current - categories.begin());
        next_category =
            categories[static_cast<std::size_t>(((at + tab_step) % count + count) % count)];
    }

    BeginBackdrop("##settings_backdrop", for_title ? Icon::Cartridge : Icon::Gear,
                  for_title ? "Game settings" : "Settings",
                  for_title ? settings_name : std::string{"All games"});
    // The Patches tab changes nothing; the Cheats tab's rows are switches alone, the Installed
    // tab's are removed.
    const bool option_rows = category != Category::Patches && category != Category::Cheats &&
                             category != Category::Installed;
    bool back = FooterButton(Glyph::Circle, "Back");
    if (option_rows) {
        FooterButton(Glyph::LeftRight, "Change");
    } else if (category == Category::Cheats && !settings_cheats.empty()) {
        FooterButton(Glyph::Cross, "On or off");
    } else if (category == Category::Installed) {
        FooterButton(Glyph::Cross, "Remove");
    }
    if (categories.size() > 1 && FooterButton(Glyph::Shoulders, "Tab") && !next_category) {
        const int at = static_cast<int>(current - categories.begin());
        next_category = categories[static_cast<std::size_t>(at + 1) % categories.size()];
    }
    if (for_title && option_rows && FooterButton(Glyph::Triangle, "Use all games' setting") &&
        focused_row >= 0) {
        Config::SetTitleValue(settings_title, rows[static_cast<std::size_t>(focused_row)].option,
                              std::nullopt);
    }

    // The tabs, under the header: the selected one filled with the accent colour.
    {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        float x = Margin;
        const float y = HeaderHeight + 10.0f;
        for (const Category c : categories) {
            const int i = static_cast<int>(c);
            const char* name = CategoryName(i);
            const ImVec2 text = ImGui::CalcTextSize(name);
            const ImVec2 size(text.x + 64.0f, TabsHeight);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", size)) {
                next_category = c;
            }
            ImGui::PopID();
            const bool selected = c == category;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x, y) + size,
                              Colour(selected ? palette->Accent : palette->Row), size.y * 0.5f);
            const ImU32 ink = Colour(selected ? palette->Backdrop : palette->Text);
            DrawIcon(dl, CategoryIcon(i), ImVec2(x + 26.0f, y + size.y * 0.5f), 22.0f, ink,
                     Colour(selected ? palette->Accent : palette->Row));
            dl->AddText(ImVec2(x + 46.0f, y + (size.y - text.y) * 0.5f), ink, name);
            x += size.x + 10.0f;
        }
    }
    EndBackdrop();

    if (next_category && *next_category != category) {
        category = *next_category;
        focused_row = -1;
        focused_patch = -1;
        focused_cheat = -1;
        focused_installed = armed_installed = -1;
        focused_rom_folder = armed_rom_folder = -1;
        focus_first_row = true;
    }
    if (category == Category::Patches) {
        StartEngineSearch();
    }

    // The rows on the left, the selected one's description on the right.
    const float top = HeaderHeight + 10.0f + TabsHeight + 10.0f;
    const float bottom = ScreenHeight - FooterHeight - 8.0f;
    const float split = 560.0f;
    BeginList("##settings_rows", ImVec2(Margin - 4.0f, top - 4.0f), ImVec2(split - 8.0f, bottom));
    if (category == Category::Patches) {
        for (std::size_t i = 0; i < settings_patches.size(); i++) {
            DrawPatchRow(i);
        }
    } else if (category == Category::Cheats) {
        for (std::size_t i = 0; i < settings_cheats.size(); i++) {
            DrawCheatRow(i);
        }
    } else if (category == Category::Installed) {
        for (std::size_t i = 0; i < settings_installed.size() && state == State::Settings; i++) {
            DrawInstalledRow(i);
        }
    } else {
        for (std::size_t i = 0; i < rows.size(); i++) {
            const Row& row = rows[i];
            if (row.category == category && (!for_title || Config::PerTitle(row.option))) {
                DrawRow(row, i);
            }
        }
    }
    back |= BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(split, top));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth - Margin - split, bottom - top));
    ImGui::Begin("##settings_info", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav);
    if (category == Category::Patches) {
        DrawPatchInfo();
    } else if (category == Category::Cheats) {
        DrawCheatInfo();
    } else if (category == Category::Installed) {
        DrawInstalledInfo();
    } else {
        DrawRowInfo();
    }
    ImGui::End();

    if (back) {
        // What changed is kept now, not at the title's end: a crash or a power-off would lose it.
        Config::Save();
        state = settings_back;
        overlay_focused = false;
        focus_first_row = true;
    }
    return Action::None;
}

/**
 * The game list's layout: the 3DS HOME Menu's on one screen. The status bar; the selected
 * title's banner in the middle of the panel where the 3DS has the top screen; the titles as a
 * grid of two rows of tiles that scrolls sideways, the selected title's name and publisher in a
 * speech bubble over the other row; the footer, with the pad's buttons at its left and the tools
 * at its right. The grid sits close over the footer, which leaves the banner the rest.
 */
namespace ListLayout {
constexpr float StatusHeight = 24.0f;
constexpr float Footer = ScreenHeight - FooterHeight;
// 80 a side: the banner's panel 276 rows for its 240.
constexpr float Tile = 80.0f;
constexpr float RowPitch = Tile + 8.0f;
constexpr float Pitch = Tile + 10.0f;
constexpr int Rows = 2;
constexpr float RowTop = Footer - 16.0f - Tile - static_cast<float>(Rows - 1) * RowPitch;
// Room for the selected tile's brackets.
constexpr float BannerBottom = RowTop - 16.0f;
constexpr float TileRounding = 16.0f;
/// The banner: the 3DS top screen's 400x240 at 1.15x (460x276), in the panel's middle: the
/// panel's 276 rows, the whole banner shown.
constexpr float BannerWidth = 460.0f;
constexpr float BannerHeight = 276.0f;
constexpr float BannerLeft = (ScreenWidth - BannerWidth) * 0.5f;
constexpr float BannerTop = StatusHeight + (BannerBottom - StatusHeight - BannerHeight) * 0.5f;
static_assert(BannerTop >= 0.0f, "the banner starts on the display");
} // namespace ListLayout

Common::Rectangle<u32> Ui::BannerRect() {
    using namespace ListLayout;
    const u32 left = static_cast<u32>(BannerLeft), top = static_cast<u32>(BannerTop);
    return {left, top, left + static_cast<u32>(BannerWidth), top + static_cast<u32>(BannerHeight)};
}

u32 Ui::BannerBackdrop() {
    const ImVec4& c = palette->Backdrop;
    const auto byte = [](float v) { return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return byte(c.x) << 16 | byte(c.y) << 8 | byte(c.z);
}

bool Ui::SelectTitle(const std::string& text) {
    const auto lower = [](std::string v) {
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return v;
    };
    const std::string want = lower(text);
    for (int i = 0; i < static_cast<int>(games.size()); i++) {
        if (lower(games[i].name).find(want) != std::string::npos ||
            lower(games[i].path).find(want) != std::string::npos) {
            // The grid moves its focus there at its next frame, as its page arrows do.
            selected_game = i;
            carousel_focus = i;
            tools_focused = false;
            return true;
        }
    }
    return false;
}

std::string Ui::BannerTitle() const {
    if (state != State::GameList || tools_focused || selected_game < 0 ||
        selected_game >= static_cast<int>(games.size())) {
        return {};
    }
    return games[selected_game].path;
}

Ui::Action Ui::DrawGameList() {
    using namespace ListLayout;
    // The banner (BannerPlayer) shows under the interface, in a hole left in the panel.
    const bool banner = window.MenuBannerShown();
    const ImRect hole(ImVec2(BannerLeft, StatusHeight),
                      ImVec2(BannerLeft + BannerWidth, BannerBottom));
    Action action = Action::None;
    if (!games.empty()) {
        selected_game = std::clamp(selected_game, 0, static_cast<int>(games.size()) - 1);
    }
    ImGuiIO& io = ImGui::GetIO();
    const ImTextureID atlas = io.Fonts->TexID;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth, ScreenHeight));
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          banner ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f) : palette->Backdrop);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##home_backdrop", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        // The backdrop and the status bar span the display. The window's draw list clips half its
        // padding in from each side (ImGui's inner clip rectangle), which the renderer cuts
        // exactly: with the banner showing (the window's own background transparent) those 8
        // pixels each side would stay black.
        dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, ScreenHeight), false);
        if (banner) {
            const ImU32 c = Colour(palette->Backdrop);
            dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, hole.Min.y), c);
            dl->AddRectFilled(ImVec2(0.0f, hole.Max.y), ImVec2(ScreenWidth, ScreenHeight), c);
            dl->AddRectFilled(ImVec2(0.0f, hole.Min.y), ImVec2(hole.Min.x, hole.Max.y), c);
            dl->AddRectFilled(ImVec2(hole.Max.x, hole.Min.y), ImVec2(ScreenWidth, hole.Max.y), c);
        }
        DrawStatusBar(dl, StatusHeight,
                      games.empty() ? std::string{}
                                    : fmt::format("{} {}", games.size(),
                                                  games.size() == 1 ? "title" : "titles"));
        dl->PopClipRect();

        if (games.empty()) {
            const std::string text = fmt::format(
                "No titles found. Copy .3ds or .cci files into {}, or choose Install CIA.",
                GameDirectory);
            const float width = ScreenWidth - 2.0f * (Margin + 24.0f);
            const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, width);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                        ImVec2((ScreenWidth - size.x) * 0.5f,
                               (StatusHeight + BannerBottom - size.y) * 0.5f),
                        Colour(palette->Text), text.c_str(), nullptr, width);
        } else if (!banner) {
            const GameEntry& game = games[static_cast<std::size_t>(selected_game)];
            // The icon a little over twice its size in a white frame, in the panel's middle.
            constexpr float Frame = 104.0f;
            const ImVec2 at((ScreenWidth - Frame) * 0.5f,
                            (StatusHeight + BannerBottom - Frame) * 0.5f);
            const ImRect frame(at, at + ImVec2(Frame, Frame));
            dl->AddRectFilled(frame.Min + ImVec2(0.0f, 3.0f), frame.Max + ImVec2(0.0f, 3.0f),
                              Colour(palette->Shadow), TileRounding);
            dl->AddRectFilled(frame.Min, frame.Max, Colour(palette->Row), TileRounding);
            const ImRect art(frame.Min + ImVec2(4.0f, 4.0f), frame.Max - ImVec2(4.0f, 4.0f));
            if (const IconSlot* slot = IconFor(game)) {
                dl->AddImageRounded(atlas, art.Min, art.Max, slot->uv0, slot->uv1, IM_COL32_WHITE,
                                    TileRounding - 4.0f);
            } else {
                DrawIcon(dl, Icon::Cartridge, art.GetCenter(), 56.0f, Colour(palette->Icon),
                         Colour(palette->Row));
            }
        }
        // The footer spans the display too: inside the clip it would stop 8 pixels short of each
        // edge.
        dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, ScreenHeight), false);
        const ImU32 panel = Colour(palette->Panel, 1.0f / palette->Panel.w);
        dl->AddRectFilled(ImVec2(0.0f, Footer), ImVec2(ScreenWidth, ScreenHeight), panel);
        dl->AddLine(ImVec2(0.0f, Footer), ImVec2(ScreenWidth, Footer), Colour(palette->Line));
        dl->PopClipRect();
    }
    ImGui::End();

    // Everything under the banner is the window the pad navigates: the grid, and the footer's
    // tools. The footer's pad buttons are in it too, for touch alone, as the window takes the
    // touches over its whole area.
    BeginList("##home", ImVec2(0.0f, BannerBottom), ImVec2(ScreenWidth, ScreenHeight));
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    const float bar_y = Footer + (FooterHeight - FooterButtonHeight) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(Margin, bar_y));
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    // A CIA is installed rather than opened, and has no settings or bubble of its own.
    const bool installer =
        !games.empty() && games[static_cast<std::size_t>(selected_game)].installer;
    bool start = !games.empty() && FooterButton(Glyph::Cross, installer ? "Install" : "Open");
    bool game_settings =
        !games.empty() && !installer && FooterButton(Glyph::Triangle, "Game settings");
    bool pin = false;
    if (!games.empty() && !installer) {
        const std::string& path = games[static_cast<std::size_t>(selected_game)].path;
        if (pin_checked_path != path) {
            pin_checked_path = path;
            pin_checked = LiveArea::IsPinned(path);
        }
        pin = FooterButton(Glyph::Square, pin_checked ? "Unpin" : "Pin to LiveArea");
    }
    ImGui::PopItemFlag();

    // The tools at the footer's right, which the pad reaches down from the grid's lower row.
    constexpr float ToolGap = 6.0f;
    ImGui::SetCursorScreenPos(ImVec2(ScreenWidth - Margin - BarButtonWidth("Settings") -
                                         BarButtonWidth("Install CIA") - BarButtonWidth("Exit") -
                                         2.0f * ToolGap,
                                     bar_y));
    // The pad's selection on a tool, however it got there: a touch leaves ImGui's highlight off,
    // and IsItemFocused asks for that too.
    const auto tool_selected = [] {
        return ImGui::GetFocusID() == ImGui::GetCurrentWindow()->DC.LastItemId;
    };
    bool on_tools = false;
    if (BarButton("Settings", Icon::Gear)) {
        OpenSettings({}, {}, State::GameList);
    }
    on_tools |= tool_selected();
    ImGui::SameLine(0.0f, ToolGap);
    if (BarButton("Install CIA", Icon::Install)) {
        OpenPickerDir(picker_dir.empty() ? std::string{GameDirectory} : picker_dir);
        state = State::InstallPicker;
        focus_first_row = true;
    }
    on_tools |= tool_selected();
    ImGui::SameLine(0.0f, ToolGap);
    if (BarButton("Exit", Icon::Power)) {
        action = Action::ExitApplication;
    }
    on_tools |= tool_selected();

    // The grid: the titles down its columns, two to a column, the selected tile's column in the
    // middle, eased there; a touch drags it sideways.
    const int count = static_cast<int>(games.size());
    const int columns = (count + Rows - 1) / Rows;
    const auto tile_id = [](int i) {
        ImGui::PushID(i);
        const ImGuiID id = ImGui::GetID("##tile");
        ImGui::PopID();
        return id;
    };
    // A step of the pad lands as the frame begins (ImGui applies it in NewFrame): the selection
    // follows it before the grid is laid out, so that the tile is drawn where the next step is
    // measured from. Holding a direction steps every frame, and a grid that caught up a frame
    // later would leave the next step's tile off the screen, where ImGui drops the pad's focus.
    // Coming back from the tools, the tile below them gives way to the selected one instead.
    bool on_tile = false;
    if (const ImGuiID focus = ImGui::GetFocusID(); focus != 0 && !tools_focused) {
        for (int i = 0; i < count; i++) {
            if (tile_id(i) == focus) {
                if (i != selected_game && !io.MouseDown[0]) {
                    carousel_free = false;
                }
                selected_game = i;
                on_tile = true;
                break;
            }
        }
    }
    const float target = static_cast<float>(selected_game / Rows) * Pitch;
    const float row_mid = ScreenWidth * 0.5f;
    const float grid_bottom = RowTop + static_cast<float>(Rows - 1) * RowPitch + Tile;
    const ImRect band(ImVec2(0.0f, RowTop - 12.0f), ImVec2(ScreenWidth, grid_bottom + 12.0f));
    if (io.MouseClicked[0]) {
        tap_selected = selected_game;
    }
    if (io.MouseDown[0] && band.Contains(io.MouseClickedPos[0])) {
        const ImVec2 drag = ImGui::GetMouseDragDelta(0, 0.0f);
        if (std::abs(drag.x) > 12.0f && std::abs(drag.x) > std::abs(drag.y)) {
            carousel_free = true;
            carousel_scroll -= io.MouseDelta.x;
            if (ImGui::GetActiveID() != 0) {
                ImGui::ClearActiveID();
            }
        }
    }
    carousel_scroll = std::clamp(carousel_scroll, -Pitch,
                                 static_cast<float>(std::max(columns - 1, 0)) * Pitch + Pitch);
    if (!carousel_free) {
        carousel_scroll += (target - carousel_scroll) * std::min(1.0f, io.DeltaTime * 14.0f);
    }
    // ImGui measures the pad's step from where the selected tile was drawn last frame. The grid
    // has scrolled since (a step moves it), so the measure moves with the tiles: from the old
    // place a held direction would skip columns.
    if (ImGuiContext& nav = *ImGui::GetCurrentContext(); nav.NavMoveRequest && on_tile) {
        nav.NavScoringRect.TranslateX(carousel_drawn - carousel_scroll);
    }
    carousel_drawn = carousel_scroll;

    // The grid's ends: a page of columns back or on, for touch, and L and R while the pad is on
    // the tiles. Their buttons go in before the tiles they lie over, which would take the touch
    // otherwise (the first item under it is the one hovered); they are drawn over the tiles.
    const float first_x = row_mid - Tile * 0.5f - carousel_scroll;
    const float last_x = first_x + static_cast<float>(std::max(columns - 1, 0)) * Pitch;
    constexpr float ArrowRadius = 30.0f;
    const float arrow_y = (RowTop + grid_bottom) * 0.5f;
    const auto arrow_shown = [&](bool left) {
        return count > 0 && (left ? first_x < 0.0f : last_x + Tile > ScreenWidth);
    };
    const auto turn_page = [&](bool left) {
        // A page from the column in the middle: the selected one, or after a drag the one the
        // grid shows there; in the selected tile's row.
        const int column =
            carousel_free
                ? std::clamp(static_cast<int>(std::lround(carousel_scroll / Pitch)), 0, columns - 1)
                : selected_game / Rows;
        const int page = static_cast<int>(std::floor(ScreenWidth / Pitch)) - 1;
        const int to = std::clamp(column + (left ? -page : page), 0, columns - 1);
        carousel_focus = std::min(to * Rows + selected_game % Rows, count - 1);
    };
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (const bool left : {true, false}) {
        if (!arrow_shown(left)) {
            continue;
        }
        ImGui::SetCursorScreenPos(
            ImVec2(left ? 0.0f : ScreenWidth - ArrowRadius, arrow_y - ArrowRadius));
        if (ImGui::InvisibleButton(left ? "##back" : "##on",
                                   ImVec2(ArrowRadius, 2.0f * ArrowRadius))) {
            turn_page(left);
        }
    }
    ImGui::PopItemFlag();
    if (on_tile && count > 0) {
        if (io.NavInputsDownDuration[ImGuiNavInput_FocusPrev] == 0.0f) {
            turn_page(true);
        } else if (io.NavInputsDownDuration[ImGuiNavInput_FocusNext] == 0.0f) {
            turn_page(false);
        }
    }

    const int before = selected_game;
    for (int i = 0; i < count; i++) {
        ImGui::PushID(i);
        const float x =
            row_mid - Tile * 0.5f + static_cast<float>(i / Rows) * Pitch - carousel_scroll;
        ImGui::SetCursorScreenPos(ImVec2(x, RowTop + static_cast<float>(i % Rows) * RowPitch));
        bool held = false;
        const Card card = CardBehavior("##tile", ImVec2(Tile, Tile), &held);
        // Back on the list: the game selected before, its tile in the middle at once. Not when a
        // tool has just opened another screen, which selects its own first item.
        if (focus_first_row && i == selected_game && state == State::GameList) {
            focus_first_row = false;
            carousel_free = false;
            carousel_scroll = target;
            FocusLastItem();
        }
        if (carousel_focus == i) {
            // The tile a page or the tools' way back chose, the grid eased back to it from
            // where a drag left it.
            carousel_focus = -1;
            carousel_free = false;
            FocusLastItem();
        }
        if (ImGui::GetFocusID() == card.id) {
            if (tools_focused && i != selected_game && !io.MouseDown[0]) {
                carousel_focus = selected_game;
            } else {
                selected_game = i;
            }
        }
        if (card.pressed) {
            // A tap selects a tile; a tap on the one selected (or cross) opens it.
            if (card.by_pad || tap_selected == i) {
                selected_game = i;
                start = true;
            }
        }
        if (card.visible) {
            const ImU32 white = Colour(held ? palette->RowFocused : palette->Row);
            dl->AddRectFilled(card.bb.Min + ImVec2(0.0f, 4.0f), card.bb.Max + ImVec2(0.0f, 4.0f),
                              Colour(palette->Shadow), TileRounding);
            dl->AddRectFilled(card.bb.Min, card.bb.Max, white, TileRounding);
            const ImRect art(card.bb.Min + ImVec2(7.0f, 7.0f), card.bb.Max - ImVec2(7.0f, 7.0f));
            if (const IconSlot* slot = IconFor(games[static_cast<std::size_t>(i)])) {
                dl->AddImageRounded(atlas, art.Min, art.Max, slot->uv0, slot->uv1, IM_COL32_WHITE,
                                    TileRounding - 7.0f);
            } else {
                DrawIcon(dl, Icon::Cartridge, art.GetCenter(), 44.0f, Colour(palette->Icon), white);
            }
            if (games[static_cast<std::size_t>(i)].installer) {
                // A CIA's badge in the corner: an arrow down into a tray.
                const ImVec2 c = card.bb.Max - ImVec2(15.0f, 15.0f);
                const ImU32 mark = IM_COL32_WHITE;
                dl->AddCircleFilled(c, 13.0f, Colour(palette->Accent), 20);
                dl->AddLine(c + ImVec2(0.0f, -7.0f), c + ImVec2(0.0f, 1.0f), mark, 2.5f);
                dl->AddTriangleFilled(c + ImVec2(-5.0f, -1.0f), c + ImVec2(5.0f, -1.0f),
                                      c + ImVec2(0.0f, 5.0f), mark);
                dl->AddLine(c + ImVec2(-7.0f, 7.0f), c + ImVec2(7.0f, 7.0f), mark, 2.0f);
            }
            if (card.focused) {
                DrawBrackets(dl, card.bb, Colour(palette->Accent));
            }
        }
        ImGui::PopID();
    }
    if (selected_game != before && !io.MouseDown[0]) {
        carousel_free = false;
    }
    tools_focused = on_tools;
    if (state == State::GameList) {
        focus_first_row = false;
    }

    // The speech bubble: the selected title's name and publisher over the grid's other row, its
    // point at the selected tile. Not while the pad is on the footer's tools.
    if (count > 0 && !on_tools) {
        const GameEntry& game = games[static_cast<std::size_t>(selected_game)];
        const float room = 560.0f;
        const std::string name = Fit(game.name, room);
        const std::string publisher =
            game.installer          ? Fit("CIA file: install it to play", room)
            : game.publisher.empty() ? std::string{}
                                     : Fit(game.publisher, room);
        const ImVec2 name_size = ImGui::CalcTextSize(name.c_str());
        const ImVec2 publisher_size = ImGui::CalcTextSize(publisher.c_str());
        const float lines = publisher.empty() ? name_size.y : name_size.y + 4.0f + publisher_size.y;
        const ImVec2 size(std::max(name_size.x, publisher_size.x) + 48.0f, lines + 18.0f);
        const float tile_x =
            row_mid + static_cast<float>(selected_game / Rows) * Pitch - carousel_scroll;
        const float point = std::clamp(tile_x, 24.0f, ScreenWidth - 24.0f);
        const float left = std::clamp(point - size.x * 0.5f, Margin, ScreenWidth - Margin - size.x);
        // Clear of the selected tile's brackets, the point between: under a tile of the upper
        // row, over one of the lower.
        constexpr float Gap = 16.0f;
        const bool upper = selected_game % Rows == 0;
        const float tile_top = RowTop + static_cast<float>(selected_game % Rows) * RowPitch;
        const float y = upper ? tile_top + Tile + Gap : tile_top - Gap - size.y;
        const ImRect bubble(ImVec2(left, y), ImVec2(left + size.x, y + size.y));
        dl->AddRectFilled(bubble.Min + ImVec2(0.0f, 3.0f), bubble.Max + ImVec2(0.0f, 3.0f),
                          Colour(palette->Shadow), 14.0f);
        dl->AddRectFilled(bubble.Min, bubble.Max, Colour(palette->Bubble), 14.0f);
        const float edge = upper ? bubble.Min.y + 1.0f : bubble.Max.y - 1.0f;
        const float tip = upper ? bubble.Min.y - 10.0f : bubble.Max.y + 10.0f;
        dl->AddTriangleFilled(ImVec2(point - 10.0f, edge), ImVec2(point + 10.0f, edge),
                              ImVec2(point, tip), Colour(palette->Bubble));
        const float top = bubble.Min.y + 9.0f;
        dl->AddText(ImVec2(bubble.GetCenter().x - name_size.x * 0.5f, top),
                    Colour(palette->BubbleText), name.c_str());
        if (!publisher.empty()) {
            dl->AddText(
                ImVec2(bubble.GetCenter().x - publisher_size.x * 0.5f, top + name_size.y + 4.0f),
                Colour(palette->BubbleTextDim), publisher.c_str());
        }
    }

    for (const bool left : {true, false}) {
        if (!arrow_shown(left)) {
            continue;
        }
        const ImVec2 c(left ? 0.0f : ScreenWidth, arrow_y);
        dl->AddCircleFilled(c + ImVec2(0.0f, 3.0f), ArrowRadius, Colour(palette->Shadow), 24);
        dl->AddCircleFilled(c, ArrowRadius, Colour(palette->Row), 24);
        const float d = left ? 1.0f : -1.0f;
        const ImVec2 tip(c.x + d * 10.0f, c.y);
        dl->AddTriangleFilled(tip, ImVec2(tip.x + d * 9.0f, c.y - 9.0f),
                              ImVec2(tip.x + d * 9.0f, c.y + 9.0f), Colour(palette->TextDim));
    }

    // Triangle on the list: the selected game's settings.
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // Nothing selected (the selection's item went away): the game selected last, next frame.
    if (state == State::GameList && focused && !games.empty() && ImGui::GetFocusID() == 0) {
        focus_first_row = true;
    }
    if (focused && !games.empty() &&
        ImGui::IsNavInputTest(ImGuiNavInput_Input, ImGuiInputReadMode_Pressed)) {
        game_settings = true;
    }
    // Square: the selected game on the LiveArea, or off it.
    if (focused && !games.empty() &&
        ImGui::IsNavInputTest(ImGuiNavInput_Menu, ImGuiInputReadMode_Pressed)) {
        pin = true;
    }
    overlay_focused = focused;
    ImGui::End();

    if (games.empty()) {
        return action;
    }
    const GameEntry& game = games[static_cast<std::size_t>(selected_game)];
    if (game.installer) {
        game_settings = pin = false;
    }
    if (game_settings) {
        OpenSettings(Config::TitleKeyOf(game.path), game.name, State::GameList,
                     ProgramIdOf(game.path));
    } else if (pin) {
        selected_game_path = game.path;
        action = Action::TogglePin;
    } else if (start) {
        selected_game_path = game.path;
        action = game.installer ? Action::InstallTitle : Action::LaunchGame;
    }
    return action;
}

std::string Ui::InstallSelected(const std::function<void()>& present) {
    GameEntry entry;
    if (!picked_cia.empty()) {
        // The Install CIA screen's: its file's name until the CIA's SMDH names it.
        entry.path = std::exchange(picked_cia, {});
        entry.name = std::string{FileUtil::GetFilename(entry.path)};
    } else if (games.empty() || !games[static_cast<std::size_t>(selected_game)].installer) {
        return "Choose a CIA to install.";
    } else {
        entry = games[static_cast<std::size_t>(selected_game)];
    }
    FileSys::CIAContainer container;
    if (!ReadCia(entry.path, container)) {
        LOG_ERROR(Frontend, "{} does not read as a CIA", entry.path);
        return "This CIA cannot be read. Make it again from your game.";
    }
    const u64 title_id = container.GetTitleMetadata().GetTitleID();
    // The contents against the memory card's free space: a title that does not fit would stop
    // partway, minutes in.
    const u64 needed = container.GetTitleMetadata().GetCombinedContentSize(container.GetHeader());
    const std::string sdmc = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
    const std::string device = sdmc.substr(0, sdmc.find(':') + 1);
    struct {
        u64 max_size;
        u64 free_size;
        u32 cluster_size;
        u32 unknown;
    } space{};
    if (sceIoDevctl(device.c_str(), 0x3001, nullptr, 0, &space, sizeof(space)) >= 0 &&
        space.free_size < needed) {
        LOG_ERROR(Frontend, "{} needs {} bytes on {}, {} are free", entry.path, needed, device,
                  space.free_size);
        constexpr u64 MiB = 1024 * 1024;
        return fmt::format("Free {} MiB on the memory card, then install it again.",
                           (needed - space.free_size + MiB - 1) / MiB);
    }

    SetStatusMessage(fmt::format("Installing {}...", entry.name));
    SetProgress(0.0f);
    present();
    const u64 start = sceKernelGetProcessTimeWide();
    u64 shown = start;
    const Service::AM::InstallStatus status = Service::AM::InstallCIA(
        entry.path, [&](std::size_t done, std::size_t total) {
            const u64 now = sceKernelGetProcessTimeWide();
            if (now - shown < 100'000 || total == 0) {
                return;
            }
            shown = now;
            SetProgress(static_cast<float>(done) / static_cast<float>(total));
            present();
        });
    LOG_INFO(Frontend, "Install of {} ({:016X}, {} bytes): status {} after {} ms", entry.path,
             title_id, needed, static_cast<u32>(status),
             (sceKernelGetProcessTimeWide() - start) / 1000);
    switch (status) {
    case Service::AM::InstallStatus::Success:
        break;
    case Service::AM::InstallStatus::ErrorEncrypted:
        return "Decrypt this CIA, then install it again.";
    case Service::AM::InstallStatus::ErrorInvalid:
        return "This CIA cannot be read. Make it again from your game.";
    case Service::AM::InstallStatus::ErrorAborted:
        return "The install stopped. Install the CIA again.";
    default:
        return "This CIA is gone. Choose it again with Install CIA.";
    }

    // The installed title in the list, selected (an update or DLC adds none: its game uses it).
    ScanGames();
    const std::string content =
        Service::AM::GetTitleContentPath(Service::FS::MediaType::SDMC, title_id);
    for (std::size_t i = 0; i < games.size(); i++) {
        if (games[i].path == content) {
            selected_game = static_cast<int>(i);
            carousel_focus = selected_game;
            tools_focused = false;
        }
    }
    switch (title_id >> 32) {
    case 0x0004000E:
        return fmt::format("{}: its update is installed.", entry.name);
    case 0x0004008C:
        return fmt::format("{}: its downloadable content is installed.", entry.name);
    default:
        return fmt::format("{} is installed.", entry.name);
    }
}

Ui::Action Ui::DrawPauseMenu() {
    Action action = Action::None;
    const ImVec2 Tile(236.0f, 72.0f);
    // The title line and the lines of two tiles (a diagnostics build's Dump frame makes a fourth),
    // with the window's padding and spacing.
#ifdef VITA_DIAGNOSTICS
    constexpr float Lines = 4.0f;
#else
    constexpr float Lines = 3.0f;
#endif
    BeginCentered("##pause", 2.0f * Tile.x + 10.0f + 32.0f,
                  32.0f + TitleSize + 6.0f + 10.0f + Lines * Tile.y + (Lines - 1.0f) * 10.0f);
    {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        dl->AddText(ImGui::GetFont(), TitleSize, at, Colour(palette->Text), "Paused");
        const float room = ImGui::GetContentRegionAvail().x - 130.0f;
        const std::string name = Fit(game_name, room);
        const ImVec2 size = ImGui::CalcTextSize(name.c_str());
        dl->AddText(ImVec2(at.x + ImGui::GetContentRegionAvail().x - size.x,
                           at.y + (TitleSize - size.y) * 0.5f),
                    Colour(palette->TextDim), name.c_str());
        ImGui::Dummy(ImVec2(0.0f, TitleSize + 6.0f));
    }

    int placed = 0;
    const auto tile = [&](const char* label, Icon icon) {
        if (placed++ % 2 == 1) {
            ImGui::SameLine();
        }
        return IconCard(label, icon, Tile);
    };
    if (tile("Resume", Icon::Play)) {
        action = Action::ResumeGame;
    }
    // Opened, or back from one of its screens: Resume selected, so that the pad's first press
    // moves from it rather than only showing where the selection is.
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    if (tile("Game settings", Icon::Cartridge)) {
        u64 program_id = 0;
        system.GetAppLoader().ReadProgramId(program_id);
        OpenSettings(game_key, game_name, State::Pause, program_id);
    }
    // The settings for all games are the game list's alone: in a game, its own.
    if (tile("Save states", Icon::Disk)) {
        // The slots as the card holds them now (a save since adds one).
        u64 program_id = 0;
        system.GetAppLoader().ReadProgramId(program_id);
        slot_states = Core::ListSaveStates(program_id, system.Movie().GetCurrentMovieID());
        state = State::Savestates;
        overlay_focused = false;
        focus_first_row = true;
    }
    if (tile("Multiplayer", Icon::People)) {
        state = State::Multiplayer;
        room_fields = Multiplayer::CurrentFields();
        editing_field = -1;
        overlay_focused = false;
        focus_first_row = true;
    }
#ifdef VITA_DIAGNOSTICS
    if (tile("Dump frame", Icon::Camera)) {
        // The next presented frame, without the menu, to ux0:data/azahar/frame_NN.ppm.
        SgxdPresent::DumpNextFrame();
    }
#endif
    if (tile("Quit game", Icon::Home)) {
        question = Question::QuitGame;
    }
    if (tile("Exit Azahar", Icon::Power)) {
        question = Question::ExitApplication;
    }

    if (BackPressed()) {
        action = Action::ResumeGame;
    }
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    return action;
}

Ui::Action Ui::DrawSavestates() {
    Action action = Action::None;
    BeginBackdrop("##savestates_backdrop", Icon::Disk, "Save states", game_name);
    bool back = FooterButton(Glyph::Circle, "Back");
    EndBackdrop();

    BeginList("##slots", ImVec2(Margin - 4.0f, HeaderHeight + 8.0f),
              ImVec2(ScreenWidth - Margin + 4.0f, ScreenHeight - FooterHeight - 8.0f));
    const ImVec2 button(160.0f, 40.0f);
    for (u32 slot = 1; slot <= 9; slot++) {
        ImGui::PushID(static_cast<int>(slot));
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddRectFilled(at, at + ImVec2(width, 52.0f),
                                                  Colour(palette->Row), RowRounding);
        // The slot's state: when it was saved (the console's local time), from a version whose
        // states this one can't load (another savestate format), or none.
        const auto saved =
            std::find_if(slot_states.begin(), slot_states.end(),
                         [slot](const Core::SaveStateInfo& s) { return s.slot == slot; });
        std::string info = "Empty";
        if (saved != slot_states.end()) {
            info = LocalTimeText(saved->time);
            if (saved->status == Core::SaveStateInfo::ValidationStatus::BuildMismatch) {
                info += "  (another build's)";
            }
        }
        const float text_y = at.y + (52.0f - ImGui::GetFontSize()) * 0.5f;
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(at.x + 18.0f, text_y), Colour(palette->Text),
                    fmt::format("Slot {}", slot).c_str());
        dl->AddText(ImVec2(at.x + 110.0f, text_y),
                    Colour(saved != slot_states.end() ? palette->Text : palette->TextDim),
                    info.c_str());
        ImGui::SetCursorScreenPos(at + ImVec2(width - 2.0f * button.x - 16.0f, 6.0f));
        if (ImGui::Button("Save", button)) {
            selected_slot = slot;
            if (saved != slot_states.end()) {
                question = Question::SaveOver;
            } else {
                action = Action::SaveState;
            }
        }
        if (focus_first_row && slot == 1) {
            focus_first_row = false;
            FocusLastItem();
        }
        ImGui::SameLine(0.0f, 8.0f);
        // Another format's state is refused by the core: nothing to load either.
        const bool loadable =
            saved != slot_states.end() &&
            saved->status != Core::SaveStateInfo::ValidationStatus::BuildMismatch;
        if (!loadable) {
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
        }
        if (ImGui::Button("Load", button) && loadable) {
            selected_slot = slot;
            question = Question::LoadSlot;
        }
        if (!loadable) {
            ImGui::PopStyleVar();
        }
        ImGui::SetCursorScreenPos(at + ImVec2(0.0f, 52.0f + 6.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        ImGui::PopID();
    }
    back |= BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (back) {
        state = State::Pause;
        overlay_focused = false;
        focus_first_row = true;
    }
    return action;
}

void Ui::OpenPickerDir(std::string dir) {
    picker_dir = std::move(dir);
    picker_entries.clear();
    if (picker_dir.empty()) {
        // The memory devices that are there, Emu4VitaPlus's: the memory card, a USB or SD2Vita
        // card, the system's user storage, the internal memory card, and host0. A device
        // is there when its root stats (opening the root as a folder misses mounted ones).
        for (const char* device : {"ux0:", "uma0:", "ur0:", "imc0:", "xmc0:", "host0:"}) {
            if (SceIoStat stat{}; sceIoGetstat(device, &stat) >= 0) {
                picker_entries.push_back({device, device, 0, true});
            }
        }
        return;
    }
    FileUtil::FSTEntry root;
    if (FileUtil::ScanDirectoryTree(picker_dir, root) == 0 && !FileUtil::IsDirectory(picker_dir)) {
        // A folder gone (the card taken out): its device's list instead.
        OpenPickerDir({});
        return;
    }
    for (const auto& entry : root.children) {
        // Adding a ROM folder, the titles the game list would take from it.
        if (entry.isDirectory ||
            (picker_folders ? HasRomExtension(entry.virtualName) : IsCiaFile(entry.virtualName))) {
            picker_entries.push_back(
                {entry.virtualName, entry.physicalName, entry.size, entry.isDirectory});
        }
    }
    // Folders first, then the files, each by name as the game list orders titles.
    std::sort(picker_entries.begin(), picker_entries.end(),
              [](const PickerEntry& a, const PickerEntry& b) {
                  return a.folder != b.folder ? a.folder : NaturalLess(a.name, b.name);
              });
}

Ui::Action Ui::DrawInstallPicker() {
    Action action = Action::None;
    BeginBackdrop("##picker_backdrop", Icon::Install, "Install CIA",
                  picker_dir.empty() ? std::string{"Memory devices"} : picker_dir);
    bool back = FooterButton(Glyph::Circle, picker_dir.empty() ? "Back" : "Parent folder");
    const bool open = FooterButton(Glyph::Cross, "Open");
    // In a folder, triangle leaves at once: back only goes up a folder.
    bool close = !picker_dir.empty() && FooterButton(Glyph::Triangle, Tr("Close"));
    bool add =
        picker_folders && !picker_dir.empty() && FooterButton(Glyph::Square, Tr("Add this folder"));
    EndBackdrop();

    BeginList("##picker", ImVec2(Margin - 4.0f, HeaderHeight + 8.0f),
              ImVec2(ScreenWidth - Margin + 4.0f, ScreenHeight - FooterHeight - 8.0f));
    const float width = ImGui::GetContentRegionAvail().x;
    std::optional<PickerEntry> chosen;
    for (std::size_t i = 0; i < picker_entries.size(); i++) {
        const PickerEntry& entry = picker_entries[i];
        ImGui::PushID(static_cast<int>(i));
        const Card card = CardItem("##entry", ImVec2(width, RowHeight));
        if (focus_first_row && i == 0) {
            focus_first_row = false;
            FocusLastItem();
        }
        if (card.visible) {
            ImDrawList* const dl = ImGui::GetWindowDrawList();
            const ImU32 under = Colour(card.focused ? palette->RowFocused : palette->Row);
            const float mid = card.bb.GetCenter().y;
            const Icon icon = entry.folder     ? Icon::Disk
                              : picker_folders ? Icon::Cartridge
                                               : Icon::Install;
            DrawIcon(dl, icon, ImVec2(card.bb.Min.x + 30.0f, mid), 24.0f, Colour(palette->Accent),
                     under);
            // A file's size at the right, in MiB as the install's free-space check counts.
            std::string size;
            if (!entry.folder) {
                size = fmt::format("{:.1f} MiB", static_cast<double>(entry.size) / (1024.0 * 1024.0));
            }
            const float size_width = ImGui::CalcTextSize(size.c_str()).x;
            const std::string name = Fit(entry.name, width - 90.0f - size_width);
            const float y = mid - ImGui::GetFontSize() * 0.5f;
            dl->AddText(ImVec2(card.bb.Min.x + 56.0f, y), Colour(palette->Text), name.c_str());
            if (!size.empty()) {
                dl->AddText(ImVec2(card.bb.Max.x - 18.0f - size_width, y),
                            Colour(palette->TextDim), size.c_str());
            }
        }
        // The row pressed, or the selected one when the footer's Open is touched.
        if (card.pressed || (open && card.focused)) {
            chosen = entry;
        }
        ImGui::PopID();
    }
    if (picker_entries.empty()) {
        ImGui::TextColored(palette->TextDim, "No folders or CIA files here.");
    }
    back |= BackPressed();
    close |= !picker_dir.empty() && overlay_focused &&
             ImGui::IsNavInputTest(ImGuiNavInput_Input, ImGuiInputReadMode_Pressed);
    add |= picker_folders && !picker_dir.empty() && overlay_focused &&
           ImGui::IsNavInputTest(ImGuiNavInput_Menu, ImGuiInputReadMode_Pressed);
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    if (add) {
        // Listed already, or the default folder: the settings show the list as it is.
        if (Config::AddRomFolder(picker_dir)) {
            LOG_INFO(Frontend, "ROM folder {} added", picker_dir);
            Config::Save();
            ScanGames();
        }
        ClosePicker();
    } else if (chosen && chosen->folder) {
        OpenPickerDir(chosen->path);
        focus_first_row = true;
    } else if (chosen && !picker_folders) {
        picked_cia = chosen->path;
        state = State::GameList;
        overlay_focused = false;
        action = Action::InstallTitle;
    } else if (close) {
        // Install CIA's folder is kept: the screen opens on it again.
        ClosePicker();
    } else if (back && !picker_dir.empty()) {
        // Up a folder ("ux0:/data/azahar" to "ux0:/data", "ux0:/data" to "ux0:"), and from a
        // device's root to the devices.
        const std::size_t colon = picker_dir.find(':');
        const std::size_t slash = picker_dir.find_last_of('/');
        std::string up;
        if (slash != std::string::npos && slash > colon + 1) {
            up = picker_dir.substr(0, slash);
        } else if (picker_dir.size() > colon + 2) {
            up = picker_dir.substr(0, colon + 1);
        }
        OpenPickerDir(std::move(up));
        focus_first_row = true;
    } else if (back) {
        ClosePicker();
    }
    return action;
}

void Ui::OpenRomFolderPicker() {
    picker_cia_dir = picker_dir;
    picker_folders = true;
    OpenPickerDir({});
    state = State::InstallPicker;
    focus_first_row = true;
}

void Ui::ClosePicker() {
    if (picker_folders) {
        picker_folders = false;
        picker_dir = std::move(picker_cia_dir);
        picker_entries.clear();
        state = State::Settings;
    } else {
        state = State::GameList;
    }
    overlay_focused = false;
    focus_first_row = true;
}

Ui::Action Ui::DrawMultiplayer() {
    if (editing_field >= 0) {
        DrawKeyboard();
        return Action::None;
    }
    BeginBackdrop("##multiplayer_backdrop", Icon::People, "Multiplayer", game_name);
    bool back = FooterButton(Glyph::Circle, "Back");
    EndBackdrop();

    // The room's fields and its buttons on the left, its state and members on the right, as the
    // settings screen lays out its rows and their description.
    const float top = HeaderHeight + 10.0f;
    const float bottom = ScreenHeight - FooterHeight - 8.0f;
    const float split = 560.0f;
    BeginList("##multiplayer_rows", ImVec2(Margin - 4.0f, top - 4.0f),
              ImVec2(split - 8.0f, bottom));
    const std::string port = std::to_string(room_fields.port);
    const std::string password(room_fields.password.size(), '*');
    const std::pair<const char*, const std::string*> fields[] = {
        {"Address", &room_fields.address},
        {"Port", &port},
        {"Nickname", &room_fields.nickname},
        {"Password", &password},
    };
    const float width = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < 4; i++) {
        ImGui::PushID(i);
        const Card card = CardItem("##field", ImVec2(width, RowHeight));
        if (focus_first_row && i == 0) {
            focus_first_row = false;
            FocusLastItem();
        }
        if (card.visible) {
            ImDrawList* const dl = ImGui::GetWindowDrawList();
            const float y = card.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
            dl->AddText(ImVec2(card.bb.Min.x + 18.0f, y), Colour(palette->Text), fields[i].first);
            const bool empty = fields[i].second->empty();
            const std::string value = Fit(empty ? "(none)" : *fields[i].second, width - 190.0f);
            const float value_width = ImGui::CalcTextSize(value.c_str()).x;
            dl->AddText(ImVec2(card.bb.Max.x - 18.0f - value_width, y),
                        Colour(empty ? palette->TextDim : palette->Text), value.c_str());
        }
        if (card.pressed) {
            editing_field = i;
            edit_text = i == 1 ? port : i == 3 ? room_fields.password : *fields[i].second;
            keyboard_shift = false;
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    const ImVec2 tool(180.0f, 48.0f);
    const bool can_join = !room_fields.address.empty() && !room_fields.nickname.empty();
    if (IconCard(Multiplayer::InRoom() ? "Join again" : "Join", Icon::People, tool) && can_join) {
        Multiplayer::Join(system, room_fields);
    }
    if (Multiplayer::InRoom()) {
        ImGui::SameLine();
        if (IconCard("Leave", Icon::Home, tool)) {
            Multiplayer::Leave();
        }
    }
    back |= BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(split, top));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth - Margin - split, bottom - top));
    ImGui::Begin("##multiplayer_info", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    const char* const heading = Multiplayer::InRoom() ? "In a room" : "Not in a room";
    ImGui::TextWrapped("%s", heading);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    // The status says more than the heading only while joining, connected or after an error.
    if (const std::string status = Multiplayer::Status(); status != heading) {
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::Spacing();
    }
    const std::vector<std::string> members = Multiplayer::Members();
    if (members.empty()) {
        ImGui::TextWrapped("Join a room hosted by a desktop Azahar (Multiplayer, Host Room) or its "
                           "room server. Every player runs the same game; its local wireless play "
                           "then finds the others.");
    }
    for (const std::string& member : members) {
        Note(palette->Accent, member);
    }
    if (!can_join) {
        Note(palette->TextDim, "Enter an address and a nickname to join.");
    }
    ImGui::End();

    if (back) {
        state = State::Pause;
        overlay_focused = false;
        focus_first_row = true;
    }
    return Action::None;
}

void Ui::DrawKeyboard() {
    static constexpr const char* key_rows[] = {"1234567890", "qwertyuiop", "asdfghjkl.",
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
    for (const char* row : key_rows) {
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
        focus_first_row = true;
        overlay_focused = false;
    }
}

void Ui::DrawStatusMessage() {
    // A panel as tall as its text, the text centred in it (a line across, wrapped lines left);
    // as wide as its longest line where that is wider (a crash notice's file paths), up to the
    // screen's width.
    constexpr float Padding = 28.0f;
    const float Width = std::clamp(ImGui::CalcTextSize(status_message.c_str()).x + 2.0f * Padding,
                                   560.0f, 920.0f);
    // With a progress: a bar under the text, its share filled in the accent colour.
    constexpr float BarHeight = 12.0f;
    constexpr float BarGap = 18.0f;
    const bool bar = status_progress >= 0.0f;
    const ImVec2 text =
        ImGui::CalcTextSize(status_message.c_str(), nullptr, false, Width - 2.0f * Padding);
    BeginCentered("##status", Width, text.y + 2.0f * Padding + (bar ? BarGap + BarHeight : 0.0f));
    ImGui::SetCursorPos(ImVec2(std::max(Padding, (Width - text.x) * 0.5f), Padding));
    ImGui::PushTextWrapPos(Width - Padding);
    ImGui::TextUnformatted(status_message.c_str());
    ImGui::PopTextWrapPos();
    if (bar) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const ImVec2 min = ImGui::GetWindowPos() + ImVec2(Padding, Padding + text.y + BarGap);
        const ImVec2 max = min + ImVec2(Width - 2.0f * Padding, BarHeight);
        dl->AddRectFilled(min, max, Colour(palette->SwitchOff), BarHeight * 0.5f);
        if (status_progress > 0.0f) {
            const float filled = std::max(BarHeight, (max.x - min.x) * status_progress);
            dl->AddRectFilled(min, ImVec2(min.x + filled, max.y), Colour(palette->Accent),
                              BarHeight * 0.5f);
        }
    }
    ImGui::End();
}

Ui::Action Ui::Update() {
    // In game: the stats overlay, which needs no ImGui frame (Draw).
    imgui_frame = !status_message.empty() || state != State::InGame;
    if (!imgui_frame) {
        return Action::None;
    }
    ++frame;
    // A theme chosen in the settings shows from the frame after.
    UseTheme();
    ImGui_ImplVita_NewFrame();

    Action action = Action::None;
    if (!status_message.empty()) {
        DrawStatusMessage();
    } else {
        switch (state) {
        case State::GameList:
            action = DrawGameList();
            break;
        case State::Settings:
            action = DrawSettings();
            break;
        case State::InGame:
            break;
        case State::Pause:
            action = DrawPauseMenu();
            break;
        case State::Savestates:
            action = DrawSavestates();
            break;
        case State::Multiplayer:
            action = DrawMultiplayer();
            break;
        case State::InstallPicker:
            action = DrawInstallPicker();
            break;
        }
    }

    if (action == Action::ResumeGame) {
        EnterGame();
    } else if (action == Action::QuitGame) {
        LeaveGame();
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

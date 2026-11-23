// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui_vita2d/imgui.h>
#include <imgui_vita2d/imgui_internal.h>
#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/devctl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <psp2/touch.h>
#include "azahar_native.h"

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
#include "citra_vita/i18n.h"
#include "citra_vita/i18n_tables.h"
#include "citra_vita/menu_glyphs.h"
#include "citra_vita/rom_check.h"
#include "citra_vita/sgxd_present.h"
#include "citra_vita/stats_hud.h"
#include "citra_vita/uvc_stream.h"
#include "citra_vita/vita_ui.h"
#include "common/assert.h"
#include "common/file_derived.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/archive_source_sd_savedata.h"
#include "core/file_sys/cia_container.h"
#include "core/file_sys/title_metadata.h"
#include "core/hle/service/am/am.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"
#include "core/movie.h"
#include "core/savestate.h"
#include "video_core/renderer_sgxd/sgxd_shader_disk_cache.h"

namespace VitaFrontend {

using I18n::Tr;
using I18n::Translatable;
using I18n::TrFormat;

namespace {
constexpr float ScreenWidth = 960.0f;
constexpr float ScreenHeight = 544.0f;

/// Where titles are looked for. One directory, no recursion: a console file manager is enough
/// work to use that a deep tree is worse than a flat one. A build with the debug tools also lists
/// the host PC's ROMs (host0:roms); without one host0 does not open and adds nothing.
constexpr char GameDirectory[] = "ux0:/data/azahar/roms";
#ifdef VITA_DEBUG_TOOLS
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

/// The full checks' outcomes (Ui::RememberCheck), a line a file: its size, the outcome, the part,
/// the path, tab-separated.
constexpr char CheckRecordFile[] = "ux0:data/azahar/checks.txt";

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
    Pad,
    Bin,
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
    case Icon::Bin:
        dl->AddLine(ImVec2(c.x - 0.38f * s, c.y - 0.26f * s),
                    ImVec2(c.x + 0.38f * s, c.y - 0.26f * s), colour, line);
        dl->AddRect(ImVec2(c.x - 0.12f * s, c.y - 0.4f * s),
                    ImVec2(c.x + 0.12f * s, c.y - 0.26f * s), colour, 0.0f, ImDrawCornerFlags_All,
                    line);
        dl->AddQuad(ImVec2(c.x - 0.28f * s, c.y - 0.16f * s),
                    ImVec2(c.x + 0.28f * s, c.y - 0.16f * s),
                    ImVec2(c.x + 0.2f * s, c.y + 0.4f * s), ImVec2(c.x - 0.2f * s, c.y + 0.4f * s),
                    colour, line);
        for (const float x : {c.x - 0.08f * s, c.x + 0.08f * s}) {
            dl->AddLine(ImVec2(x, c.y - 0.04f * s), ImVec2(x, c.y + 0.28f * s), colour, line);
        }
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
    case Icon::Pad: {
        // A handheld's body, its d-pad left and two buttons right, cut out of it.
        dl->AddRectFilled(ImVec2(c.x - 0.44f * s, c.y - 0.24f * s),
                          ImVec2(c.x + 0.44f * s, c.y + 0.24f * s), colour, 0.18f * s);
        const float t = 0.05f * s;
        const ImVec2 d(c.x - 0.22f * s, c.y);
        dl->AddRectFilled(ImVec2(d.x - 0.12f * s, d.y - t), ImVec2(d.x + 0.12f * s, d.y + t),
                          under);
        dl->AddRectFilled(ImVec2(d.x - t, d.y - 0.12f * s), ImVec2(d.x + t, d.y + 0.12f * s),
                          under);
        dl->AddCircleFilled(ImVec2(c.x + 0.16f * s, c.y + 0.06f * s), 0.06f * s, under, 10);
        dl->AddCircleFilled(ImVec2(c.x + 0.28f * s, c.y - 0.06f * s), 0.06f * s, under, 10);
        break;
    }
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
    Start,
};

/// The menus' confirm and back as the footer shows them (the system's enter button).
Glyph ConfirmGlyph() {
    return ImGui_ImplVita_ConfirmButton() == SCE_CTRL_CIRCLE ? Glyph::Circle : Glyph::Cross;
}
Glyph BackGlyph() {
    return ImGui_ImplVita_BackButton() == SCE_CTRL_CIRCLE ? Glyph::Circle : Glyph::Cross;
}
const char* ConfirmName() {
    return ImGui_ImplVita_ConfirmButton() == SCE_CTRL_CIRCLE ? Tr("Circle") : Tr("Cross");
}

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
    case Glyph::Start: {
        // START on a pill.
        const ImVec2 size = ImGui::CalcTextSize("START");
        const ImVec2 half(size.x * 0.5f + 0.5f * r, 0.7f * r);
        dl->AddRectFilled(c - half, c + half, disc, 0.5f * r);
        dl->AddText(ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), mark, "START");
        break;
    }
    }
}

/// What a glyph's width is beside its radius (the shoulders' two pills and START are wider).
float GlyphWidth(Glyph glyph, float r) {
    if (glyph == Glyph::Start) {
        return ImGui::CalcTextSize("START").x + r;
    }
    return glyph == Glyph::Shoulders ? 4.0f * r : 2.0f * r;
}

/// While an InputBlackout lives: the windows drawn take neither the focus nor the pad's navigation
/// (BeginList, BeginCentered).
bool blacked_out = false;

/// The pad and touch hidden from what is drawn while it lives: the screen behind a question, drawn
/// as it is but taking nothing. ImGui's widgets, and the screens' own drags, tab keys and back
/// button, read them from the IO as they are drawn, so with them cleared nothing behind takes a
/// press, a drag or a tab change; they are back for what is drawn after. The windows behind take
/// no focus either: ImGui moves the pad's selection within the focused window.
class InputBlackout {
public:
    InputBlackout() : io(ImGui::GetIO()), saved(io) {
        blacked_out = true;
        std::fill(std::begin(io.MouseDown), std::end(io.MouseDown), false);
        std::fill(std::begin(io.MouseClicked), std::end(io.MouseClicked), false);
        std::fill(std::begin(io.MouseDoubleClicked), std::end(io.MouseDoubleClicked), false);
        std::fill(std::begin(io.MouseReleased), std::end(io.MouseReleased), false);
        std::fill(std::begin(io.NavInputs), std::end(io.NavInputs), 0.0f);
        std::fill(std::begin(io.NavInputsDownDuration), std::end(io.NavInputsDownDuration), -1.0f);
        std::fill(std::begin(io.NavInputsDownDurationPrev), std::end(io.NavInputsDownDurationPrev),
                  -1.0f);
        io.MouseDelta = ImVec2(0.0f, 0.0f);
        io.MouseWheel = io.MouseWheelH = 0.0f;
    }
    ~InputBlackout() {
        io = saved;
        blacked_out = false;
    }
    InputBlackout(const InputBlackout&) = delete;
    InputBlackout& operator=(const InputBlackout&) = delete;

private:
    ImGuiIO& io;
    const ImGuiIO saved;
};

/// The flags of a window the pad navigates, out of the navigation behind a question.
ImGuiWindowFlags NavFlags() {
    return blacked_out ? ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus : 0;
}

/// The screen behind dimmed: a layer over every window but the panel drawn after it.
void DimScreen(const char* id) {
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth, ScreenHeight));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.55f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

/// A centred panel over whatever is behind it, for the pause menu.
bool BeginCentered(const char* id, float width, float height) {
    ImGui::SetNextWindowPos(ImVec2((ScreenWidth - width) * 0.5f, (ScreenHeight - height) * 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    return ImGui::Begin(id, nullptr,
                        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | NavFlags());
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
 * that loses focus), nor behind a question (InputBlackout).
 */
void BeginList(const char* id, ImVec2 min, ImVec2 max) {
    ImGui::SetNextWindowPos(min);
    ImGui::SetNextWindowSize(max - min);
    if (!blacked_out && !ImGui::GetIO().MouseDown[0] && ImGui::GetActiveID() == 0) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    // Room for the selected row's outline.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 8.0f));
    ImGui::Begin(id, nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | NavFlags());
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
    static constexpr const char* names[] = {Translatable("Display"), Translatable("Speed"),
                                            Translatable("Sound"),   Translatable("Controls"),
                                            Translatable("System"),  Translatable("Patches"),
                                            Translatable("Cheats"),  Translatable("Installed"),
                                            Translatable("File"),    Translatable("Data")};
    return names[category];
}

Icon CategoryIcon(int category) {
    static constexpr Icon icons[] = {Icon::Screens, Icon::Gauge,     Icon::Speaker, Icon::Pad,
                                     Icon::Globe,   Icon::Patch,     Icon::Cheat,   Icon::Disk,
                                     Icon::Cartridge, Icon::Bin};
    return icons[category];
}

u64 TreeSize(const FileUtil::FSTEntry& entry) {
    u64 size = 0;
    for (const FileUtil::FSTEntry& child : entry.children) {
        size += child.isDirectory ? TreeSize(child) : child.size;
    }
    return size;
}

struct FolderEntry {
    std::string name;
    u64 size;
    bool folder;
};

/// A folder's entries, with sceIoDread's stat (no call for each file); none when it does not open.
std::optional<std::vector<FolderEntry>> ListFolder(const std::string& path) {
    const SceUID d = sceIoDopen(path.c_str());
    if (d < 0) {
        return std::nullopt;
    }
    std::vector<FolderEntry> entries;
    SceIoDirent entry{};
    while (sceIoDread(d, &entry) > 0) {
        const std::string_view name{entry.d_name};
        if (name != "." && name != "..") {
            entries.push_back({std::string{name}, static_cast<u64>(entry.d_stat.st_size),
                               SCE_S_ISDIR(entry.d_stat.st_mode)});
        }
    }
    sceIoDclose(d);
    return entries;
}

void CountFolder(const std::string& path, u64& bytes, u32& files) {
    for (const FolderEntry& entry : ListFolder(path).value_or(std::vector<FolderEntry>{})) {
        if (entry.folder) {
            CountFolder(path + "/" + entry.name, bytes, files);
        } else {
            bytes += entry.size;
            files++;
        }
    }
}

/// Deletes a folder's files (`removed` after each), its folders, then the folder: how many of
/// them stayed.
u32 DeleteFolder(const std::string& path, const std::function<void()>& removed) {
    u32 left = 0;
    for (const FolderEntry& entry : ListFolder(path).value_or(std::vector<FolderEntry>{})) {
        const std::string inner = path + "/" + entry.name;
        if (entry.folder) {
            left += DeleteFolder(inner, removed);
        } else {
            left += sceIoRemove(inner.c_str()) < 0 ? 1 : 0;
            removed();
        }
    }
    return left + (sceIoRmdir(path.c_str()) < 0 ? 1 : 0);
}

/// CountFolder and DeleteFolder through FileUtil, for the emulated SD card: its paths deeper than
/// the Vita's ten components are folded into one name, which a plain listing does not show.
void CountSdFolder(const std::string& path, u64& bytes, u32& files) {
    FileUtil::ForeachDirectoryEntry(
        nullptr, path, [&](u64*, const std::string& folder, const std::string& name) {
            const std::string inner = folder + "/" + name;
            if (FileUtil::IsDirectory(inner)) {
                CountSdFolder(inner, bytes, files);
            } else {
                bytes += FileUtil::GetSize(inner);
                files++;
            }
            return true;
        });
}

u32 DeleteSdFolder(const std::string& path, const std::function<void()>& removed) {
    u32 left = 0;
    FileUtil::ForeachDirectoryEntry(
        nullptr, path, [&](u64*, const std::string& folder, const std::string& name) {
            const std::string inner = folder + "/" + name;
            if (FileUtil::IsDirectory(inner)) {
                left += DeleteSdFolder(inner, removed);
            } else {
                left += FileUtil::Delete(inner) ? 0 : 1;
                removed();
            }
            return true;
        });
    return left + (FileUtil::DeleteDir(path) ? 0 : 1);
}

std::string SizeText(u64 bytes) {
    constexpr u64 KiB = 1024, MiB = KiB * KiB;
    return bytes < MiB ? fmt::format("{} KiB", (bytes + KiB - 1) / KiB)
                       : fmt::format("{} MiB", (bytes + MiB - 1) / MiB);
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
    // The translations' characters past Latin-1 in the menus' font, from the console's own
    // Chinese font: a 24-pixel square of them stands as high as the font's capitals and digits
    // with their ascenders.
    MenuGlyphs::Add(*io.Fonts, io.FontDefault, 24.0f);
    io.Fonts->Flags |= ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoPowerOfTwoHeight;
    io.FontGlobalScale = 0.75f;
}

/// The atlas built and the glyphs MenuGlyphs reserved drawn into it: once every rectangle of it
/// is added, before a backend takes it.
void BuildMenuAtlas() {
    MenuGlyphs::Draw(*ImGui::GetIO().Fonts);
}
} // Anonymous namespace

void ShowGxmNotice(const char* title, const std::string& text, const std::string& hint,
                   const char* button) {
    if (!GxmPresent::Initialize()) {
        LOG_CRITICAL(Frontend, "Could not bring up GXM for: {}", text);
        return;
    }
    CreateMenuContext();
    BuildMenuAtlas();
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
    // The quick menu is drawn over the app, and its presses and touches reach the app's pad and
    // panel reads too: a cross there would close the notice and take the GPU from under the
    // quick menu. So the pad and the panel count only while no system UI is over the app, and
    // after one only from a frame with neither cross nor the panel held: the press that closed
    // the quick menu does not pass.
    bool held_off = true;
    bool was_overlaid = false;
    bool fading = false; // since the system's UI left, until the shell blanks its display plane
    for (bool done = false; !done;) {
        const bool overlaid = SystemUiOverlaid();
        if (overlaid != was_overlaid) {
            LOG_INFO(Frontend, "notice: the system's UI {} the app", overlaid ? "over" : "off");
            was_overlaid = overlaid;
        }
        if (overlaid) {
            fading = true;
        } else if (fading) {
            fading = ShellPlaneShown();
        }
        if (overlaid || fading) {
            held_off = true;
        } else if (held_off) {
            SceCtrlData pad{};
            SceTouchData touch{};
            const bool confirm = sceCtrlPeekBufferPositive(0, &pad, 1) > 0 &&
                                 (pad.buttons & ImGui_ImplVita_ConfirmButton()) != 0;
            const bool panel =
                sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0 && touch.reportNum > 0;
            held_off = confirm || panel;
        }
        ImGui_ImplVita_GamepadUsage(!held_off);
        ImGui_ImplVita_TouchUsage(!held_off);
        ImGui_ImplVita_NewFrame();
        // Read before the frame ends, which clears ImGui's pad inputs.
        const bool cross_down = ImGui::GetIO().NavInputs[ImGuiNavInput_Activate] > 0.0f;
        BeginBackdrop("##notice", Icon::Cartridge, title, "");
        const bool touched = FooterButton(ConfirmGlyph(), button);
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

        // A release over the button as the input is held off is no press of it.
        done = !held_off && (touched || (cross_down && !cross_was_down));
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

bool ShellPlaneShown() {
    const int shown = azaharShellPlaneShown();
    ASSERT_MSG(shown >= 0, "the shell's display plane was not read ({:#010x})",
               static_cast<u32>(shown));
    return shown != 0;
}

bool SystemUiOverlaid() {
    SceAppMgrAppState state{};
    const int got = _sceAppMgrGetAppState(&state, sizeof(state), 0);
    ASSERT_MSG(got == 0, "SceAppMgr's app state was not read ({:#010x})", static_cast<u32>(got));
    return state.isSystemUiOverlaid;
}

Ui::Ui(Core::System& system_, EmuWindowVita& window_) : system{system_}, window{window_} {
    CreateMenuContext();
    ImGuiIO& io = ImGui::GetIO();
    // The titles' icons get slots of the atlas, written as the game list shows them (IconFor):
    // the menus draw with one texture, and a slot is a rectangle of it like a glyph.
    for (int i = 0; i < IconSlots; ++i) {
        icon_rects.push_back(io.Fonts->AddCustomRectRegular(IconSize, IconSize));
    }
    BuildMenuAtlas();

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
    ControlLayouts::InitHud();
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
    return pinned ? Tr("The title's bubble is off the LiveArea.")
                  : Tr("Pinned: its LiveArea bubble starts the game in Azahar.");
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
#ifdef VITA_DEBUG_TOOLS
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
        // The screen layout's and the control layout's custom layouts follow their built-in
        // choices (ValueText).
        const int choices = row.option == Option::ScreenLayout
                                ? static_cast<int>(Config::Layout::Count)
                            : row.option == Option::ControlLayout
                                ? 1
                                : Config::Maximum(row.option) + 1;
        ASSERT_MSG(row.kind != Kind::Choice || static_cast<int>(row.choices.size()) == choices,
                   "settings: {}'s choices do not match its option", row.title);
        rows.push_back(std::move(row));
    };

    // Display: what is on the screen and where.
    add({Option::ScreenLayout,
         Category::Display,
         Kind::Choice,
         Translatable("Screen layout"),
         Translatable("How the two screens are arranged. Overlay: one screen fills the display and "
                      "the other sits small and see-through in the bottom-right corner, where you "
                      "can still touch it. Large overlay: the same with a bigger corner screen. "
                      "Your own layouts, made in the Layout editor below, follow these."),
         {Translatable("Default"), Translatable("Single screen"), Translatable("Large screen"),
          Translatable("Side by side"), Translatable("Hybrid"), Translatable("Overlay"),
          Translatable("Large overlay")},
         0,
         nullptr,
         nullptr});
    add({Option::SwapScreens,
         Category::Display,
         Kind::Toggle,
         Translatable("Swap screens"),
         Translatable("Puts the touch screen where the top screen would be."),
         {},
         0,
         nullptr,
         nullptr});
    add({Option::Widescreen,
         Category::Display,
         Kind::Choice,
         Translatable("Widescreen"),
         Translatable("How the top screen fills the display in the Single screen and overlay "
                      "layouts, with Swap screens off. Off: the 3DS's shape, with black bars at "
                      "the sides. Widescreen hack: shows more of the scene at the sides; it can "
                      "cause glitches, as some games leave the new edges empty or draw them "
                      "wrong. Widescreen stretch: stretches the picture to the display's width."),
         {Translatable("Off"), Translatable("Widescreen hack"), Translatable("Widescreen stretch")},
         0,
         nullptr,
         nullptr});
    add({Option::OverlayOpacity,
         Category::Display,
         Kind::Slider,
         Translatable("Overlay opacity"),
         Translatable("How opaque the small screen of the overlay layouts is."),
         {},
         5,
         "%",
         nullptr});
    add({Option::TopScale,
         Category::Display,
         Kind::Choice,
         Translatable("Internal resolution"),
         Translatable(
             "Draws both screens at a higher resolution: sharper wherever a screen is shown larger "
             "than on the 3DS, but much more GPU work (about 2.25 times at 1.5x), so slow games get "
             "slower. Can crash games that use a lot of video memory."),
         {Translatable("Native"), "1.25x", "1.5x", "1.75x", "2x"},
         0,
         nullptr,
         nullptr});
    add({Option::Msaa,
         Category::Display,
         Kind::Toggle,
         Translatable("Anti-aliasing (2x MSAA)"),
         Translatable("Smooths jagged edges on the top screen. It costs GPU time, so slow games "
                      "get slower, and Internal resolution stays at Native while it is on. Can "
                      "crash games that use a lot of video memory."),
         {},
         0,
         nullptr,
         nullptr});
    // Among a title's patches (DrawSettings), where the title's own offer it.
    add({Option::HudScale,
         Category::Patches,
         Kind::Choice,
         Translatable("HUD resolution"),
         Translatable("Draws the top screen's HUD, text and menus at a higher resolution over the "
                      "3D picture, which stays at the 3DS's: sharper text for little GPU work. It "
                      "needs Internal resolution at Native."),
         {Translatable("Native"), "1.25x", "1.5x"},
         0,
         nullptr,
         nullptr});
    add({Option::SmoothScreens,
         Category::Display,
         Kind::Toggle,
         Translatable("Smooth picture"),
         Translatable("Blends neighbouring pixels where the 3DS's picture is stretched onto the "
                      "display, instead of showing each as a block. It costs no speed. For "
                      "Internal resolution at Native: a higher one is always smoothed."),
         {},
         0,
         nullptr,
         nullptr});
    add({Option::StatsOverlay,
         Category::Display,
         Kind::Toggle,
         Translatable("Stats overlay"),
         Translatable("Shows the speed and frame rates in the top-right corner while a game runs."),
         {},
         0,
         nullptr,
         nullptr});

    // Speed: how fast and how smoothly.
    add({Option::Vsync,
         Category::Speed,
         Kind::Toggle,
         Translatable("Vsync"),
         Translatable("Shows each picture whole at the display's refresh and holds the game to the "
                      "display's rate. Off: pictures show at once and may tear, and Speed limit "
                      "sets the pace."),
         {},
         0,
         nullptr,
         nullptr});
    add({Option::SpeedLimit,
         Category::Speed,
         Kind::Slider,
         Translatable("Speed limit"),
         Translatable("How fast the game may run, in percent of full speed. No limit runs it as "
                      "fast as the console can. Lower it to save battery."),
         {},
         5,
         "%",
         Translatable("No limit")});
    add({Option::FrameSkipLimit,
         Category::Speed,
         Kind::Choice,
         Translatable("Frame skip limit"),
         Translatable(
             "When the GPU falls behind, the busier screen skips frames so the game keeps its "
             "speed. This sets how many frames in a row it may skip: higher keeps heavy games "
             "faster but less smooth. Off draws every frame and the game slows down instead."),
         {Translatable("Off"), Translatable("1 frame"), Translatable("2 frames"),
          Translatable("3 frames"), Translatable("4 frames"), Translatable("No limit")},
         0,
         nullptr,
         nullptr});
    add({Option::WaitForGpu,
         Category::Speed,
         Kind::Toggle,
         Translatable("Wait for GPU results"),
         Translatable("The game waits for the GPU to finish a picture before reading it, as on a "
                      "3DS. Off can run faster, but may cause flickering, outdated or missing "
                      "pictures and broken effects."),
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
    // Among a title's patches too, where the title's own offer it.
    add({Option::EvenPacing,
         Category::Patches,
         Kind::Toggle,
         Translatable("Even frame pacing (experimental)"),
         Translatable("Experimental. Shows the game's pictures at an even beat of two refreshes: "
                      "one that is ready early waits for its turn. For a game that runs at 30 "
                      "frames a second and still looks choppy. It works with Vsync on, and can "
                      "show a picture up to two refreshes (33 ms) later."),
         {},
         0,
         nullptr,
         nullptr});
    add({Option::SimulateGpuTimings,
         Category::Speed,
         Kind::Toggle,
         Translatable("Simulate 3DS GPU timings"),
         Translatable("The game hears that the GPU is done only after the time a 3DS's GPU would "
                      "take. It can steady games whose speed follows their frame time, but games "
                      "that wait for the GPU lose frames."),
         {},
         0,
         nullptr,
         nullptr});
    add({Option::CpuClock,
         Category::Speed,
         Kind::Slider,
         Translatable("3DS CPU clock"),
         Translatable("How fast the emulated 3DS CPU runs, in percent of a 3DS's. Lower can speed "
                      "up games the Vita's CPU is too slow for; higher can smooth games that slow "
                      "down on a 3DS too. Either can make a game freeze or misbehave."),
         {},
         5,
         "%",
         nullptr});
    add({Option::Multicore,
         Category::Speed,
         Kind::Toggle,
         Translatable("Multicore"),
         Translatable("Runs the 3DS's CPU cores on two of the Vita's cores at once, which is "
                      "faster in most games. Turn it off for a game that freezes or misbehaves "
                      "with it on."),
         {},
         0,
         nullptr,
         nullptr});

    // Sound.
    add({Option::StretchAudio,
         Category::Sound,
         Kind::Toggle,
         Translatable("Stretch audio"),
         Translatable("Below full speed, slows the sound down with the game instead of letting it "
                      "break up."),
         {},
         0,
         nullptr,
         nullptr});

    // Controls: a control layout of the player's, or the Vita input each 3DS button answers to.
    add({Option::ControlLayout,
         Category::Controls,
         Kind::Choice,
         Translatable("Control layout"),
         Translatable("Which Vita input presses each 3DS button. Buttons below: the rows below "
                      "this one. Your own layouts, made in the control layout editor, have a "
                      "mapping of their own and can put buttons on the screen, which a finger on "
                      "the display presses."),
         {Translatable("Buttons below")},
         0,
         nullptr,
         nullptr});
    const std::vector<const char*> inputs{Translatable("Circle"),
                                          Translatable("Cross"),
                                          Translatable("Triangle"),
                                          Translatable("Square"),
                                          Translatable("Up"),
                                          Translatable("Down"),
                                          Translatable("Left"),
                                          Translatable("Right"),
                                          "L",
                                          "R",
                                          "Start",
                                          "Select",
                                          Translatable("Rear touch, left half"),
                                          Translatable("Rear touch, right half"),
                                          Translatable("None")};
    const auto control = [&](Option option, const char* title, const char* description) {
        add({option, Category::Controls, Kind::Choice, title, description, inputs, 0, nullptr,
             nullptr});
    };
    control(Option::MapA, "A", Translatable("The Vita input that presses the 3DS's A."));
    control(Option::MapB, "B", Translatable("The Vita input that presses the 3DS's B."));
    control(Option::MapX, "X", Translatable("The Vita input that presses the 3DS's X."));
    control(Option::MapY, "Y", Translatable("The Vita input that presses the 3DS's Y."));
    control(Option::MapUp, Translatable("D-pad up"),
            Translatable("The Vita input that presses the 3DS's d-pad up."));
    control(Option::MapDown, Translatable("D-pad down"),
            Translatable("The Vita input that presses the 3DS's d-pad down."));
    control(Option::MapLeft, Translatable("D-pad left"),
            Translatable("The Vita input that presses the 3DS's d-pad left."));
    control(Option::MapRight, Translatable("D-pad right"),
            Translatable("The Vita input that presses the 3DS's d-pad right."));
    control(Option::MapL, "L", Translatable("The Vita input that presses the 3DS's L."));
    control(Option::MapR, "R", Translatable("The Vita input that presses the 3DS's R."));
    control(Option::MapZL, "ZL",
            Translatable("The Vita input that presses the New 3DS's ZL (and the Circle Pad Pro's). "
                         "A rear touch half is held while a finger is anywhere on it."));
    control(Option::MapZR, "ZR",
            Translatable("The Vita input that presses the New 3DS's ZR (and the Circle Pad Pro's). "
                         "A rear touch half is held while a finger is anywhere on it."));
    control(Option::MapStart, "Start",
            Translatable("The Vita input that presses the 3DS's Start."));
    control(Option::MapSelect, "Select",
            Translatable("The Vita input that presses the 3DS's Select."));
    add({Option::RightStick,
         Category::Controls,
         Kind::Choice,
         Translatable("Right stick"),
         Translatable("What the right stick moves: the 3DS's C-stick (the New 3DS's, and the "
                      "Circle Pad Pro's in games made for it), L and R (left and right, for games "
                      "that turn the camera with them), or the d-pad."),
         {Translatable("C-stick"), Translatable("L and R"), Translatable("D-pad")},
         0,
         nullptr,
         nullptr});
    add({Option::SwapShortcut,
         Category::Controls,
         Kind::Choice,
         Translatable("Swap screens shortcut"),
         Translatable("Swaps the screens while you play. With PS: hold the PS button and press the "
                      "other; the game sees neither, and the PS button alone still opens this "
                      "menu as you let go of it. A rear touch half also presses the 3DS button it "
                      "is mapped to."),
         {Translatable("PS + Circle"), Translatable("PS + Cross"), Translatable("PS + Triangle"),
          Translatable("PS + Square"), Translatable("PS + Up"), Translatable("PS + Down"),
          Translatable("PS + Left"), Translatable("PS + Right"), Translatable("PS + L"),
          Translatable("PS + R"), Translatable("PS + Start"), Translatable("PS + Select"),
          Translatable("Rear touch, left half"), Translatable("Rear touch, right half"),
          Translatable("None")},
         0,
         nullptr,
         nullptr});
    add({Option::Motion,
         Category::Controls,
         Kind::Toggle,
         Translatable("Motion controls"),
         Translatable("Tilting and turning the Vita moves the 3DS's motion sensors, for games "
                      "that steer or aim with them. Its screen is the touch screen, held as you "
                      "would hold the 3DS open. Off, the sensors stay still."),
         {},
         0,
         nullptr,
         nullptr});
    add({Option::HiddenScreenTouch,
         Category::Controls,
         Kind::Toggle,
         Translatable("Touch a hidden bottom screen"),
         Translatable("With a layout that shows the top screen alone, touching the display still "
                      "presses the bottom screen where it would be. Turn it off so that a brush "
                      "of the display presses nothing."),
         {},
         0,
         nullptr,
         nullptr});

    // System: the console and the menus.
    add({Option::Theme,
         Category::System,
         Kind::Choice,
         Translatable("Menu theme"),
         Translatable("The menus' colours: dark grey, or the light grey of the 3DS HOME Menu."),
         {Translatable("Dark"), Translatable("Light")},
         0,
         nullptr,
         nullptr});
    add({Option::Region,
         Category::System,
         Kind::Choice,
         Translatable("Region"),
         Translatable("The console region reported to the game. Auto picks one the game accepts."),
         {Translatable("Auto"), Translatable("Japan"), Translatable("USA"), Translatable("Europe"),
          Translatable("Australia"), Translatable("China"), Translatable("Korea"),
          Translatable("Taiwan")},
         0,
         nullptr,
         nullptr});
    add({Option::SystemLanguage,
         Category::System,
         Kind::Choice,
         Translatable("System language"),
         Translatable("The 3DS's language, which games show their text in. With region Auto, a "
                      "language the game's region lacks gives that region's own."),
         {Translatable("Japanese"), Translatable("English"), Translatable("French"),
          Translatable("German"), Translatable("Italian"), Translatable("Spanish"),
          Translatable("Simplified Chinese"), Translatable("Korean"), Translatable("Dutch"),
          Translatable("Portuguese"), Translatable("Russian"), Translatable("Traditional Chinese")},
         0,
         nullptr,
         nullptr});
    add({Option::ForceNew3ds,
         Category::System,
         Kind::Toggle,
         Translatable("Force New 3DS mode"),
         Translatable("Runs every game as on a New 3DS. Games with New 3DS enhancements turn them "
                      "on, which needs more of the Vita's CPU and memory, so they can get slower. "
                      "Off: only games made for the New 3DS alone run as on one. A save state "
                      "keeps the mode it was saved in."),
         {},
         0,
         nullptr,
         nullptr});
    add({Option::StartupNotice,
         Category::System,
         Kind::Toggle,
         Translatable("Startup notice"),
         Translatable(
             "Shows a notice as Azahar starts, while the quick menu, notifications and plugin "
             "menus such as PSVshell's still work. Off: Azahar takes over the GPU at once."),
         {},
         0,
         nullptr,
         nullptr});
    add({Option::UsbVideo,
         Category::System,
         Kind::Choice,
         Translatable("USB video"),
         Translatable("Sends one screen to a PC over the USB cable, as a 400x240 webcam named "
                      "Azahar. The Vita keeps its own layout: to see only the other screen, use "
                      "Single screen (with Swap screens if needed)."),
         {Translatable("Off"), Translatable("Top screen"), Translatable("Bottom screen")},
         0,
         nullptr,
         nullptr});
    // Each language's name in its own language: the one the player reads is there whichever the
    // menus show.
    add({Option::Language,
         Category::System,
         Kind::Choice,
         Translatable("Language"),
         Translatable("The language of Azahar's menus. A game keeps its own."),
         {I18n::LanguageNames().begin(), I18n::LanguageNames().end()},
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
    if (stuck_ask.exchange(false, std::memory_order_acquire)) {
        question = Question::Stuck;
    }
}

void Ui::AskStuck() {
    stuck_ask.store(true, std::memory_order_release);
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

void Ui::OpenSettings(std::string title, std::string name, State back, u64 program_id,
                      std::string path) {
    settings_title = std::move(title);
    settings_name = std::move(name);
    settings_back = back;
    settings_path = std::move(path);
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
    settings_engine = settings_program_id != 0 ? Loader::EngineChoicesOf(settings_program_id)
                                               : Loader::EngineChoices{};
    // A title not started yet: its code searched when the Patches tab first opens.
    engine_search_asked = false;
    ScanInstalled();
    settings_data.clear();
    data_scanned = false;
    category = SettingsCategories().front();
    focused_row = -1;
    focused_patch = -1;
    focused_engine = -1;
    focused_cheat = -1;
    focused_installed = armed_installed = -1;
    focused_data = -1;
    focus_first_row = true;
    state = State::Settings;
    overlay_focused = false;
}

void Ui::ScanInstalled() {
    // What of it is installed: from the game list only, never under a running title.
    settings_installed.clear();
    settings_rom_version.reset();
    if (settings_back != State::GameList || settings_program_id == 0) {
        return;
    }
    const u64 low = settings_program_id & 0xFFFFFFFF;
    const bool application = settings_program_id >> 32 == 0x00040000;
    const std::pair<const char*, u64> parts[] = {
        {Translatable("Installed game"), settings_program_id},
        {Translatable("Update"), application ? 0x0004000Eull << 32 | low : 0},
        {Translatable("Downloadable content"), application ? 0x0004008Cull << 32 | low : 0},
    };
    bool game_installed = false;
    for (const auto& [kind, title_id] : parts) {
        const std::string content =
            Service::AM::GetTitlePath(Service::FS::MediaType::SDMC, title_id) + "content";
        FileUtil::FSTEntry tree;
        if (title_id == 0 || FileUtil::ScanDirectoryTree(content, tree, 2) == 0) {
            continue;
        }
        // A partial install (a crash mid-way) has contents but maybe no TMD: version 0.
        FileSys::TitleMetadata tmd;
        const u16 version =
            tmd.Load(Service::AM::GetTitleMetadataPath(Service::FS::MediaType::SDMC, title_id)) ==
                    Loader::ResultStatus::Success
                ? tmd.GetTitleVersion()
                : 0;
        settings_installed.push_back({kind, title_id, TreeSize(tree), version, false, false});
        game_installed |= title_id == settings_program_id;
    }
    // The title's CCI (an installed title's file is its content, no CCI): its version, and the
    // row that installs it while its game is not installed.
    const auto cci = settings_path.empty() ? std::nullopt
                                           : Service::AM::ReadCCIInstallInfo(settings_path);
    if (!cci || cci->title_id != settings_program_id) {
        return;
    }
    settings_rom_version = cci->version;
    if (!game_installed) {
        settings_installed.insert(settings_installed.begin(),
                                  {Translatable("Install to system"), cci->title_id,
                                   cci->content_size, cci->version, true, cci->encrypted});
    }
}

std::vector<Ui::Category> Ui::SettingsCategories() const {
    std::vector<Category> shown;
    for (int c = 0; c < static_cast<int>(Category::Count); c++) {
        const auto tab = static_cast<Category>(c);
        // Patches and Cheats even with none: the engine settings are looked for when the tab
        // opens, and the Cheats tab says where the title's cheat file goes.
        const bool has_rows =
            tab == Category::Patches     ? settings_program_id != 0
            : tab == Category::Cheats    ? settings_program_id != 0
            : tab == Category::Installed ? !settings_installed.empty()
            : tab == Category::Data      ? settings_program_id != 0
            : tab == Category::File
                ? !settings_path.empty()
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
    // USB video turned on while another plugin has the port: the player is asked first.
    if (row.option == Config::Option::UsbVideo && value != 0 && RowValue(row) == 0 &&
        !UvcStream::TakeoverAllowed() && UvcStream::PortTaken()) {
        question = Question::UsbSetting;
        usb_prompt_value = value;
        return;
    }
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
        // The screen layout's and the control layout's custom layouts after their built-in
        // choices.
        const int count = row.option == Config::Option::ScreenLayout ||
                                  row.option == Config::Option::ControlLayout
                              ? Config::Maximum(row.option) + 1
                              : static_cast<int>(row.choices.size());
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
        return Tr("Off while Vsync is on. Turn Vsync off to set a speed limit.");
    }
    if (row.option >= Config::Option::MapA && row.option <= Config::Option::MapSelect &&
        Config::CustomControlIndex(other(Config::Option::ControlLayout))) {
        return Tr("Set by the control layout. Change it in the control layout editor, or choose "
                  "Buttons below.");
    }
    if (row.option == Config::Option::TopScale && other(Config::Option::Msaa) != 0) {
        return Tr(
            "Native while Anti-aliasing is on. Turn Anti-aliasing off to choose a resolution.");
    }
    return nullptr;
}

std::string Ui::ValueText(const Row& row, int value) const {
    switch (row.kind) {
    case Row::Kind::Toggle:
        return value != 0 ? Tr("On") : Tr("Off");
    case Row::Kind::Choice:
        if (const auto custom = Config::CustomLayoutIndex(value);
            custom && row.option == Config::Option::ScreenLayout) {
            const auto layout = ScreenLayouts::Get(*custom);
            return layout ? layout->name : std::string{};
        }
        if (const auto custom = Config::CustomControlIndex(value);
            custom && row.option == Config::Option::ControlLayout) {
            const auto layout = ControlLayouts::Get(*custom);
            return layout ? layout->name : std::string{};
        }
        return Tr(row.choices[static_cast<std::size_t>(value)]);
    case Row::Kind::Slider:
        if (value == 0 && row.zero != nullptr) {
            return Tr(row.zero);
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
            value_width = std::max(value_width, ImGui::CalcTextSize(Tr(choice)).x);
        }
        if (row.option == Config::Option::ScreenLayout) {
            for (const std::string& name : ScreenLayouts::Names()) {
                value_width = std::max(value_width, ImGui::CalcTextSize(name.c_str()).x);
            }
        }
        value_width += 2.0f * (Arrow + 14.0f);
    } else if (row.kind == Row::Kind::Slider) {
        number_width =
            ImGui::CalcTextSize(fmt::format("{}{}", Config::Maximum(row.option), row.unit).c_str())
                .x;
        if (row.zero != nullptr) {
            number_width = std::max(number_width, ImGui::CalcTextSize(Tr(row.zero)).x);
        }
        value_width = BarWidth + 14.0f + number_width;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const float label_width = width - value_width - 3.0f * Pad;
    const char* const title = Tr(row.title);
    const ImVec2 label = ImGui::CalcTextSize(title, nullptr, false, label_width);
    const Card card = CardItem("##row", ImVec2(width, std::max(RowHeight, label.y + 28.0f)));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    const bool focused = ImGui::GetFocusID() == card.id;
    if (focused) {
        focused_row = static_cast<int>(index);
        focused_patch = -1;
        focused_engine = -1;
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
                    title, nullptr, label_width);
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
    const char* state = Tr("Built in");
    ImVec4 state_colour = palette->Text;
    if (!settings_patches_in.empty()) {
        const bool in = settings_patches_in[index];
        state = in ? Tr("Applied") : Tr("Left out");
        state_colour = in ? palette->Accent : palette->TextDim;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 state_size = ImGui::CalcTextSize(state);
    const float label_width = width - state_size.x - 3.0f * Pad;
    const char* const name = Tr(patch.name);
    const ImVec2 label = ImGui::CalcTextSize(name, nullptr, false, label_width);
    const Card card = CardItem("##patch", ImVec2(width, std::max(RowHeight, label.y + 28.0f)));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    if (ImGui::GetFocusID() == card.id) {
        focused_patch = static_cast<int>(index);
        focused_engine = -1;
        focused_row = -1;
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float mid = card.bb.GetCenter().y;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text), name,
                    nullptr, label_width);
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
    ImGui::TextWrapped("%s", Tr(patch.name));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", Tr(patch.description));
    ImGui::Spacing();

    if (settings_patches_in.empty()) {
        Note(palette->TextDim,
             Tr("Applied when the game starts, if your copy is the version it was made for."));
    } else if (settings_patches_in[index]) {
        Note(palette->Accent, Tr("Applied to the running game."));
    } else {
        Note(palette->TextDim,
             Tr("Left out: your copy is a different version from the one it was made for."));
    }
    Note(palette->TextDim, Tr("Built into Azahar for this game. It can't be turned off."));
}

void Ui::DrawEngineRow(std::size_t index) {
    constexpr float Pad = 18.0f;

    ImGui::PushID(static_cast<int>(index));
    const Loader::EngineSetting& setting = Loader::EngineSettings()[index];
    const bool on = Loader::EngineSettingOn(settings_engine, index);
    const float width = ImGui::GetContentRegionAvail().x;
    const float label_width = width - 56.0f - 3.0f * Pad;
    const char* const name = Tr(setting.name);
    const ImVec2 label = ImGui::CalcTextSize(name, nullptr, false, label_width);
    const Card card = CardItem("##engine", ImVec2(width, std::max(RowHeight, label.y + 28.0f)));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    const bool focused = ImGui::GetFocusID() == card.id;
    if (focused) {
        focused_engine = static_cast<int>(index);
        focused_patch = -1;
        focused_row = -1;
    }
    if (card.pressed ||
        (focused && (ImGui::IsNavInputTest(ImGuiNavInput_DpadLeft, ImGuiInputReadMode_Pressed) ||
                     ImGui::IsNavInputTest(ImGuiNavInput_DpadRight, ImGuiInputReadMode_Pressed)))) {
        Loader::SetEngineChoice(settings_program_id, 1u << settings_engine.bits[index], !on);
        settings_engine = Loader::EngineChoicesOf(settings_program_id);
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float mid = card.bb.GetCenter().y;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text), name,
                    nullptr, label_width);
        DrawSwitch(dl, card.bb.Max.x - Pad, mid, Loader::EngineSettingOn(settings_engine, index),
                   1.0f);
    }
    ImGui::PopID();
}

void Ui::DrawEngineValueRow(std::size_t index) {
    constexpr float Pad = 18.0f;
    constexpr float Arrow = 10.0f;

    ImGui::PushID(static_cast<int>(1000 + index));
    const Loader::EngineValue& value = Loader::EngineValues()[index];
    std::size_t choices = 0;
    float value_width = 0.0f;
    while (choices < value.labels.size() && value.labels[choices] != nullptr) {
        value_width = std::max(value_width, ImGui::CalcTextSize(Tr(value.labels[choices])).x);
        choices++;
    }
    value_width += 2.0f * (Arrow + 14.0f);
    const float width = ImGui::GetContentRegionAvail().x;
    const float label_width = width - value_width - 3.0f * Pad;
    const char* const name = Tr(value.name);
    const ImVec2 label = ImGui::CalcTextSize(name, nullptr, false, label_width);
    const Card card = CardItem("##engine", ImVec2(width, std::max(RowHeight, label.y + 28.0f)));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    const bool focused = ImGui::GetFocusID() == card.id;
    if (focused) {
        focused_engine = static_cast<int>(Loader::EngineSettingCount + index);
        focused_patch = -1;
        focused_row = -1;
    }
    const ImRect value_bb(card.bb.Max.x - Pad - value_width, card.bb.Min.y, card.bb.Max.x - Pad,
                          card.bb.Max.y);
    int delta = 0;
    if (focused && ImGui::IsNavInputTest(ImGuiNavInput_DpadLeft, ImGuiInputReadMode_Repeat)) {
        delta = -1;
    } else if (focused &&
               ImGui::IsNavInputTest(ImGuiNavInput_DpadRight, ImGuiInputReadMode_Repeat)) {
        delta = 1;
    } else if (card.pressed) {
        const float x = ImGui::GetIO().MousePos.x;
        delta = !card.by_pad && x >= value_bb.Min.x && x < value_bb.GetCenter().x ? -1 : 1;
    }
    const std::size_t choice = settings_engine.value[index];
    if (delta != 0) {
        const std::size_t next =
            delta > 0 ? (choice + 1) % choices : (choice + choices - 1) % choices;
        Loader::SetEngineValue(settings_program_id, index, static_cast<u8>(next));
        settings_engine = Loader::EngineChoicesOf(settings_program_id);
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float mid = card.bb.GetCenter().y;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text), name,
                    nullptr, label_width);
        const ImU32 arrows = Colour(focused ? palette->Accent : palette->TextDim);
        dl->AddTriangleFilled(ImVec2(value_bb.Min.x, mid),
                              ImVec2(value_bb.Min.x + Arrow, mid - Arrow),
                              ImVec2(value_bb.Min.x + Arrow, mid + Arrow), arrows);
        dl->AddTriangleFilled(ImVec2(value_bb.Max.x, mid),
                              ImVec2(value_bb.Max.x - Arrow, mid - Arrow),
                              ImVec2(value_bb.Max.x - Arrow, mid + Arrow), arrows);
        const char* const text = Tr(value.labels[settings_engine.value[index]]);
        const ImVec2 size = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(value_bb.GetCenter().x - size.x * 0.5f, mid - size.y * 0.5f),
                    Colour(palette->Text), text);
    }
    ImGui::PopID();
}

void Ui::DrawEngineFilterRow(std::size_t index) {
    constexpr float Pad = 18.0f;

    ImGui::PushID(static_cast<int>(2000 + index));
    const Loader::EngineFilter& filter = Loader::EngineFilters()[index];
    const bool on = ((settings_engine.filters_off >> index) & 1) == 0;
    const float width = ImGui::GetContentRegionAvail().x;
    const float label_width = width - 56.0f - 3.0f * Pad;
    const char* const name = Tr(filter.name);
    const ImVec2 label = ImGui::CalcTextSize(name, nullptr, false, label_width);
    const Card card = CardItem("##engine", ImVec2(width, std::max(RowHeight, label.y + 28.0f)));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    const bool focused = ImGui::GetFocusID() == card.id;
    if (focused) {
        focused_engine =
            static_cast<int>(Loader::EngineSettingCount + Loader::EngineValueCount + index);
        focused_patch = -1;
        focused_row = -1;
    }
    if (card.pressed ||
        (focused && (ImGui::IsNavInputTest(ImGuiNavInput_DpadLeft, ImGuiInputReadMode_Pressed) ||
                     ImGui::IsNavInputTest(ImGuiNavInput_DpadRight, ImGuiInputReadMode_Pressed)))) {
        Loader::SetEngineFilterOff(settings_program_id, index, on);
        settings_engine = Loader::EngineChoicesOf(settings_program_id);
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float mid = card.bb.GetCenter().y;
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text), name,
                    nullptr, label_width);
        DrawSwitch(dl, card.bb.Max.x - Pad, mid, ((settings_engine.filters_off >> index) & 1) == 0,
                   1.0f);
    }
    ImGui::PopID();
}

void Ui::DrawEngineInfo() {
    const std::size_t index = static_cast<std::size_t>(focused_engine);
    const bool bit = index < Loader::EngineSettingCount;
    const bool value = !bit && index < Loader::EngineSettingCount + Loader::EngineValueCount;
    const char* name;
    const char* description;
    if (bit) {
        name = Loader::EngineSettings()[index].name;
        description = Loader::EngineSettings()[index].description;
    } else if (value) {
        const Loader::EngineValue& v = Loader::EngineValues()[index - Loader::EngineSettingCount];
        name = v.name;
        description = v.description;
    } else {
        const Loader::EngineFilter& f =
            Loader::EngineFilters()[index - Loader::EngineSettingCount - Loader::EngineValueCount];
        name = f.name;
        description = f.description;
    }

    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", Tr(name));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", Tr(description));
    ImGui::Spacing();

    if (bit) {
        const Loader::EngineSetting& setting = Loader::EngineSettings()[index];
        const bool own = ((settings_engine.defaults >> settings_engine.bits[index]) & 1) != 0;
        Note(palette->TextDim, own ? Tr("The game's own: on.") : Tr("The game's own: off."));
        if (setting.azahar == Loader::EngineDefault::Off) {
            Note(palette->TextDim, Tr("Azahar turns it off unless you turn it on."));
        } else if (setting.azahar == Loader::EngineDefault::On) {
            Note(palette->TextDim, Tr("Azahar turns it on unless you turn it off."));
        }
    } else if (value) {
        Note(palette->TextDim, Tr("Game default: the game sets it itself."));
    } else {
        Note(palette->TextDim, Tr("Off, the game skips drawing this effect, which is faster."));
    }
    const bool running = !settings_title.empty() && settings_title == game_key;
    Note(running ? palette->Accent : palette->TextDim,
         running ? Tr("The running game keeps what it started with: restart it to apply this.")
                 : Tr("Applied when the game starts."));
}

void Ui::StartEngineSearch() {
    if (std::exchange(engine_search_asked, true) || settings_engine.scanned ||
        settings_path.empty()) {
        return;
    }
    ASSERT_MSG(!engine_search.joinable(), "settings: an engine search already runs");
    engine_search_done.store(false, std::memory_order_relaxed);
    // On the cores the game list leaves idle; the screen keeps drawing meanwhile.
    engine_search = Common::NamedThread{
        Common::ThreadCfg{"engine search", 1024 * 1024, 1 << 0 | 1 << 1 | 1 << 3},
        [this, path = settings_path] {
            if (const auto loader = Loader::GetLoader(path)) {
                loader->ScanEngine();
            }
            engine_search_done.store(true, std::memory_order_release);
        }};
}

void Ui::DrawEngineSearch() {
    if (engine_search_done.load(std::memory_order_acquire)) {
        engine_search.join();
        settings_engine = Loader::EngineChoicesOf(settings_program_id);
        focus_first_row = true;
        return;
    }
    constexpr float Padding = 28.0f;
    constexpr float Width = 560.0f;
    constexpr float BarHeight = 12.0f;
    constexpr float BarGap = 18.0f;
    DimScreen("##engine_search_dim");
    const char* const text = Tr("Looking for this game's engine settings...");
    const ImVec2 size = ImGui::CalcTextSize(text, nullptr, false, Width - 2.0f * Padding);
    BeginCentered("##engine_search", Width, size.y + 2.0f * Padding + BarGap + BarHeight);
    ImGui::SetCursorPos(ImVec2(std::max(Padding, (Width - size.x) * 0.5f), Padding));
    ImGui::PushTextWrapPos(Width - Padding);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    // No share to show: a piece of the bar sweeps across it, a second a pass.
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetWindowPos() + ImVec2(Padding, Padding + size.y + BarGap);
    const float track = Width - 2.0f * Padding;
    const float piece = track * 0.3f;
    const float at =
        std::fmod(static_cast<float>(ImGui::GetTime()), 1.0f) * (track + piece) - piece;
    dl->AddRectFilled(min, min + ImVec2(track, BarHeight), Colour(palette->SwitchOff),
                      BarHeight * 0.5f);
    dl->AddRectFilled(ImVec2(min.x + std::max(at, 0.0f), min.y),
                      ImVec2(min.x + std::min(at + piece, track), min.y + BarHeight),
                      Colour(palette->Accent), BarHeight * 0.5f);
    ImGui::End();
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
        ImGui::TextWrapped("%s", Tr("No cheats"));
        ImGui::SetWindowFontScale(1.0f);
        ImGui::Spacing();
        Note(palette->TextDim,
             TrFormat("Put this game's Gateway codes in {}: a [name] line for each cheat, then its "
                      "code lines. Reopen this screen to see them.",
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
        Note(palette->Accent,
             running ? Tr("On: active now.") : Tr("On: applied while the game runs."));
    } else {
        Note(palette->TextDim, Tr("Off."));
    }
    Note(palette->TextDim, TrFormat("From {}.", file));
}

void Ui::DrawInstalledRow(std::size_t index) {
    constexpr float Pad = 18.0f;

    ImGui::PushID(static_cast<int>(index));
    const InstalledPart part = settings_installed[index];
    const bool armed = armed_installed == static_cast<int>(index);
    const u64 mib = (part.size + 1024 * 1024 - 1) / (1024 * 1024);
    const std::string size_text = armed          ? TrFormat("{} again to remove", ConfirmName())
                                  : part.install ? fmt::format("r{}, {} MiB", part.version, mib)
                                                 : fmt::format("v{}, {} MiB", part.version, mib);
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
        const char* const kind = Tr(part.kind);
        const ImVec2 label = ImGui::CalcTextSize(kind);
        const ImVec2 size_size = ImGui::CalcTextSize(size_text.c_str());
        dl->AddText(ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text), kind);
        dl->AddText(ImVec2(card.bb.Max.x - Pad - size_size.x, mid - size_size.y * 0.5f),
                    Colour(armed ? palette->Accent : palette->TextDim), size_text.c_str());
    }
    ImGui::PopID();
    if (!card.pressed) {
        return;
    }
    if (part.install) {
        install_requested = true;
        return;
    }
    if (!armed) {
        armed_installed = static_cast<int>(index);
        return;
    }
    const Result result =
        Service::AM::UninstallProgram(Service::FS::MediaType::SDMC, part.title_id);
    LOG_INFO(Frontend, "Removed {} {:016X}: {:08X}", part.kind, part.title_id, result.raw);
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
    // The game's removal from a CCI's screen brings the install row back.
    ScanInstalled();
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
    ImGui::TextWrapped("%s", Tr(part.kind));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    if (part.install) {
        ImGui::TextWrapped("%s", Tr("Installs the game onto the emulated SD card from this file, "
                                    "as installing a CIA of it does. The installed game shows in "
                                    "the list beside this file, and both use the same save data."));
        ImGui::Spacing();
        if (part.encrypted) {
            Note(palette->Accent, Tr("This file is encrypted: decrypt it, then install it."));
        }
        Note(palette->TextDim,
             TrFormat("Needs {} MiB on the memory card.",
                      (part.size + 1024 * 1024 - 1) / (1024 * 1024)));
        Note(palette->TextDim, TrFormat("Revision {}.", part.version));
        Note(palette->TextDim, TrFormat("Title {:016X}.", part.title_id));
        return;
    }
    const u32 high = static_cast<u32>(part.title_id >> 32);
    ImGui::TextWrapped(
        "%s", high == 0x0004000E
                  ? Tr("Installed from a CIA. Removing it takes the game back to its own version.")
              : high == 0x0004008C
                  ? Tr("Installed from a CIA. Removing it takes this content out of the game.")
                  : Tr("Installed from a CIA. Removing it takes the game off the list."));
    ImGui::Spacing();
    Note(palette->TextDim, Tr("Removing it keeps the game's save data."));
    Note(palette->TextDim, TrFormat("Version {}.", part.version));
    if (high == 0x0004000E && settings_rom_version) {
        Note(palette->TextDim, TrFormat("The game file is revision {}.", *settings_rom_version));
    }
    Note(palette->TextDim, TrFormat("Title {:016X}.", part.title_id));
}

void Ui::DrawFileRow() {
    constexpr float Pad = 18.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const Card card = CardItem("##check", ImVec2(width, RowHeight));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const char* const label = Tr("Check file");
        const ImVec2 size = ImGui::CalcTextSize(label);
        dl->AddText(ImVec2(card.bb.Min.x + Pad, card.bb.GetCenter().y - size.y * 0.5f),
                    Colour(palette->Text), label);
    }
    check_requested |= card.pressed;
}

void Ui::DrawFileInfo() {
    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", Tr("Check file"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", Tr("Reads the whole file and compares every part with the checksums "
                                "3DS files carry, to find a damaged or incomplete copy. A large "
                                "game takes a few minutes."));
    ImGui::Spacing();
    Note(palette->TextDim, std::string{FileUtil::GetFilename(settings_path)});
    if (const auto found = check_results.find(settings_path); found != check_results.end()) {
        Note(palette->Accent, found->second);
    }
}

void Ui::ScanData() {
    data_scanned = true;
    settings_data.clear();
    // The save data: the save archive's folder (the archive and its metadata; the title's
    // installed content beside it stays) and the title's extdata, where some games keep their
    // saves. Both nest past ten components: folded (CountSdFolder).
    DataPart saves{DataKind::SaveData, Translatable("Save data")};
    const auto add_saves = [&saves](std::string folder) {
        if (!FileUtil::IsDirectory(folder)) {
            return;
        }
        // A folder the game left without a file in it reads as no save data.
        const u32 before = saves.files;
        CountSdFolder(folder, saves.bytes, saves.files);
        if (saves.files != before) {
            saves.folders.push_back(std::move(folder));
        }
    };
    const std::string& sdmc = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
    const std::string archive =
        FileSys::ArchiveSource_SDSaveData::GetSaveDataPathFor(sdmc, settings_program_id);
    constexpr std::string_view ArchiveTail = "/00000001/";
    ASSERT_MSG(archive.ends_with("/data/00000001/"), "settings: save data at {}", archive);
    add_saves(archive.substr(0, archive.size() - ArchiveTail.size()));
    u64 extdata_id = 0;
    if (settings_path.empty()) {
        ASSERT_MSG(settings_back == State::Pause, "settings: title {:016X} without its file",
                   settings_program_id);
        system.GetAppLoader().ReadExtdataId(extdata_id);
    } else if (const std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(settings_path)) {
        loader->ReadExtdataId(extdata_id);
    }
    if (extdata_id != 0) {
        std::string extdata = FileSys::GetExtDataPathFromId(sdmc, extdata_id);
        extdata.pop_back();
        add_saves(std::move(extdata));
    }
    settings_data.push_back(std::move(saves));

    const auto add = [](DataPart& part, std::string folder) {
        if (ListFolder(folder)) {
            CountFolder(folder, part.bytes, part.files);
            part.folders.push_back(std::move(folder));
        }
    };

    DataPart shaders{DataKind::ShaderCache, Translatable("Shader cache")};
    const std::string prefix = fmt::format("{:016x}.", settings_program_id);
    const std::string root = SgxdRenderer::ShaderDiskCache::Root;
    for (const FolderEntry& entry : ListFolder(root).value_or(std::vector<FolderEntry>{})) {
        if (entry.folder && entry.name.starts_with(prefix)) {
            add(shaders, root + "/" + entry.name);
        }
    }
    settings_data.push_back(std::move(shaders));
}

void Ui::DrawDataRow(std::size_t index) {
    constexpr float Pad = 18.0f;

    ImGui::PushID(static_cast<int>(index));
    const DataPart& part = settings_data[index];
    const std::string size_text =
        part.folders.empty() ? std::string{Tr("Empty")} : SizeText(part.bytes);
    const float width = ImGui::GetContentRegionAvail().x;
    const Card card = CardItem("##data", ImVec2(width, RowHeight));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    if (ImGui::GetFocusID() == card.id) {
        focused_data = static_cast<int>(index);
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float mid = card.bb.GetCenter().y;
        const char* const name = Tr(part.name);
        const ImVec2 label = ImGui::CalcTextSize(name);
        const ImVec2 size = ImGui::CalcTextSize(size_text.c_str());
        dl->AddText(ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(palette->Text), name);
        dl->AddText(ImVec2(card.bb.Max.x - Pad - size.x, mid - size.y * 0.5f),
                    Colour(palette->TextDim), size_text.c_str());
    }
    ImGui::PopID();
    if (card.pressed && settings_back == State::GameList && !part.folders.empty()) {
        question = Question::ClearData;
        clear_data = static_cast<int>(index);
    }
}

void Ui::DrawDataInfo() {
    if (focused_data < 0) {
        return;
    }
    const DataPart& part = settings_data[static_cast<std::size_t>(focused_data)];
    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", Tr(part.name));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "%s", part.kind == DataKind::SaveData
                  ? Tr("What the game saved on the emulated SD card: its progress and its own "
                       "options. Deleting it starts the game over as new. Its savestates and extra "
                       "data stay.")
                  : Tr("The programs Azahar made for this game's graphics, by every version of "
                       "Azahar that ran it, kept so that the next start need not make them again. "
                       "Deleting it frees their space; the game stutters for a while as it makes "
                       "them again."));
    ImGui::Spacing();
    if (part.folders.empty()) {
        Note(palette->TextDim, Tr("Nothing to delete."));
    } else {
        Note(palette->TextDim, TrFormat("{} in {} files.", SizeText(part.bytes), part.files));
    }
    if (settings_back != State::GameList) {
        Note(palette->Accent, Tr("Quit the game to delete it."));
    }
}

std::string Ui::ClearData(const std::function<void()>& present) {
    ASSERT_MSG(clear_data >= 0 && settings_back == State::GameList,
               "settings: data {} cleared from screen {}", clear_data,
               static_cast<int>(settings_back));
    const DataPart part = settings_data[static_cast<std::size_t>(std::exchange(clear_data, -1))];
    const bool saves = part.kind == DataKind::SaveData;
    SetStatusMessage(saves ? TrFormat("Deleting {}'s save data...", settings_name)
                           : TrFormat("Deleting {}'s shader cache...", settings_name));
    SetProgress(0.0f);
    present();
    if (!saves) {
        SgxdRenderer::ShaderDiskCache::Forget(settings_program_id);
    }
    const u64 start = sceKernelGetProcessTimeWide();
    u64 shown = start;
    u32 done = 0;
    const auto removed = [&] {
        done++;
        const u64 now = sceKernelGetProcessTimeWide();
        if (now - shown < 100'000) {
            return;
        }
        shown = now;
        SetProgress(static_cast<float>(done) / static_cast<float>(std::max(part.files, done)));
        present();
    };
    u32 left = 0;
    for (const std::string& folder : part.folders) {
        left += saves ? DeleteSdFolder(folder, removed) : DeleteFolder(folder, removed);
    }
    LOG_INFO(Frontend, "Deleted {} of {:016X}: {} files, {} bytes, in {} ms; {} stayed", part.name,
             settings_program_id, done, part.bytes, (sceKernelGetProcessTimeWide() - start) / 1000,
             left);
    ClearStatusMessage();
    ScanData();
    if (left != 0) {
        return TrFormat("{} files or folders could not be deleted. Restart the Vita, then delete "
                        "them again.",
                        left);
    }
    return saves ? TrFormat("{}'s save data is deleted.", settings_name)
                 : TrFormat("{}'s shader cache is deleted.", settings_name);
}

void Ui::DrawConsoleNameRow() {
    constexpr float Pad = 18.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const Card card = CardItem("##console_name", ImVec2(width, RowHeight));
    if (ImGui::GetFocusID() == card.id) {
        focused_row = ConsoleNameRow;
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float y = card.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(card.bb.Min.x + Pad, y), Colour(palette->Text), Tr("Console name"));
        // Not set: the name the 3DS has, AZAHAR on a new one.
        const std::string name = Config::ConsoleName();
        const std::string value = name.empty() ? std::string{"AZAHAR"} : name;
        const float value_width = ImGui::CalcTextSize(value.c_str()).x;
        dl->AddText(ImVec2(card.bb.Max.x - Pad - value_width, y),
                    Colour(name.empty() ? palette->TextDim : palette->Text), value.c_str());
    }
    if (card.pressed) {
        OpenKeyboard(KeyboardConsoleName, Config::ConsoleName());
    }
}

void Ui::DrawConsoleNameInfo() {
    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", Tr("Console name"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", Tr("The 3DS's user name, which games show: in greetings, as a save's "
                                "owner, to other players. Up to 10 characters. Empty: AZAHAR."));
    ImGui::Spacing();
    if (!game_key.empty()) {
        Note(palette->Accent, Tr("Takes effect when a game starts."));
    }
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
            const std::string value = armed ? TrFormat("{} again to remove", ConfirmName())
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
    if (focused_row == ConsoleNameRow) {
        DrawConsoleNameInfo();
        return;
    }
    if (focused_row == RomFolderRow || focused_row == AddRomFolderRow) {
        DrawRomFolderInfo();
        return;
    }
    if (focused_row == LayoutEditorRow) {
        DrawLayoutEditorInfo();
        return;
    }
    if (focused_row == ControlEditorRow) {
        DrawControlEditorInfo();
        return;
    }
    if (focused_row < 0) {
        return;
    }
    const Row& row = rows[static_cast<std::size_t>(focused_row)];
    const bool for_title = !settings_title.empty();
    const std::optional<int> own =
        Config::TitleValue(for_title ? settings_title : game_key, row.option);

    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", Tr(row.title));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", Tr(row.description));
    ImGui::Spacing();

    if (const char* reason = DisabledReason(row)) {
        Note(palette->Accent, reason);
    }
    if (const char* problem = UvcStream::Problem();
        problem && row.option == Config::Option::UsbVideo) {
        Note(palette->Accent, Tr(problem));
    }
    const int all_games = Config::Global(row.option);
    if (for_title && own && *own != all_games) {
        Note(palette->Own,
             TrFormat("Set for this game. All games: {}.", ValueText(row, all_games)));
    } else if (for_title && own) {
        Note(palette->TextDim, Tr("Set for this game, the same as all games for now. Changing it "
                                  "for all games won't change it here."));
    } else if (for_title) {
        Note(palette->TextDim, Tr("Follows the setting for all games."));
    } else if (!game_key.empty() && own) {
        Note(*own != all_games ? palette->Own : palette->TextDim,
             TrFormat("{} sets its own: {}. Change it in Game settings.", game_name,
                      ValueText(row, *own)));
    }
    if (const std::optional<int> pin = Config::PinOf(row.option)) {
        Note(palette->Accent, TrFormat("A launch argument holds this at {} until Azahar exits.",
                                       ValueText(row, *pin)));
    }
    if (!Config::Runtime(row.option) && !game_key.empty()) {
        Note(palette->Accent, Tr("Takes effect when a game starts."));
    }
}

Ui::Action Ui::DrawSettings() {
    if (editing_field >= 0) {
        DrawKeyboard();
        return Action::None;
    }
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
                  for_title ? Tr("Game settings") : Tr("Settings"),
                  for_title ? settings_name : std::string{Tr("All games")});
    // The Patches tab changes nothing; the Cheats tab's rows are switches alone, the Installed
    // tab's are removed.
    const bool option_rows = category != Category::Patches && category != Category::Cheats &&
                             category != Category::Installed && category != Category::File &&
                             category != Category::Data;
    bool back = FooterButton(BackGlyph(), Tr("Back"));
    if (option_rows) {
        FooterButton(Glyph::LeftRight, Tr("Change"));
    } else if (category == Category::Cheats && !settings_cheats.empty()) {
        FooterButton(ConfirmGlyph(), Tr("On or off"));
    } else if (category == Category::Installed) {
        const bool install_row =
            focused_installed >= 0 &&
            settings_installed[static_cast<std::size_t>(focused_installed)].install;
        FooterButton(ConfirmGlyph(), install_row ? Tr("Install") : Tr("Remove"));
    } else if (category == Category::File && FooterButton(ConfirmGlyph(), Tr("Check"))) {
        check_requested = true;
    } else if (category == Category::Data && settings_back == State::GameList) {
        FooterButton(ConfirmGlyph(), Tr("Delete"));
    }
    if (categories.size() > 1 && FooterButton(Glyph::Shoulders, Tr("Tab")) && !next_category) {
        const int at = static_cast<int>(current - categories.begin());
        next_category = categories[static_cast<std::size_t>(at + 1) % categories.size()];
    }
    if (for_title && option_rows && FooterButton(Glyph::Triangle, Tr("Use all games' setting")) &&
        focused_row >= 0) {
        Config::SetTitleValue(settings_title, rows[static_cast<std::size_t>(focused_row)].option,
                              std::nullopt);
    }

    // The tabs, under the header: the selected one filled with the accent colour. Where they don't
    // fit across the screen, the others lose their icons, then their names (icons alone).
    {
        enum class Fit { Whole, Names, Icons };
        constexpr float Gap = 10.0f;
        const auto tab_width = [this](Category c, Fit fit) {
            const float text = ImGui::CalcTextSize(Tr(CategoryName(static_cast<int>(c)))).x;
            if (c == category || fit == Fit::Whole) {
                return text + 64.0f;
            }
            return fit == Fit::Names ? text + 32.0f : 48.0f;
        };
        Fit fit = Fit::Whole;
        for (const Fit tried : {Fit::Whole, Fit::Names, Fit::Icons}) {
            fit = tried;
            float total = -Gap;
            for (const Category c : categories) {
                total += tab_width(c, fit) + Gap;
            }
            if (total <= ScreenWidth - 2.0f * Margin) {
                break;
            }
        }
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        float x = Margin;
        const float y = HeaderHeight + 10.0f;
        for (const Category c : categories) {
            const int i = static_cast<int>(c);
            const char* name = Tr(CategoryName(i));
            const ImVec2 text = ImGui::CalcTextSize(name);
            const ImVec2 size(tab_width(c, fit), TabsHeight);
            ImGui::SetCursorScreenPos(ImVec2(x, y));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", size)) {
                next_category = c;
            }
            ImGui::PopID();
            const bool selected = c == category;
            const bool icon = selected || fit != Fit::Names;
            const bool named = selected || fit != Fit::Icons;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x, y) + size,
                              Colour(selected ? palette->Accent : palette->Row), size.y * 0.5f);
            const ImU32 ink = Colour(selected ? palette->Backdrop : palette->Text);
            if (icon) {
                DrawIcon(dl, CategoryIcon(i),
                         ImVec2(x + (named ? 26.0f : size.x * 0.5f), y + size.y * 0.5f), 22.0f, ink,
                         Colour(selected ? palette->Accent : palette->Row));
            }
            if (named) {
                dl->AddText(ImVec2(x + (icon ? 46.0f : 16.0f), y + (size.y - text.y) * 0.5f), ink,
                            name);
            }
            x += size.x + Gap;
        }
    }
    EndBackdrop();

    if (next_category && *next_category != category) {
        category = *next_category;
        focused_row = -1;
        focused_patch = -1;
        focused_engine = -1;
        focused_cheat = -1;
        focused_installed = armed_installed = -1;
        focused_rom_folder = armed_rom_folder = -1;
        focused_data = -1;
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
        // The options a title's patches offer, as its own values: each where the title has its bit.
        for (std::size_t i = 0; i < rows.size(); i++) {
            if (rows[i].category != Category::Patches) {
                continue;
            }
            const u32 bit = rows[i].option == Config::Option::EvenPacing ? Loader::TuningEvenPacing
                                                                         : Loader::TuningHudScale;
            if ((Loader::TitleTuningOf(settings_program_id) & bit) != 0) {
                DrawRow(rows[i], i);
            }
        }
        for (std::size_t i = 0; settings_engine.found && i < Loader::EngineSettings().size(); i++) {
            if (settings_engine.bits[i] >= 0) {
                DrawEngineRow(i);
            }
        }
        for (std::size_t i = 0; settings_engine.found && i < Loader::EngineValues().size(); i++) {
            if ((settings_engine.values >> i) & 1) {
                DrawEngineValueRow(i);
            }
        }
        for (std::size_t i = 0; settings_engine.found && i < Loader::EngineFilters().size(); i++) {
            if ((settings_engine.filters >> i) & 1) {
                DrawEngineFilterRow(i);
            }
        }
    } else if (category == Category::Cheats) {
        for (std::size_t i = 0; i < settings_cheats.size(); i++) {
            DrawCheatRow(i);
        }
    } else if (category == Category::Installed) {
        for (std::size_t i = 0; i < settings_installed.size() && state == State::Settings; i++) {
            DrawInstalledRow(i);
        }
    } else if (category == Category::File) {
        DrawFileRow();
    } else if (category == Category::Data) {
        if (!data_scanned) {
            ScanData();
        }
        for (std::size_t i = 0; i < settings_data.size(); i++) {
            DrawDataRow(i);
        }
    } else {
        for (std::size_t i = 0; i < rows.size(); i++) {
            const Row& row = rows[i];
            if (row.category == category && (!for_title || Config::PerTitle(row.option))) {
                DrawRow(row, i);
                if (row.option == Config::Option::ControlLayout) {
                    DrawControlEditorRow();
                }
            }
        }
        if (category == Category::System && !for_title) {
            DrawConsoleNameRow();
            DrawRomFolderRows();
        }
        if (category == Category::Display) {
            DrawLayoutEditorRow();
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
        if (focused_row >= 0) {
            DrawRowInfo();
        } else if (focused_engine >= 0) {
            DrawEngineInfo();
        } else {
            DrawPatchInfo();
        }
    } else if (category == Category::Cheats) {
        DrawCheatInfo();
    } else if (category == Category::Installed) {
        DrawInstalledInfo();
    } else if (category == Category::File) {
        DrawFileInfo();
    } else if (category == Category::Data) {
        DrawDataInfo();
    } else {
        DrawRowInfo();
    }
    // A description longer than the panel: the right stick scrolls it, from its top for each row.
    const std::array<int, 8> shown{static_cast<int>(category),
                                   focused_row,
                                   focused_patch,
                                   focused_engine,
                                   focused_cheat,
                                   focused_installed,
                                   focused_rom_folder,
                                   focused_data};
    if (shown != info_shown) {
        info_shown = shown;
        ImGui::SetScrollY(0.0f);
    } else if (const float stick = ImGui_ImplVita_RightStickY(); stick != 0.0f && !blacked_out) {
        ImGui::SetScrollY(ImGui::GetScrollY() + stick * 600.0f * ImGui::GetIO().DeltaTime);
    }
    ImGui::End();

    if (back) {
        // What changed is kept now, not at the title's end: a crash or a power-off would lose it.
        Config::Save();
        state = settings_back;
        overlay_focused = false;
        focus_first_row = true;
    }
    if (std::exchange(check_requested, false) && !back) {
        check_path = settings_path;
        check_name = settings_name;
        return Action::CheckFile;
    }
    if (std::exchange(install_requested, false) && !back) {
        install_rom_path = settings_path;
        install_rom_name = settings_name;
        return Action::InstallTitle;
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
                      games.empty()       ? std::string{}
                      : games.size() == 1 ? TrFormat("{} title", games.size())
                                          : TrFormat("{} titles", games.size()));
        dl->PopClipRect();

        if (games.empty()) {
            const std::string text =
                TrFormat("No titles found. Copy .3ds or .cci files into {} or a ROM folder of the "
                         "settings' System tab, or choose Install CIA.",
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
    bool start =
        !games.empty() && FooterButton(ConfirmGlyph(), installer ? Tr("Install") : Tr("Open"));
    bool game_settings =
        !games.empty() && !installer && FooterButton(Glyph::Triangle, Tr("Game settings"));
    // A CIA's triangle checks its file: it has no settings of its own.
    bool check = !games.empty() && installer && FooterButton(Glyph::Triangle, Tr("Check file"));
    bool pin = false;
    if (!games.empty() && !installer) {
        const std::string& path = games[static_cast<std::size_t>(selected_game)].path;
        if (pin_checked_path != path) {
            pin_checked_path = path;
            pin_checked = LiveArea::IsPinned(path);
        }
        pin = FooterButton(Glyph::Square, pin_checked ? Tr("Unpin") : Tr("Pin to LiveArea"));
    }
    ImGui::PopItemFlag();

    // The tools at the footer's right, which the pad reaches down from the grid's lower row.
    constexpr float ToolGap = 6.0f;
    ImGui::SetCursorScreenPos(ImVec2(ScreenWidth - Margin - BarButtonWidth(Tr("Settings")) -
                                         BarButtonWidth(Tr("Install CIA")) -
                                         BarButtonWidth(Tr("Exit")) - 2.0f * ToolGap,
                                     bar_y));
    // The pad's selection on a tool, however it got there: a touch leaves ImGui's highlight off,
    // and IsItemFocused asks for that too.
    const auto tool_selected = [] {
        return ImGui::GetFocusID() == ImGui::GetCurrentWindow()->DC.LastItemId;
    };
    bool on_tools = false;
    if (BarButton(Tr("Settings"), Icon::Gear)) {
        OpenSettings({}, {}, State::GameList);
    }
    on_tools |= tool_selected();
    ImGui::SameLine(0.0f, ToolGap);
    if (BarButton(Tr("Install CIA"), Icon::Install)) {
        OpenPickerDir(picker_dir.empty() ? std::string{GameDirectory} : picker_dir);
        state = State::InstallPicker;
        focus_first_row = true;
    }
    on_tools |= tool_selected();
    ImGui::SameLine(0.0f, ToolGap);
    if (BarButton(Tr("Exit"), Icon::Power)) {
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
        const std::string publisher = game.installer ? Fit(Tr("CIA file: install it to play"), room)
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
        check |= game_settings;
        game_settings = pin = false;
    }
    if (game_settings) {
        OpenSettings(Config::TitleKeyOf(game.path), game.name, State::GameList,
                     ProgramIdOf(game.path), game.path);
    } else if (check) {
        check_path = game.path;
        check_name = game.name;
        action = Action::CheckFile;
    } else if (pin) {
        // Asked first: the square sits beside the buttons players press all the time.
        selected_game_path = game.path;
        question = Question::Pin;
        question_game = game.name;
        question_pinned = LiveArea::IsPinned(game.path);
    } else if (start) {
        selected_game_path = game.path;
        if (!game.installer && Config::Global(Config::Option::UsbVideo) != 0 &&
            !UvcStream::TakeoverAllowed() && UvcStream::PortTaken()) {
            question = Question::UsbLaunch; // the answer starts the game
        } else {
            action = game.installer ? Action::InstallTitle : Action::LaunchGame;
        }
    }
    return action;
}

/// What to free on the memory card the emulated SD card is on for `needed` bytes to fit; none
/// when they fit. A title that does not fit would stop partway, minutes in.
static std::optional<std::string> MemoryCardShortOf(u64 needed, const std::string& what) {
    const std::string sdmc = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
    const std::string device = sdmc.substr(0, sdmc.find(':') + 1);
    struct {
        u64 max_size;
        u64 free_size;
        u32 cluster_size;
        u32 unknown;
    } space{};
    if (sceIoDevctl(device.c_str(), 0x3001, nullptr, 0, &space, sizeof(space)) < 0 ||
        space.free_size >= needed) {
        return std::nullopt;
    }
    LOG_ERROR(Frontend, "{} needs {} bytes on {}, {} are free", what, needed, device,
              space.free_size);
    constexpr u64 MiB = 1024 * 1024;
    return TrFormat("Free {} MiB on the memory card, then install it again.",
                    (needed - space.free_size + MiB - 1) / MiB);
}

std::string Ui::InstallRom(const std::function<void()>& present) {
    const std::string path = std::exchange(install_rom_path, {});
    const auto info = Service::AM::ReadCCIInstallInfo(path);
    if (!info) {
        LOG_ERROR(Frontend, "{} does not read as a CCI", path);
        return Tr("This game file cannot be read. Make it again from your game.");
    }
    if (info->encrypted) {
        return Tr("Decrypt this game, then install it again.");
    }
    if (auto short_of = MemoryCardShortOf(info->content_size, path)) {
        return *std::move(short_of);
    }
    const std::string name = std::exchange(install_rom_name, {});
    SetStatusMessage(TrFormat("Installing {}...", name));
    SetProgress(0.0f);
    present();
    const u64 start = sceKernelGetProcessTimeWide();
    u64 shown = start;
    const Service::AM::InstallStatus status =
        Service::AM::InstallCCI(path, [&](std::size_t done, std::size_t total) {
            const u64 now = sceKernelGetProcessTimeWide();
            if (now - shown < 100'000 || total == 0) {
                return;
            }
            shown = now;
            SetProgress(static_cast<float>(done) / static_cast<float>(total));
            present();
        });
    LOG_INFO(Frontend, "Install of {} as {:016X} v{} ({} bytes): status {} after {} ms", path,
             info->title_id, info->version, info->content_size, static_cast<u32>(status),
             (sceKernelGetProcessTimeWide() - start) / 1000);
    // A stopped install takes its contents back off: the tab reads again either way.
    ScanGames();
    if (state == State::Settings) {
        ScanInstalled();
        focused_installed = armed_installed = -1;
        focus_first_row = true;
    }
    switch (status) {
    case Service::AM::InstallStatus::Success:
        return TrFormat("{} is installed.", name);
    case Service::AM::InstallStatus::ErrorEncrypted:
        return Tr("Decrypt this game, then install it again.");
    case Service::AM::InstallStatus::ErrorAborted:
        return Tr("The install stopped. Install the game again.");
    case Service::AM::InstallStatus::ErrorInvalid:
        return Tr("This game file cannot be read. Make it again from your game.");
    default:
        return Tr("This game file is gone.");
    }
}

std::string Ui::InstallSelected(const std::function<void()>& present) {
    if (!install_rom_path.empty()) {
        return InstallRom(present);
    }
    GameEntry entry;
    if (!picked_cia.empty()) {
        // The Install CIA screen's: its file's name until the CIA's SMDH names it.
        entry.path = std::exchange(picked_cia, {});
        entry.name = std::string{FileUtil::GetFilename(entry.path)};
    } else if (games.empty() || !games[static_cast<std::size_t>(selected_game)].installer) {
        return Tr("Choose a CIA to install.");
    } else {
        entry = games[static_cast<std::size_t>(selected_game)];
    }
    FileSys::CIAContainer container;
    if (!ReadCia(entry.path, container)) {
        LOG_ERROR(Frontend, "{} does not read as a CIA", entry.path);
        return Tr("This CIA cannot be read. Make it again from your game.");
    }
    const u64 title_id = container.GetTitleMetadata().GetTitleID();
    // The contents against the memory card's free space.
    const u64 needed = container.GetTitleMetadata().GetCombinedContentSize(container.GetHeader());
    if (auto short_of = MemoryCardShortOf(needed, entry.path)) {
        return *std::move(short_of);
    }

    SetStatusMessage(TrFormat("Installing {}...", entry.name));
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
        return Tr("Decrypt this CIA, then install it again.");
    case Service::AM::InstallStatus::ErrorInvalid:
        return Tr("This CIA cannot be read. Make it again from your game.");
    case Service::AM::InstallStatus::ErrorAborted:
        return Tr("The install stopped. Install the CIA again.");
    default:
        return Tr("This CIA is gone. Choose it again with Install CIA.");
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
        return TrFormat("{}: its update is installed.", entry.name);
    case 0x0004008C:
        return TrFormat("{}: its downloadable content is installed.", entry.name);
    default:
        return TrFormat("{} is installed.", entry.name);
    }
}

std::string Ui::CheckSelected(const std::function<void()>& present) {
    std::string path = std::exchange(check_path, {});
    std::string name = std::exchange(check_name, {});
    if (path.empty()) {
        if (games.empty()) {
            return Tr("Choose a game to check.");
        }
        const GameEntry& game = games[static_cast<std::size_t>(selected_game)];
        path = game.path;
        name = game.name;
    }
    SetStatusMessage(TrFormat("Checking {}...", name));
    SetProgress(0.0f);
    present();
    const RomCheck::Result result = RomCheck::Check(path, [&](float done) {
        SetProgress(done);
        present();
    });
    ClearStatusMessage();
    std::string text;
    switch (result.outcome) {
    case RomCheck::Outcome::Intact:
        text = TrFormat("{} is intact: every part matches its checksum.", name);
        break;
    case RomCheck::Outcome::Damaged:
        text = TrFormat("{} is damaged in {}. Copy it again from your game.", name,
                        Tr(result.part));
        break;
    case RomCheck::Outcome::CutShort:
        text = TrFormat("{} is incomplete: the file ends early. Copy it again from your game.",
                        name);
        break;
    case RomCheck::Outcome::Encrypted:
        text = TrFormat("{} is encrypted, so it can't be checked. Decrypt it first.", name);
        break;
    case RomCheck::Outcome::NoChecksums:
        text = TrFormat("{} can't be checked: its file type carries no checksums.", name);
        break;
    case RomCheck::Outcome::Unreadable:
        text = TrFormat("{} can't be read as a 3DS game.", name);
        break;
    }
    check_results[path] = text;
    RememberCheck(path, result);
    return text;
}

const Ui::CheckRecord* Ui::RememberedCheck(const std::string& path) {
    if (!check_records_loaded) {
        check_records_loaded = true;
        if (std::FILE* const f = std::fopen(CheckRecordFile, "rb")) {
            char line[1024];
            while (std::fgets(line, sizeof(line), f) != nullptr) {
                // size, outcome, part, path; tab-separated.
                std::string text{line};
                if (!text.empty() && text.back() == '\n') {
                    text.pop_back();
                }
                const auto t1 = text.find('\t');
                const auto t2 = t1 == std::string::npos ? t1 : text.find('\t', t1 + 1);
                const auto t3 = t2 == std::string::npos ? t2 : text.find('\t', t2 + 1);
                if (t3 == std::string::npos) {
                    continue;
                }
                check_records[text.substr(t3 + 1)] = {
                    std::strtoull(text.c_str(), nullptr, 10),
                    static_cast<RomCheck::Outcome>(std::atoi(text.c_str() + t1 + 1)),
                    text.substr(t2 + 1, t3 - t2 - 1)};
            }
            std::fclose(f);
        }
    }
    const auto found = check_records.find(path);
    // A file changed since (another copy over it) has no check of its own yet.
    return found != check_records.end() && found->second.size == FileUtil::GetSize(path)
               ? &found->second
               : nullptr;
}

void Ui::RememberCheck(const std::string& path, const RomCheck::Result& result) {
    RememberedCheck(path); // the file's records read first
    check_records[path] = {FileUtil::GetSize(path), result.outcome,
                           result.part != nullptr ? result.part : ""};
    std::FILE* const f = std::fopen(CheckRecordFile, "wb");
    if (f == nullptr) {
        LOG_WARNING(Frontend, "Could not write {}", CheckRecordFile);
        return;
    }
    for (const auto& [file, record] : check_records) {
        std::fprintf(f, "%llu\t%d\t%s\t%s\n", static_cast<unsigned long long>(record.size),
                     static_cast<int>(record.outcome), record.part.c_str(), file.c_str());
    }
    std::fclose(f);
}

bool Ui::LaunchChecked(const std::string& path, const std::function<void()>& present) {
    if (std::exchange(launch_anyway, false)) {
        return true;
    }
    const auto listed = std::find_if(games.begin(), games.end(),
                                     [&](const GameEntry& game) { return game.path == path; });
    const std::string name =
        listed != games.end() ? listed->name : std::string{FileUtil::GetFilename(path)};
    SetStatusMessage(TrFormat("Checking {}...", name));
    present();
    RomCheck::Result result = RomCheck::QuickCheck(path);
    ClearStatusMessage();
    // A full check found what the quick one does not read (the RomFS's data).
    if (const CheckRecord* record = RememberedCheck(path);
        record != nullptr && result.outcome == RomCheck::Outcome::Intact &&
        (record->outcome == RomCheck::Outcome::Damaged ||
         record->outcome == RomCheck::Outcome::CutShort)) {
        result.outcome = record->outcome;
        result.part = record->part.c_str();
    }
    if (result.outcome != RomCheck::Outcome::Damaged &&
        result.outcome != RomCheck::Outcome::CutShort) {
        return true;
    }
    LOG_INFO(Frontend, "launch check of {}: {} {}", path,
             result.outcome == RomCheck::Outcome::Damaged ? "damaged in" : "cut short",
             result.part != nullptr ? result.part : "");
    selected_game_path = path;
    question = Question::Damaged;
    question_game = name;
    damaged_text =
        result.outcome == RomCheck::Outcome::Damaged
            ? TrFormat("{} might be corrupted: {} doesn't match its checksum. It can crash or "
                       "misbehave. Launch it anyway?",
                       name, Tr(result.part))
            : TrFormat("{} might be corrupted: the file ends early. It can crash or misbehave. "
                       "Launch it anyway?",
                       name);
    return false;
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
        dl->AddText(ImGui::GetFont(), TitleSize, at, Colour(palette->Text), Tr("Paused"));
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
    if (tile(Tr("Resume"), Icon::Play)) {
        action = Action::ResumeGame;
    }
    // Opened, or back from one of its screens: Resume selected, so that the pad's first press
    // moves from it rather than only showing where the selection is.
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    if (tile(Tr("Game settings"), Icon::Cartridge)) {
        u64 program_id = 0;
        system.GetAppLoader().ReadProgramId(program_id);
        OpenSettings(game_key, game_name, State::Pause, program_id);
    }
    // The settings for all games are the game list's alone: in a game, its own.
    if (tile(Tr("Save states"), Icon::Disk)) {
        // The slots as the card holds them now (a save since adds one).
        u64 program_id = 0;
        system.GetAppLoader().ReadProgramId(program_id);
        slot_states = Core::ListSaveStates(program_id, system.Movie().GetCurrentMovieID());
        state = State::Savestates;
        overlay_focused = false;
        focus_first_row = true;
    }
    if (tile(Tr("Multiplayer"), Icon::People)) {
        state = State::Multiplayer;
        room_fields = Multiplayer::CurrentFields();
        editing_field = -1;
        overlay_focused = false;
        focus_first_row = true;
    }
#ifdef VITA_DIAGNOSTICS
    if (tile(Tr("Dump frame"), Icon::Camera)) {
        // The next presented frame, without the menu, to ux0:data/azahar/frame_NN.ppm.
        SgxdPresent::DumpNextFrame();
    }
#endif
    if (tile(Tr("Quit game"), Icon::Home)) {
        question = Question::QuitGame;
    }
    if (tile(Tr("Exit Azahar"), Icon::Power)) {
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
    BeginBackdrop("##savestates_backdrop", Icon::Disk, Tr("Save states"), game_name);
    bool back = FooterButton(BackGlyph(), Tr("Back"));
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
        std::string info = Tr("Empty");
        if (saved != slot_states.end()) {
            info = LocalTimeText(saved->time);
            if (saved->status == Core::SaveStateInfo::ValidationStatus::BuildMismatch) {
                info += "  ";
                info += Tr("(another version's: can't be loaded)");
            }
        }
        const float text_y = at.y + (52.0f - ImGui::GetFontSize()) * 0.5f;
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(at.x + 18.0f, text_y), Colour(palette->Text),
                    TrFormat("Slot {}", slot).c_str());
        dl->AddText(ImVec2(at.x + 110.0f, text_y),
                    Colour(saved != slot_states.end() ? palette->Text : palette->TextDim),
                    info.c_str());
        ImGui::SetCursorScreenPos(at + ImVec2(width - 2.0f * button.x - 16.0f, 6.0f));
        if (ImGui::Button(Tr("Save"), button)) {
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
        if (ImGui::Button(Tr("Load"), button) && loadable) {
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
    BeginBackdrop("##picker_backdrop", picker_folders ? Icon::Disk : Icon::Install,
                  picker_folders ? Tr("Add ROM folder") : Tr("Install CIA"),
                  picker_dir.empty() ? std::string{Tr("Memory devices")} : picker_dir);
    bool back = FooterButton(BackGlyph(), picker_dir.empty() ? Tr("Back") : Tr("Parent folder"));
    const bool open = FooterButton(ConfirmGlyph(), Tr("Open"));
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
        ImGui::TextColored(palette->TextDim, "%s",
                           picker_folders ? Tr("No folders or titles here.")
                                          : Tr("No folders or CIA files here."));
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
    BeginBackdrop("##multiplayer_backdrop", Icon::People, Tr("Multiplayer"), game_name);
    bool back = FooterButton(BackGlyph(), Tr("Back"));
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
        {Tr("Address"), &room_fields.address},
        {Tr("Port"), &port},
        {Tr("Nickname"), &room_fields.nickname},
        {Tr("Password"), &password},
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
            const std::string value =
                Fit(empty ? std::string{Tr("(none)")} : *fields[i].second, width - 190.0f);
            const float value_width = ImGui::CalcTextSize(value.c_str()).x;
            dl->AddText(ImVec2(card.bb.Max.x - 18.0f - value_width, y),
                        Colour(empty ? palette->TextDim : palette->Text), value.c_str());
        }
        if (card.pressed) {
            OpenKeyboard(i, i == 1 ? port : i == 3 ? room_fields.password : *fields[i].second);
        }
        ImGui::PopID();
    }
    ImGui::Spacing();
    const ImVec2 tool(180.0f, 48.0f);
    const bool can_join = !room_fields.address.empty() && !room_fields.nickname.empty();
    if (IconCard(Multiplayer::InRoom() ? Tr("Join again") : Tr("Join"), Icon::People, tool) &&
        can_join) {
        Multiplayer::Join(system, room_fields);
    }
    if (Multiplayer::InRoom() || Adhoc::Active()) {
        ImGui::SameLine();
        if (IconCard(Tr("Leave"), Icon::Home, tool)) {
            if (Adhoc::Active()) {
                Adhoc::Leave();
            } else {
                Multiplayer::Leave();
            }
        }
    }
    // Ad hoc play: no access point and no room elsewhere, one Vita hosts and the others join it.
    const bool can_adhoc = !room_fields.nickname.empty() && !Adhoc::Active();
    if (IconCard(Tr("Host ad hoc"), Icon::Globe, tool) && can_adhoc) {
        Adhoc::Host(system, room_fields.nickname);
    }
    ImGui::SameLine();
    if (IconCard(Tr("Join ad hoc"), Icon::Globe, tool) && can_adhoc) {
        Adhoc::Join(system, room_fields.nickname);
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
    const char* const heading = Multiplayer::InRoom() ? Tr("In a room") : Tr("Not in a room");
    ImGui::TextWrapped("%s", heading);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    // The status says more than the heading only while joining, connected or after an error.
    if (const std::string status = Multiplayer::Status(); status != heading) {
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::Spacing();
    }
    if (Adhoc::Active()) {
        ImGui::TextWrapped("%s", Adhoc::Status().c_str());
        ImGui::Spacing();
    }
    const std::vector<std::string> members = Multiplayer::Members();
    if (members.empty()) {
        ImGui::TextWrapped(
            "%s", Tr("Join a room hosted by Azahar on a PC (Multiplayer > Host Room) or a room "
                     "server, or play ad hoc with Vitas nearby: one hosts, the others join. Then "
                     "everyone plays the same game in its local wireless mode."));
    }
    for (const std::string& member : members) {
        Note(palette->Accent, member);
    }
    if (!can_join) {
        Note(palette->TextDim, Tr("Enter an address and a nickname to join."));
    }
    ImGui::End();

    if (back) {
        state = State::Pause;
        overlay_focused = false;
        focus_first_row = true;
    }
    return Action::None;
}

void Ui::OpenKeyboard(int field, std::string text) {
    editing_field = field;
    edit_text = text;
    edit_original = std::move(text);
    keyboard_error.clear();
    keyboard_shift = false;
}

void Ui::CloseKeyboard() {
    editing_field = -1;
    keyboard_error.clear();
    focus_first_row = true;
    overlay_focused = false;
}

void Ui::DrawKeyboard() {
    static constexpr const char* key_rows[] = {"1234567890", "qwertyuiop", "asdfghjkl.",
                                               "zxcvbnm-_:"};
    static constexpr const char* titles[] = {Translatable("Address"),  Translatable("Port"),
                                             Translatable("Nickname"), Translatable("Password"),
                                             Translatable("Console name"),
                                             Translatable("Layout name"),
                                             Translatable("Layout name")};
    const std::size_t limit = editing_field == KeyboardConsoleName ? Config::MaxConsoleName
                              : editing_field == KeyboardLayoutName
                                  ? ScreenLayouts::MaxNameLength
                              : editing_field == KeyboardControlName
                                  ? ControlLayouts::MaxNameLength
                                  : std::size_t{64};
    if (std::exchange(keyboard_refocus, false)) {
        ImGui::SetNextWindowFocus();
    }
    const std::string before = edit_text;
    // Ten keys of 56 a row and their spacing.
    BeginCentered("##keyboard", 700.0f, 420.0f);
    ImGui::Text("%s", Tr(titles[editing_field]));
    ImGui::Separator();
    ImGui::Text("%s_", editing_field == 3 ? std::string(edit_text.size(), '*').c_str()
                                          : edit_text.c_str());
    // The names with a limit of their own show how much of it is used: keys past it do nothing.
    if (editing_field == KeyboardConsoleName || editing_field == KeyboardLayoutName ||
        editing_field == KeyboardControlName) {
        ImGui::SameLine();
        ImGui::TextDisabled("%zu/%zu", edit_text.size(), limit);
    }
    if (!keyboard_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, palette->Accent);
        ImGui::TextUnformatted(keyboard_error.c_str());
        ImGui::PopStyleColor();
    }
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
                edit_text.size() < limit) {
                edit_text.push_back(ch);
            }
        }
    }
    bool done = false, cancel = false;
    // On, the key in the accent colour: one label keeps its focus as it changes.
    if (keyboard_shift) {
        ImGui::PushStyleColor(ImGuiCol_Button, palette->Accent);
        ImGui::PushStyleColor(ImGuiCol_Text, palette->Backdrop);
    }
    const bool shift = ImGui::Button(Tr("Shift"), ImVec2(110.0f, 44.0f));
    if (keyboard_shift) {
        ImGui::PopStyleColor(2);
    }
    if (shift) {
        keyboard_shift = !keyboard_shift;
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr("Space"), ImVec2(110.0f, 44.0f)) && editing_field != 1 &&
        edit_text.size() < limit) {
        edit_text.push_back(' ');
    }
    ImGui::SameLine();
    if ((ImGui::Button(Tr("Delete"), ImVec2(110.0f, 44.0f)) ||
         ImGui::IsNavInputTest(ImGuiNavInput_Menu, ImGuiInputReadMode_Repeat)) &&
        !edit_text.empty()) {
        edit_text.pop_back();
    }
    ImGui::SameLine();
    cancel = ImGui::Button(Tr("Cancel"), ImVec2(110.0f, 44.0f)) || BackPressed();
    ImGui::SameLine();
    done = ImGui::Button(Tr("OK"), ImVec2(110.0f, 44.0f));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", Tr("Square: delete"));
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    if (edit_text != before) {
        keyboard_error.clear();
    }
    if (done && editing_field == 1) {
        if (const int port = std::atoi(edit_text.c_str()); port <= 0 || port > 65535) {
            keyboard_error = Tr("Enter a port from 1 to 65535.");
            done = false;
        }
    }
    if (cancel && edit_text != edit_original) {
        question = Question::DiscardText;
        return;
    }
    if (done) {
        switch (editing_field) {
        case 0:
            room_fields.address = edit_text;
            break;
        case 1:
            room_fields.port = static_cast<u16>(std::atoi(edit_text.c_str()));
            break;
        case 2:
            room_fields.nickname = edit_text;
            break;
        case KeyboardConsoleName:
            Config::SetConsoleName(edit_text);
            break;
        case KeyboardLayoutName:
            editor.name = edit_text;
            break;
        case KeyboardControlName:
            control_editor.name = edit_text;
            break;
        default:
            room_fields.password = edit_text;
            break;
        }
    }
    if (done || cancel) {
        CloseKeyboard();
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

Ui::Action Ui::DrawQuestion() {
    constexpr float Padding = 28.0f;
    constexpr float Width = 640.0f;
    const bool damaged = question == Question::Damaged;
    // A third answer between the two: a damaged file's full check, or the game started without
    // USB video.
    const bool three = damaged || question == Question::UsbLaunch;
    const ImVec2 button(three ? 180.0f : 200.0f, 48.0f);
    std::string text;
    const char* yes;
    switch (question) {
    case Question::Damaged:
        text = damaged_text;
        yes = Tr("Launch anyway");
        break;
    case Question::Pin:
        text = question_pinned
                   ? TrFormat("Take {}'s bubble off the LiveArea?", question_game)
                   : TrFormat("Pin {} to the LiveArea as a bubble of its own?", question_game);
        yes = question_pinned ? Tr("Unpin") : Tr("Pin");
        break;
    case Question::QuitGame:
        text = TrFormat("Quit {}? Progress since the game last saved is lost.", game_name);
        yes = Tr("Quit game");
        break;
    case Question::ExitApplication:
        text = TrFormat("Exit Azahar? {}'s progress since it last saved is lost.", game_name);
        yes = Tr("Exit Azahar");
        break;
    case Question::SaveOver:
    case Question::LoadSlot: {
        const auto saved = std::find_if(
            slot_states.begin(), slot_states.end(),
            [this](const Core::SaveStateInfo& s) { return s.slot == selected_slot; });
        ASSERT_MSG(saved != slot_states.end(), "savestates: slot {} asked about is empty",
                   selected_slot);
        const std::string when = LocalTimeText(saved->time);
        if (question == Question::SaveOver) {
            text = TrFormat("Save over slot {}? Its state from {} is replaced.", selected_slot,
                            when);
            yes = Tr("Save");
        } else {
            text = TrFormat("Load slot {}? The game goes back to {}, and the progress since then "
                            "is lost.",
                            selected_slot, when);
            yes = Tr("Load");
        }
        break;
    }
    case Question::DiscardText:
        text = Tr("Discard what you typed?");
        yes = Tr("Discard");
        break;
    case Question::DiscardLayout:
        text = Tr("Leave the layout editor? The changes are not saved.");
        yes = Tr("Leave");
        break;
    case Question::DiscardControls:
        text = Tr("Leave the control layout editor? The changes are not saved.");
        yes = Tr("Leave");
        break;
    case Question::DeleteControls: {
        const auto layout =
            control_editor.index ? ControlLayouts::Get(*control_editor.index) : std::nullopt;
        text = TrFormat("Delete the control layout {}? The games that use it go back to the "
                        "buttons in the Controls tab.",
                        layout ? layout->name : std::string{});
        yes = Tr("Delete");
        break;
    }
    case Question::DeleteLayout: {
        const auto layout = editor.index ? ScreenLayouts::Get(*editor.index) : std::nullopt;
        text = TrFormat("Delete the layout {}? The games that use it go back to Default.",
                        layout ? layout->name : std::string{});
        yes = Tr("Delete");
        break;
    }
    case Question::Stuck:
        text = TrFormat("{} seems to be stuck. Turn off Multicore in the game's settings, then "
                        "restart the game.",
                        game_name);
        yes = nullptr; // Dismiss alone
        break;
    case Question::ClearData:
        text = settings_data[static_cast<std::size_t>(clear_data)].kind == DataKind::SaveData
                   ? TrFormat("Delete {}'s save data? The game starts over as new. This can't be "
                              "undone.",
                              settings_name)
                   : TrFormat("Delete {}'s shader cache? The game stutters for a while as it "
                              "makes its shaders again.",
                              settings_name);
        yes = Tr("Delete");
        break;
    case Question::UsbLaunch:
        text = Tr("Another plugin, such as vitastream, is using the USB port. Take it over for "
                  "USB video? That plugin stops working until you reboot. Play without turns USB "
                  "video off.");
        yes = Tr("Take over");
        break;
    default:
        text = Tr("Another plugin, such as vitastream, is using the USB port. Take it over for "
                  "USB video? That plugin stops working until you reboot.");
        yes = Tr("Take over");
        break;
    }

    DimScreen("##question_dim");

    const ImVec2 size = ImGui::CalcTextSize(text.c_str(), nullptr, false, Width - 2.0f * Padding);
    ImGui::SetNextWindowFocus();
    BeginCentered("##question", Width, size.y + 3.0f * Padding + button.y);
    ImGui::SetCursorPos(ImVec2(Padding, Padding));
    ImGui::PushTextWrapPos(Width - Padding);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    const float buttons = yes == nullptr ? button.x
                          : three        ? 3.0f * button.x + 2.0f * 16.0f
                                         : 2.0f * button.x + 20.0f;
    ImGui::SetCursorPos(ImVec2((Width - buttons) * 0.5f, 2.0f * Padding + size.y));
    bool agreed = false;
    if (yes != nullptr) {
        agreed = ImGui::Button(yes, button);
        ImGui::SameLine(0.0f, three ? 16.0f : 20.0f);
    }
    bool middle = false;
    if (three) {
        middle = ImGui::Button(damaged ? Tr("Full check") : Tr("Play without"), button);
        ImGui::SameLine(0.0f, 16.0f);
    }
    const bool cancel =
        ImGui::Button(yes == nullptr ? Tr("Dismiss") : Tr("Cancel"), button) || BackPressed();
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetItemDefaultFocus(); // Cancel
    }
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    if (!agreed && !cancel && !middle) {
        return Action::None;
    }
    const Question asked = question;
    question = Question::None;
    overlay_focused = false;
    if (asked == Question::Damaged) {
        if (middle) {
            check_path = selected_game_path;
            check_name = question_game;
            return Action::CheckFile;
        }
        launch_anyway = agreed;
        return agreed ? Action::LaunchGame : Action::None;
    }
    if (asked == Question::Pin) {
        return agreed ? Action::TogglePin : Action::None;
    }
    if (asked == Question::Stuck) {
        return Action::ResumeGame; // dismissed: back to the game
    }
    if (asked == Question::DiscardText) {
        if (agreed) {
            CloseKeyboard();
        } else {
            keyboard_refocus = true;
        }
        return Action::None;
    }
    if (asked == Question::SaveOver || asked == Question::LoadSlot) {
        return !agreed                      ? Action::None
               : asked == Question::SaveOver ? Action::SaveState
                                             : Action::LoadState;
    }
    if (asked == Question::QuitGame || asked == Question::ExitApplication) {
        return !agreed                      ? Action::None
               : asked == Question::QuitGame ? Action::QuitGame
                                             : Action::ExitApplication;
    }
    if (asked == Question::ClearData) {
        if (!agreed) {
            clear_data = -1;
        }
        return agreed ? Action::ClearData : Action::None;
    }
    if (asked == Question::DiscardControls || asked == Question::DeleteControls) {
        if (agreed && asked == Question::DeleteControls) {
            Config::DeleteControlLayout(*control_editor.index);
        }
        if (agreed) {
            CloseControlEditor();
        }
        return Action::None;
    }
    if (asked == Question::DiscardLayout || asked == Question::DeleteLayout) {
        if (agreed && asked == Question::DeleteLayout) {
            Config::DeleteScreenLayout(*editor.index);
        }
        if (agreed) {
            CloseLayoutEditor();
        }
        return Action::None;
    }
    if (agreed) {
        UvcStream::AllowTakeover();
    }
    if (asked == Question::UsbSetting) {
        if (agreed) {
            Config::SetGlobal(Config::Option::UsbVideo, usb_prompt_value);
        }
        return Action::None;
    }
    // The launch's: Cancel starts nothing.
    if (cancel) {
        return Action::None;
    }
    if (middle) {
        Config::SetGlobal(Config::Option::UsbVideo, 0);
    }
    return Action::LaunchGame;
}

namespace {

/// The editor's limits on a screen: each side of its bounds at least this long, and at most twice
/// the display's width.
constexpr float MinScreenSide = 24.0f;
constexpr float MaxScreenSide = 2.0f * ScreenWidth;

ScreenLayouts::Screen OtherScreen(ScreenLayouts::Screen screen) {
    return screen == ScreenLayouts::Screen::Top ? ScreenLayouts::Screen::Bottom
                                                : ScreenLayouts::Screen::Top;
}

const char* ScreenName(ScreenLayouts::Screen screen) {
    return screen == ScreenLayouts::Screen::Top ? Tr("Top screen") : Tr("Bottom screen");
}

/// A name for a new layout that no other has: Layout 1, Layout 2, ... (ASCII, as the keyboard
/// types).
std::string FreeLayoutName() {
    for (int n = 1;; n++) {
        std::string name = fmt::format("Layout {}", n);
        if (!ScreenLayouts::Find(name)) {
            return name;
        }
    }
}

std::string TrimName(const std::string& text) {
    const std::size_t first = text.find_first_not_of(' ');
    return first == std::string::npos
               ? std::string{}
               : text.substr(first, text.find_last_not_of(' ') - first + 1);
}

/// Whether the display point is on the screen where the transformation places it.
bool OnScreen(const ScreenLayouts::Affine& transform, ScreenLayouts::Point p) {
    const ScreenLayouts::Point q = transform.Inverse().Apply(p);
    return q.x >= 0.0f && q.x < 1.0f && q.y >= 0.0f && q.y < 1.0f;
}

/// A held button's steps: its press, then one every 50 ms once it has been held 0.35 s.
/// `held_for` is its time held, kept by the caller.
bool Repeats(u32 held, u32 button, float& held_for, float dt) {
    if ((held & button) == 0) {
        held_for = 0.0f;
        return false;
    }
    const float before = held_for;
    held_for += dt;
    if (before == 0.0f) {
        return true;
    }
    constexpr float Delay = 0.35f, Every = 0.05f;
    const auto steps = [](float t) {
        return t < Delay ? -1 : static_cast<int>((t - Delay) / Every);
    };
    return steps(held_for) != steps(before);
}

/// A stick's axis byte as -1 to 1, 0 within its dead zone and full past 0.9 of its reach.
float StickAxis(u8 raw) {
    constexpr float DeadZone = 0.2f, Full = 0.9f;
    const float v = (static_cast<float>(raw) - 128.0f) / 128.0f;
    if (std::abs(v) < DeadZone) {
        return 0.0f;
    }
    return std::copysign(std::min((std::abs(v) - DeadZone) / (Full - DeadZone), 1.0f), v);
}

/// The transformation's screen kept within the editor's limits: each side of its bounds between
/// MinScreenSide and MaxScreenSide, its middle on the display.
ScreenLayouts::Affine Limited(ScreenLayouts::Affine transform) {
    using ScreenLayouts::Affine;
    const std::array<float, 4> b = ScreenLayouts::Bounds(ScreenLayouts::Corners(transform));
    const float w = b[2] - b[0], h = b[3] - b[1];
    const float sx = std::clamp(w, MinScreenSide, MaxScreenSide) / w;
    const float sy = std::clamp(h, MinScreenSide, MaxScreenSide) / h;
    if (sx != 1.0f || sy != 1.0f) {
        transform = ScreenLayouts::AboutCentre(transform, Affine::Scale(sx, sy));
    }
    const ScreenLayouts::Point c = transform.Apply({0.5f, 0.5f});
    return ScreenLayouts::CentredAt(
        transform, {std::clamp(c.x, 0.0f, ScreenWidth), std::clamp(c.y, 0.0f, ScreenHeight)});
}

ImVec2 ToImVec(ScreenLayouts::Point p) {
    return ImVec2(p.x, p.y);
}

void DashedQuad(ImDrawList* dl, const ScreenLayouts::Quad& q, ImU32 colour, float thickness) {
    constexpr float Dash = 9.0f, Gap = 7.0f;
    // Round the outline: top-left, top-right, bottom-right, bottom-left.
    const int order[5] = {0, 1, 3, 2, 0};
    for (int i = 0; i < 4; i++) {
        const ImVec2 a(q.x[order[i]], q.y[order[i]]), b(q.x[order[i + 1]], q.y[order[i + 1]]);
        const ImVec2 d = b - a;
        const float length = std::sqrt(d.x * d.x + d.y * d.y);
        for (float at = 0.0f; at < length; at += Dash + Gap) {
            const float end = std::min(at + Dash, length);
            dl->AddLine(a + d * (at / length), a + d * (end / length), colour, thickness);
        }
    }
}

} // namespace

void Ui::DrawLayoutEditorRow() {
    constexpr float Pad = 18.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const Card card = CardItem("##layout_editor", ImVec2(width, RowHeight));
    if (ImGui::GetFocusID() == card.id) {
        focused_row = LayoutEditorRow;
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float y = card.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(card.bb.Min.x + Pad, y), Colour(palette->Text), Tr("Layout editor"));
        // The layout it opens on: a custom one by its name, or a new one.
        const int value = settings_title.empty()
                              ? Config::Global(Config::Option::ScreenLayout)
                              : Config::ForTitle(settings_title, Config::Option::ScreenLayout);
        const auto custom = Config::CustomLayoutIndex(value);
        const auto layout = custom ? ScreenLayouts::Get(*custom) : std::nullopt;
        const std::string shown =
            Fit(layout ? layout->name : std::string{Tr("New layout")}, width * 0.5f);
        const float shown_width = ImGui::CalcTextSize(shown.c_str()).x;
        dl->AddText(ImVec2(card.bb.Max.x - Pad - shown_width, y),
                    Colour(layout ? palette->Text : palette->TextDim), shown.c_str());
    }
    if (card.pressed) {
        OpenLayoutEditor();
    }
}

void Ui::DrawLayoutEditorInfo() {
    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", Tr("Layout editor"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped("%s", Tr("Place the two screens yourself: move, size and turn each one and "
                                "see it on the display as you go, then save it under a name. It "
                                "opens on the Screen layout above: a layout of your own is "
                                "changed, a built-in one is the start of a new one. Your layouts "
                                "follow the built-in ones in Screen layout."));
    ImGui::Spacing();
    if (!game_key.empty()) {
        Note(palette->Accent, Tr("The game's own screens show in the editor."));
    }
}

ScreenLayouts::ScreenLayout Ui::SettingsScreenLayout() const {
    const auto value_of = [this](Config::Option option) {
        return settings_title.empty() ? Config::Global(option)
                                      : Config::ForTitle(settings_title, option);
    };
    const int value = value_of(Config::Option::ScreenLayout);
    if (const auto custom = Config::CustomLayoutIndex(value)) {
        std::optional<ScreenLayouts::ScreenLayout> layout = ScreenLayouts::Get(*custom);
        ASSERT_MSG(layout, "layout editor: the settings choose custom layout {}, of {}", *custom,
                   ScreenLayouts::Count());
        return std::move(*layout);
    }
    // The built-in layout as these settings show it, without the swap: the editor places the top
    // and the bottom screen, and Swap screens trades them as it does any layout's.
    return ScreenLayouts::MakeBuiltin(
        static_cast<Config::Layout>(value), false, value_of(Config::Option::Widescreen) != 0,
        static_cast<float>(value_of(Config::Option::OverlayOpacity)) / 100.0f);
}

void Ui::OpenLayoutEditor() {
    const auto value_of = [this](Config::Option option) {
        return settings_title.empty() ? Config::Global(option)
                                      : Config::ForTitle(settings_title, option);
    };
    const int value = value_of(Config::Option::ScreenLayout);
    editor = {};
    editor.layout = SettingsScreenLayout();
    if (const auto custom = Config::CustomLayoutIndex(value)) {
        editor.index = custom;
        editor.name = editor.layout.name;
    } else {
        editor.name = FreeLayoutName();
    }
    // What is held now, the cross that opened it, is not the editor's.
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    editor.pad = pad.buttons;
    editor.touches = 2; // a finger already down is not the preview's
    state = State::LayoutEditor;
    overlay_focused = false;
    window.PreviewLayout(editor.layout);
    LOG_INFO(Frontend, "layout editor: open on {}", editor.index ? editor.name : "a new layout");
}

void Ui::CloseLayoutEditor() {
    window.PreviewLayout(std::nullopt);
    editor.panel = EditorPanel::None;
    state = State::Settings;
    overlay_focused = false;
    focus_first_row = true;
    LOG_INFO(Frontend, "layout editor: closed");
}

void Ui::EditLayout() {
    using ScreenLayouts::Affine;
    using ScreenLayouts::Point;
    using ScreenLayouts::Screen;
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const u32 held = pad.buttons;
    const u32 pressed = held & ~editor.pad;
    editor.pad = held;
    bool edited = false;

    if (pressed & ImGui_ImplVita_BackButton()) {
        // Back to the settings, asked first where there is something to lose.
        if (editor.changed) {
            question = Question::DiscardLayout;
        } else {
            CloseLayoutEditor();
        }
        return;
    }
    if (pressed & (SCE_CTRL_START | ImGui_ImplVita_ConfirmButton())) {
        editor.panel = pressed & SCE_CTRL_START ? EditorPanel::Save : EditorPanel::Tools;
        focus_first_row = true;
        return;
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
        editor.selected = OtherScreen(editor.selected);
    }
    ScreenLayouts::Placement& selected = editor.layout[editor.selected];
    Affine& m = selected.transform;
    const auto change = [&](const Affine& next) {
        m = next;
        edited = true;
    };
    if (pressed & SCE_CTRL_SQUARE) {
        change(ScreenLayouts::AboutCentre(m, Affine::Rotate(1)));
    }
    if (pressed & SCE_CTRL_SELECT) {
        selected.shown = !selected.shown;
        edited = true;
    }
    // The d-pad: a pixel a press, and on while held.
    static constexpr struct {
        u32 button;
        float dx, dy;
    } Moves[] = {{SCE_CTRL_UP, 0.0f, -1.0f},
                 {SCE_CTRL_DOWN, 0.0f, 1.0f},
                 {SCE_CTRL_LEFT, -1.0f, 0.0f},
                 {SCE_CTRL_RIGHT, 1.0f, 0.0f}};
    for (std::size_t i = 0; i < 4; i++) {
        if (Repeats(held, Moves[i].button, editor.held_for[i], dt)) {
            change(Affine::Translate(Moves[i].dx, Moves[i].dy) * m);
        }
    }
    // L and R: smaller or larger in proportion, two pixels along its longer side a step.
    for (std::size_t i = 0; i < 2; i++) {
        if (Repeats(held, i == 0 ? SCE_CTRL_LTRIGGER : SCE_CTRL_RTRIGGER, editor.held_for[4 + i],
                    dt)) {
            const std::array<float, 4> b = ScreenLayouts::Bounds(ScreenLayouts::Corners(m));
            const float longer = std::max(b[2] - b[0], b[3] - b[1]);
            change(ScreenLayouts::AboutCentre(
                m, Affine::Scale((longer + (i == 0 ? -2.0f : 2.0f)) / longer)));
        }
    }
    // The left stick moves it, up to 480 pixels a second; the right one stretches it, right
    // wider and up taller, up to its size again a second. Squared, for slow fine moves.
    constexpr float Speed = 480.0f;
    const float lx = StickAxis(pad.lx), ly = StickAxis(pad.ly);
    if (lx != 0.0f || ly != 0.0f) {
        change(Affine::Translate(lx * std::abs(lx) * Speed * dt, ly * std::abs(ly) * Speed * dt) *
               m);
    }
    const float rx = StickAxis(pad.rx), ry = -StickAxis(pad.ry);
    if (rx != 0.0f || ry != 0.0f) {
        change(ScreenLayouts::AboutCentre(
            m, Affine::Scale(1.0f + rx * std::abs(rx) * dt, 1.0f + ry * std::abs(ry) * dt)));
    }

    // Touch: a finger on a screen selects it and drags it, two pinch the selected one.
    SceTouchData touch{};
    const int fingers = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0
                            ? std::min(static_cast<int>(touch.reportNum), 2)
                            : 0;
    std::array<Point, 2> now{};
    for (int i = 0; i < fingers; i++) {
        // The panel's coordinates are twice the display's.
        now[i] = {static_cast<float>(touch.report[i].x) * 0.5f,
                  static_cast<float>(touch.report[i].y) * 0.5f};
    }
    if (fingers > 0 && editor.touches == 0) {
        // A touch's start: on a bar, the bar's (its buttons); else the preview's, and the screen
        // under it, the one in front first, is selected.
        const bool on_bars = !editor.bars_hidden && (now[0].y < HeaderHeight ||
                                                     now[0].y >= ScreenHeight - FooterHeight);
        editor.preview_touch = !on_bars;
        editor.dragging = false;
        if (editor.preview_touch) {
            for (const Screen s : {editor.layout.front, OtherScreen(editor.layout.front)}) {
                if (OnScreen(editor.layout[s].transform, now[0])) {
                    editor.selected = s;
                    editor.dragging = true;
                    break;
                }
            }
        }
    } else if (fingers > 0 && editor.preview_touch) {
        Affine& t = editor.layout[editor.selected].transform;
        const std::array<Point, 2>& was = editor.touch;
        if (fingers == 2 && editor.touches == 2) {
            // Scaled about the fingers' middle by how far apart they moved, and moved with it.
            const auto middle = [](const std::array<Point, 2>& p) {
                return Point{(p[0].x + p[1].x) * 0.5f, (p[0].y + p[1].y) * 0.5f};
            };
            const auto apart = [](const std::array<Point, 2>& p) {
                return std::hypot(p[1].x - p[0].x, p[1].y - p[0].y);
            };
            const Point from = middle(was), to = middle(now);
            if (const float before = apart(was); before > 8.0f) {
                t = Affine::Translate(to.x, to.y) * Affine::Scale(apart(now) / before) *
                    Affine::Translate(-from.x, -from.y) * t;
                edited = true;
            }
        } else if (fingers == 1 && editor.touches == 1 && editor.dragging) {
            t = Affine::Translate(now[0].x - was[0].x, now[0].y - was[0].y) * t;
            edited = true;
        }
    }
    editor.touches = fingers;
    editor.touch = now;

    if (edited) {
        for (ScreenLayouts::Placement& p : editor.layout.screens) {
            p.transform = Limited(p.transform);
        }
        editor.changed = editor.dirty = true;
        editor.busy += dt;
        editor.idle = 0.0f;
        editor.bars_hidden |= editor.busy > 0.2f;
    } else {
        editor.busy = 0.0f;
        editor.idle += dt;
        editor.bars_hidden &= editor.idle < 0.5f;
    }
}

void Ui::DrawLayoutPreview() {
    using ScreenLayouts::Screen;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth, ScreenHeight));
    ImGui::Begin("##layout_preview", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    // Under the bars and the panels, whichever was made first.
    ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, ScreenHeight), false);
    // A running game's screens are drawn under the interface where the layout places them; in the
    // game list each is a card of its own colour.
    const bool game = !game_key.empty();
    for (const Screen s : {OtherScreen(editor.layout.front), editor.layout.front}) {
        const ScreenLayouts::Placement& p = editor.layout[s];
        const ScreenLayouts::Quad q = ScreenLayouts::Corners(p.transform);
        const ImVec2 tl(q.x[0], q.y[0]), tr(q.x[1], q.y[1]), bl(q.x[2], q.y[2]),
            br(q.x[3], q.y[3]);
        const bool selected = s == editor.selected;
        if (!game && p.shown) {
            const ImVec4& tint = s == Screen::Top ? palette->Icon : palette->Own;
            dl->AddQuadFilled(tl, tr, br, bl, Colour(palette->Panel));
            dl->AddQuadFilled(tl, tr, br, bl, Colour(tint, 0.45f * p.opacity));
        }
        const ImU32 line = Colour(selected ? palette->Accent : palette->Text, selected ? 1.0f : 0.7f);
        const float thickness = selected ? 3.0f : 1.5f;
        if (p.shown) {
            dl->AddQuad(tl, tr, br, bl, line, thickness);
        } else {
            DashedQuad(dl, q, line, thickness);
        }
        // Its top: an arrow inside the middle of the edge the picture's top is on.
        const ImVec2 across = tr - tl, down = bl - tl;
        const float across_length = std::sqrt(across.x * across.x + across.y * across.y);
        const float down_length = std::sqrt(down.x * down.x + down.y * down.y);
        if (across_length > 1.0f && down_length > 1.0f) {
            const ImVec2 ex = across * (1.0f / across_length), ey = down * (1.0f / down_length);
            const ImVec2 top = tl + across * 0.5f;
            dl->AddTriangleFilled(top + ey * 6.0f, top + ey * 20.0f - ex * 9.0f,
                                  top + ey * 20.0f + ex * 9.0f, line);
        }
        const std::string label = p.shown ? std::string{ScreenName(s)}
                                          : TrFormat("{} (hidden)", ScreenName(s));
        const ImVec2 size = ImGui::CalcTextSize(label.c_str());
        const ImVec2 centre = ToImVec(p.transform.Apply({0.5f, 0.5f}));
        const ImVec2 at = centre - size * 0.5f;
        dl->AddRectFilled(at - ImVec2(10.0f, 4.0f), at + size + ImVec2(10.0f, 4.0f),
                          Colour(palette->Panel, 0.85f), size.y * 0.5f + 4.0f);
        dl->AddText(at, Colour(selected ? palette->Accent : palette->Text), label.c_str());
    }
    dl->PopClipRect();
    ImGui::End();
}

void Ui::DrawLayoutBars() {
    using ScreenLayouts::Affine;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth, ScreenHeight));
    ImGui::Begin("##layout_bars", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, ScreenHeight), false);
    const ImU32 panel = Colour(palette->Panel, 0.9f / palette->Panel.w);
    const float footer = ScreenHeight - FooterHeight;
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, HeaderHeight), panel);
    dl->AddRectFilled(ImVec2(0.0f, footer), ImVec2(ScreenWidth, ScreenHeight), panel);
    DrawIcon(dl, Icon::Screens, ImVec2(Margin + 16.0f, HeaderHeight * 0.5f), 30.0f,
             Colour(palette->Accent), panel);
    const float title_x = Margin + 44.0f;
    const std::string title = Fit(editor.name, 260.0f);
    dl->AddText(ImGui::GetFont(), TitleSize, ImVec2(title_x, (HeaderHeight - TitleSize) * 0.5f),
                Colour(palette->Text), title.c_str());
    const ScreenLayouts::Placement& p = editor.layout[editor.selected];
    const std::array<float, 4> b = ScreenLayouts::Bounds(ScreenLayouts::Corners(p.transform));
    const std::string place =
        TrFormat("{}: {}x{} at {}, {}, turned {} degrees, {}% opaque{}", ScreenName(editor.selected),
                 std::lround(b[2] - b[0]), std::lround(b[3] - b[1]), std::lround(b[0]),
                 std::lround(b[1]), ScreenLayouts::TurnDegrees(p.transform),
                 std::lround(p.opacity * 100.0f), p.shown ? "" : Tr(", hidden"));
    const char* const hint =
        Tr("Move: d-pad, left stick or drag. Size: L, R or pinch. Stretch: right stick.");
    const float text_x = title_x + 280.0f;
    const float line = ImGui::GetFontSize();
    dl->AddText(ImVec2(text_x, HeaderHeight * 0.5f - line - 2.0f), Colour(palette->Text),
                Fit(place, ScreenWidth - Margin - text_x).c_str());
    dl->AddText(ImVec2(text_x, HeaderHeight * 0.5f + 2.0f), Colour(palette->TextDim),
                Fit(hint, ScreenWidth - Margin - text_x).c_str());
    dl->PopClipRect();

    // The pad's buttons, each a touch target for the same.
    ImGui::SetCursorScreenPos(ImVec2(Margin, footer + (FooterHeight - FooterButtonHeight) * 0.5f));
    if (FooterButton(BackGlyph(), Tr("Back"))) {
        if (editor.changed) {
            question = Question::DiscardLayout;
        } else {
            CloseLayoutEditor();
        }
    }
    if (FooterButton(ConfirmGlyph(), Tr("Tools"))) {
        editor.panel = EditorPanel::Tools;
        focus_first_row = true;
    }
    if (FooterButton(Glyph::Triangle, Tr("Other screen"))) {
        editor.selected = OtherScreen(editor.selected);
    }
    if (FooterButton(Glyph::Square, Tr("Turn"))) {
        Affine& t = editor.layout[editor.selected].transform;
        t = ScreenLayouts::AboutCentre(t, Affine::Rotate(1));
        editor.changed = editor.dirty = true;
    }
    FooterButton(Glyph::Shoulders, Tr("Size"));
    if (FooterButton(Glyph::Start, Tr("Save"))) {
        editor.panel = EditorPanel::Save;
        focus_first_row = true;
    }
    ImGui::End();
}

void Ui::DrawLayoutTools() {
    using ScreenLayouts::Affine;
    constexpr float Pad = 18.0f;
    constexpr float Width = 560.0f;
    const bool deletable = editor.index.has_value();
    const int rows = deletable ? 8 : 7;
    // Kept focused, as the lists are, unless a touch holds an item.
    if (!ImGui::GetIO().MouseDown[0] && ImGui::GetActiveID() == 0) {
        ImGui::SetNextWindowFocus();
    }
    BeginCentered("##layout_tools", Width,
                  32.0f + TitleSize + 14.0f + static_cast<float>(rows) * (RowHeight + 6.0f));
    ScreenLayouts::Placement& p = editor.layout[editor.selected];
    Affine& t = p.transform;
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), TitleSize, ImGui::GetCursorScreenPos(),
                                        Colour(palette->Text), ScreenName(editor.selected));
    ImGui::Dummy(ImVec2(0.0f, TitleSize + 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 6.0f));
    const float width = ImGui::GetContentRegionAvail().x;
    bool edited = false;
    const auto row = [&](const char* id, const char* label, const std::string& value) {
        const Card card = CardItem(id, ImVec2(width, RowHeight));
        if (focus_first_row) {
            focus_first_row = false;
            FocusLastItem();
        }
        if (card.visible) {
            ImDrawList* const dl = ImGui::GetWindowDrawList();
            const float y = card.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
            dl->AddText(ImVec2(card.bb.Min.x + Pad, y), Colour(palette->Text), label);
            const float value_width = ImGui::CalcTextSize(value.c_str()).x;
            dl->AddText(ImVec2(card.bb.Max.x - Pad - value_width, y), Colour(palette->TextDim),
                        value.c_str());
        }
        return card;
    };
    const Card shown = row("##shown", Tr("Show this screen"), p.shown ? Tr("On") : Tr("Off"));
    if (shown.pressed) {
        p.shown = !p.shown;
        edited = true;
    }
    // The opacity's steps: the pad's left and right, or a tap on either half.
    const Card opacity = row("##opacity", Tr("Opacity"),
                             fmt::format("< {}% >", std::lround(p.opacity * 100.0f)));
    int step = 0;
    if (opacity.focused && ImGui::IsNavInputTest(ImGuiNavInput_DpadLeft, ImGuiInputReadMode_Repeat)) {
        step = -1;
    } else if (opacity.focused &&
               ImGui::IsNavInputTest(ImGuiNavInput_DpadRight, ImGuiInputReadMode_Repeat)) {
        step = 1;
    } else if (opacity.pressed && !opacity.by_pad) {
        step = ImGui::GetIO().MousePos.x < opacity.bb.GetCenter().x ? -1 : 1;
    }
    if (step != 0) {
        p.opacity = std::clamp(std::round(p.opacity * 20.0f + static_cast<float>(step)) / 20.0f,
                               0.1f, 1.0f);
        edited = true;
    }
    const bool in_front = editor.layout.front == editor.selected;
    if (row("##front", Tr("Bring to front"), in_front ? Tr("In front") : "").pressed) {
        editor.layout.front = editor.selected;
        edited = true;
    }
    const ScreenLayouts::Size display{ScreenWidth, ScreenHeight};
    const ScreenLayouts::Point middle{ScreenWidth * 0.5f, ScreenHeight * 0.5f};
    if (row("##fill", Tr("Fill the display"), "").pressed) {
        t = ScreenLayouts::CentredAt(ScreenLayouts::Fitted(t, display, false), middle);
        edited = true;
    }
    if (row("##stretch", Tr("Stretch over the display"), "").pressed) {
        t = ScreenLayouts::CentredAt(ScreenLayouts::Fitted(t, display, true), middle);
        edited = true;
    }
    if (row("##native", Tr("3DS size"), "").pressed) {
        // The screen's own pixels, one a display pixel, turned as it is.
        ScreenLayouts::Size native = editor.selected == ScreenLayouts::Screen::Top
                                         ? ScreenLayouts::TopScreenSize
                                         : ScreenLayouts::BottomScreenSize;
        if (ScreenLayouts::TurnDegrees(t) % 180 != 0) {
            std::swap(native.w, native.h);
        }
        t = ScreenLayouts::Fitted(t, native, true);
        edited = true;
    }
    if (row("##centre", Tr("Centre on the display"), "").pressed) {
        t = ScreenLayouts::CentredAt(t, middle);
        edited = true;
    }
    if (deletable && row("##delete", Tr("Delete this layout"), "").pressed) {
        question = Question::DeleteLayout;
    }
    ImGui::PopStyleVar();
    const bool back = BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    if (edited) {
        t = Limited(t);
        editor.changed = editor.dirty = true;
    }
    if (back) {
        editor.panel = EditorPanel::None;
        overlay_focused = false;
    }
}

void Ui::DrawLayoutSave() {
    constexpr float Padding = 28.0f;
    constexpr float Width = 640.0f;
    const std::string name = TrimName(editor.name);
    const std::optional<std::size_t> named = ScreenLayouts::Find(name);
    // Save keeps the layout opened under the name, Save as new adds one: neither takes another
    // layout's name.
    const bool save_ok = !name.empty() && (!named || named == editor.index);
    const bool new_ok = !name.empty() && !named;
    const std::string uses = settings_title.empty()
                                 ? std::string{Tr("All games then use it.")}
                                 : TrFormat("{} then uses it.", settings_name);
    const char* const problem = name.empty() ? Tr("Give the layout a name.")
                                : !save_ok   ? Tr("Another layout has this name.")
                                             : nullptr;
    if (!ImGui::GetIO().MouseDown[0] && ImGui::GetActiveID() == 0) {
        ImGui::SetNextWindowFocus();
    }
    BeginCentered("##layout_save", Width, 2.0f * Padding + TitleSize + 3.0f * RowHeight + 40.0f);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), TitleSize, ImGui::GetCursorScreenPos(),
                                        Colour(palette->Text), Tr("Save layout"));
    ImGui::Dummy(ImVec2(0.0f, TitleSize + 6.0f));
    // The name's field: cross or a tap types it on the keyboard.
    const float width = ImGui::GetContentRegionAvail().x;
    const Card field = CardItem("##layout_name", ImVec2(width, RowHeight));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    if (field.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float y = field.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(field.bb.Min.x + 18.0f, y), Colour(palette->Text), Tr("Name"));
        const std::string value = Fit(editor.name, width - 160.0f);
        const float value_width = ImGui::CalcTextSize(value.c_str()).x;
        dl->AddText(ImVec2(field.bb.Max.x - 18.0f - value_width, y), Colour(palette->Text),
                    value.c_str());
    }
    if (field.pressed) {
        OpenKeyboard(KeyboardLayoutName, editor.name);
    }
    Note(problem ? palette->Own : palette->TextDim, problem ? std::string{problem} : uses);
    ImGui::Spacing();
    const ImVec2 button(editor.index ? 180.0f : 200.0f, 48.0f);
    const auto action = [&](const char* label, bool enabled) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, enabled ? 1.0f : 0.4f);
        const bool pressed = ImGui::Button(label, button);
        ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, 16.0f);
        return pressed && enabled;
    };
    const bool save = action(Tr("Save"), save_ok);
    const bool save_new = editor.index && action(Tr("Save as new"), new_ok);
    const bool cancel = action(Tr("Cancel"), true) || BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    if (save || save_new) {
        editor.name = name;
        SaveEditedLayout(save_new);
    } else if (cancel) {
        editor.panel = EditorPanel::None;
        overlay_focused = false;
    }
}

void Ui::SaveEditedLayout(bool as_new) {
    editor.layout.name = editor.name;
    const std::size_t index =
        Config::SaveScreenLayout(as_new ? std::nullopt : editor.index, editor.layout);
    const int value = Config::CustomLayoutValue(index);
    if (settings_title.empty()) {
        Config::SetGlobal(Config::Option::ScreenLayout, value);
    } else {
        Config::SetTitleValue(settings_title, Config::Option::ScreenLayout, value);
    }
    Config::Save();
    LOG_INFO(Frontend, "layout editor: {} saved as custom layout {}", editor.name, index);
    CloseLayoutEditor();
}

Ui::Action Ui::DrawLayoutEditor() {
    const bool covered =
        editor.panel != EditorPanel::None || editing_field >= 0 || question != Question::None;
    if (!covered) {
        EditLayout();
        if (state != State::LayoutEditor) {
            return Action::None;
        }
    } else {
        // Under a panel the pad and the touch are its: a press or a finger still down as it
        // closes is not the editor's.
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        editor.pad = pad.buttons;
        editor.touches = 2;
        editor.preview_touch = false;
        editor.bars_hidden = false;
    }
    DrawLayoutPreview();
    if (editor.panel == EditorPanel::None && editing_field < 0) {
        if (!editor.bars_hidden) {
            DrawLayoutBars();
        }
    } else if (editing_field >= 0) {
        DrawKeyboard();
    } else if (editor.panel == EditorPanel::Tools) {
        DrawLayoutTools();
    } else {
        DrawLayoutSave();
    }
    // Applied once a change, not each frame: the window places the screens and inverts the
    // bottom one's for the touches when it is given the layout.
    if (state == State::LayoutEditor && std::exchange(editor.dirty, false)) {
        window.PreviewLayout(editor.layout);
    }
    return Action::None;
}

namespace {

/// The Vita inputs' names, Config::ControlInput's order (the Controls tab's choices).
constexpr const char* InputNames[] = {Translatable("Circle"),
                                      Translatable("Cross"),
                                      Translatable("Triangle"),
                                      Translatable("Square"),
                                      Translatable("Up"),
                                      Translatable("Down"),
                                      Translatable("Left"),
                                      Translatable("Right"),
                                      "L",
                                      "R",
                                      "Start",
                                      "Select",
                                      Translatable("Rear touch, left half"),
                                      Translatable("Rear touch, right half"),
                                      Translatable("None")};
static_assert(std::size(InputNames) == static_cast<std::size_t>(Config::ControlInput::Count));

std::string FreeControlName() {
    for (int n = 1;; n++) {
        std::string name = fmt::format("Controls {}", n);
        if (!ControlLayouts::Find(name)) {
            return name;
        }
    }
}

/// The on-screen button after `from` (none: the first), in Button order, round to the first.
std::optional<ControlLayouts::Button> NextOnScreen(const ControlLayouts::ControlLayout& layout,
                                                   std::optional<ControlLayouts::Button> from) {
    const std::size_t start = from ? static_cast<std::size_t>(*from) + 1 : 0;
    for (std::size_t k = 0; k < ControlLayouts::SpotCount; k++) {
        const std::size_t i = (start + k) % ControlLayouts::SpotCount;
        if (layout.on_screen[i]) {
            return static_cast<ControlLayouts::Button>(i);
        }
    }
    return std::nullopt;
}

/// An on-screen button as the editor shows it: a disc, its ring, its label (the d-pad's an
/// arrow).
void DrawOnScreenButton(ImDrawList* dl, ControlLayouts::Button button,
                        const ControlLayouts::OnScreen& p, float opacity, bool selected) {
    const ImVec2 c(p.x, p.y);
    const float r = p.size * 0.5f;
    dl->AddCircleFilled(c, r, IM_COL32(20, 20, 20, static_cast<int>(opacity * 0.55f * 255.0f)),
                        32);
    dl->AddCircle(c, r - 1.5f,
                  selected ? Colour(palette->Accent)
                           : IM_COL32(255, 255, 255, static_cast<int>(opacity * 0.8f * 255.0f)),
                  32, selected ? 4.0f : 3.0f);
    const ImU32 ink = IM_COL32(255, 255, 255, static_cast<int>(std::max(opacity, 0.6f) * 255.0f));
    const auto arrow = [&](float dx, float dy) {
        const float a = r * 0.42f;
        dl->AddTriangleFilled(ImVec2(c.x + dx * a, c.y + dy * a),
                              ImVec2(c.x - dx * a * 0.6f - dy * a, c.y - dy * a * 0.6f + dx * a),
                              ImVec2(c.x - dx * a * 0.6f + dy * a, c.y - dy * a * 0.6f - dx * a),
                              ink);
    };
    switch (button) {
    case ControlLayouts::Button::Up:
        arrow(0.0f, -1.0f);
        break;
    case ControlLayouts::Button::Down:
        arrow(0.0f, 1.0f);
        break;
    case ControlLayouts::Button::Left:
        arrow(-1.0f, 0.0f);
        break;
    case ControlLayouts::Button::Right:
        arrow(1.0f, 0.0f);
        break;
    default: {
        const char* const label = ControlLayouts::DiscLabel(button);
        const ImVec2 size = ImGui::CalcTextSize(label);
        const float scale =
            std::min({2.0f, p.size * 0.7f / size.x, p.size * 0.45f / ImGui::GetFontSize()});
        const float font = ImGui::GetFontSize() * scale;
        dl->AddText(ImGui::GetFont(), font, c - size * (scale * 0.5f), ink, label);
        break;
    }
    }
}

} // namespace

void Ui::DrawControlEditorRow() {
    constexpr float Pad = 18.0f;
    const float width = ImGui::GetContentRegionAvail().x;
    const Card card = CardItem("##control_editor", ImVec2(width, RowHeight));
    if (ImGui::GetFocusID() == card.id) {
        focused_row = ControlEditorRow;
    }
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float y = card.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(card.bb.Min.x + Pad, y), Colour(palette->Text),
                    Tr("Control layout editor"));
        // The layout it opens on: one of the player's by its name, or a new one.
        const int value = settings_title.empty()
                              ? Config::Global(Config::Option::ControlLayout)
                              : Config::ForTitle(settings_title, Config::Option::ControlLayout);
        const auto custom = Config::CustomControlIndex(value);
        const auto layout = custom ? ControlLayouts::Get(*custom) : std::nullopt;
        const std::string shown =
            Fit(layout ? layout->name : std::string{Tr("New layout")}, width * 0.5f);
        const float shown_width = ImGui::CalcTextSize(shown.c_str()).x;
        dl->AddText(ImVec2(card.bb.Max.x - Pad - shown_width, y),
                    Colour(layout ? palette->Text : palette->TextDim), shown.c_str());
    }
    if (card.pressed) {
        OpenControlEditor();
    }
}

void Ui::DrawControlEditorInfo() {
    ImGui::SetWindowFontScale(TitleSize / ImGui::GetFontSize());
    ImGui::TextWrapped("%s", Tr("Control layout editor"));
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Spacing();
    ImGui::TextWrapped(
        "%s", Tr("Make a control layout of your own: the Vita input each 3DS button answers to, "
                 "and buttons on the screen that a finger on the display presses, such as ZL and "
                 "ZR without the rear touch panel. It opens on the Control layout above: a layout "
                 "of your own is changed, the buttons below are the start of a new one."));
    ImGui::Spacing();
    if (!game_key.empty()) {
        Note(palette->Accent, Tr("The game's own screens show in the editor."));
    }
}

void Ui::OpenControlEditor() {
    const auto value_of = [this](Config::Option option) {
        return settings_title.empty() ? Config::Global(option)
                                      : Config::ForTitle(settings_title, option);
    };
    ControlEditor& ed = control_editor;
    ed = {};
    if (const auto custom = Config::CustomControlIndex(value_of(Config::Option::ControlLayout))) {
        std::optional<ControlLayouts::ControlLayout> layout = ControlLayouts::Get(*custom);
        ASSERT_MSG(layout, "control editor: the settings choose control layout {}, of {}",
                   *custom, ControlLayouts::Count());
        ed.layout = std::move(*layout);
        ed.index = custom;
        ed.name = ed.layout.name;
    } else {
        // The Controls tab's mapping as these settings have it, with no button on screen yet.
        for (std::size_t b = 0; b < ControlLayouts::ButtonCount; b++) {
            ed.layout.inputs[b] = value_of(
                static_cast<Config::Option>(static_cast<int>(Config::Option::MapA) + b));
        }
        ed.name = FreeControlName();
    }
    ed.selected = NextOnScreen(ed.layout, std::nullopt);
    ed.screens = SettingsScreenLayout();
    // What is held now, the cross that opened it, is not the editor's.
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    ed.pad = pad.buttons;
    ed.touches = 2;
    state = State::ControlEditor;
    overlay_focused = false;
    LOG_INFO(Frontend, "control editor: open on {}", ed.index ? ed.name : "a new layout");
}

void Ui::CloseControlEditor() {
    control_editor.panel = EditorPanel::None;
    state = State::Settings;
    overlay_focused = false;
    focus_first_row = true;
    LOG_INFO(Frontend, "control editor: closed");
}

void Ui::EditControls() {
    using ControlLayouts::OnScreen;
    using ScreenLayouts::Point;
    ControlEditor& ed = control_editor;
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const u32 held = pad.buttons;
    const u32 pressed = held & ~ed.pad;
    ed.pad = held;

    if (pressed & ImGui_ImplVita_BackButton()) {
        if (ed.changed) {
            question = Question::DiscardControls;
        } else {
            CloseControlEditor();
        }
        return;
    }
    if (pressed & (SCE_CTRL_START | ImGui_ImplVita_ConfirmButton())) {
        ed.panel = pressed & SCE_CTRL_START ? EditorPanel::Save : EditorPanel::Tools;
        focus_first_row = true;
        return;
    }
    if (pressed & SCE_CTRL_TRIANGLE) {
        ed.selected = NextOnScreen(ed.layout, ed.selected);
    }
    bool edited = false;
    const auto selected = [&]() -> OnScreen* {
        return ed.selected ? &*ed.layout.on_screen[static_cast<std::size_t>(*ed.selected)]
                           : nullptr;
    };
    if (OnScreen* p = selected()) {
        // The d-pad: a pixel a press, and on while held; L and R: two pixels smaller or larger.
        static constexpr struct {
            u32 button;
            float dx, dy;
        } Moves[] = {{SCE_CTRL_UP, 0.0f, -1.0f},
                     {SCE_CTRL_DOWN, 0.0f, 1.0f},
                     {SCE_CTRL_LEFT, -1.0f, 0.0f},
                     {SCE_CTRL_RIGHT, 1.0f, 0.0f}};
        for (std::size_t i = 0; i < 4; i++) {
            if (Repeats(held, Moves[i].button, ed.held_for[i], dt)) {
                p->x += Moves[i].dx;
                p->y += Moves[i].dy;
                edited = true;
            }
        }
        for (std::size_t i = 0; i < 2; i++) {
            if (Repeats(held, i == 0 ? SCE_CTRL_LTRIGGER : SCE_CTRL_RTRIGGER, ed.held_for[4 + i],
                        dt)) {
                p->size += i == 0 ? -2.0f : 2.0f;
                edited = true;
            }
        }
        // The left stick moves it, up to 480 pixels a second, squared for slow fine moves.
        constexpr float Speed = 480.0f;
        const float lx = StickAxis(pad.lx), ly = StickAxis(pad.ly);
        if (lx != 0.0f || ly != 0.0f) {
            p->x += lx * std::abs(lx) * Speed * dt;
            p->y += ly * std::abs(ly) * Speed * dt;
            edited = true;
        }
    }

    // Touch: a finger on an on-screen button selects and drags it (over the bars too: they hide
    // while it is down), two pinch the selected one.
    SceTouchData touch{};
    const int fingers = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) > 0
                            ? std::min(static_cast<int>(touch.reportNum), 2)
                            : 0;
    std::array<Point, 2> now{};
    for (int i = 0; i < fingers; i++) {
        // The panel's coordinates are twice the display's.
        now[i] = {static_cast<float>(touch.report[i].x) * 0.5f,
                  static_cast<float>(touch.report[i].y) * 0.5f};
    }
    if (fingers > 0 && ed.touches == 0) {
        const auto hit = ControlLayouts::Hit(ed.layout, now[0].x, now[0].y);
        const bool on_bars =
            now[0].y < HeaderHeight || now[0].y >= ScreenHeight - FooterHeight;
        ed.preview_touch = hit || !on_bars;
        ed.dragging = hit.has_value();
        if (hit) {
            ed.selected = hit;
        }
    } else if (fingers > 0 && ed.preview_touch) {
        if (OnScreen* p = selected()) {
            const std::array<Point, 2>& was = ed.touch;
            if (fingers == 2 && ed.touches == 2) {
                const float before = std::hypot(was[1].x - was[0].x, was[1].y - was[0].y);
                if (before > 8.0f) {
                    p->size *= std::hypot(now[1].x - now[0].x, now[1].y - now[0].y) / before;
                    edited = true;
                }
            } else if (fingers == 1 && ed.touches == 1 && ed.dragging) {
                p->x += now[0].x - was[0].x;
                p->y += now[0].y - was[0].y;
                edited = true;
            }
        }
    }
    if (fingers == 0) {
        ed.preview_touch = ed.dragging = false;
    }
    ed.touches = fingers;
    ed.touch = now;
    if (edited) {
        *selected() = ControlLayouts::Limited(*selected());
        ed.changed = true;
    }
}

void Ui::DrawControlPreview() {
    using ScreenLayouts::Screen;
    const ControlEditor& ed = control_editor;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth, ScreenHeight));
    ImGui::Begin("##control_preview", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::BringWindowToDisplayBack(ImGui::GetCurrentWindow());
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, ScreenHeight), false);
    // A running game's screens are under the interface; in the game list each is a card where the
    // screen layout puts it.
    if (game_key.empty()) {
        dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, ScreenHeight),
                          Colour(palette->Backdrop));
        for (const Screen s : {OtherScreen(ed.screens.front), ed.screens.front}) {
            const ScreenLayouts::Placement& p = ed.screens[s];
            if (!p.shown) {
                continue;
            }
            const ScreenLayouts::Quad q = ScreenLayouts::Corners(p.transform);
            const ImVec2 tl(q.x[0], q.y[0]), tr(q.x[1], q.y[1]), bl(q.x[2], q.y[2]),
                br(q.x[3], q.y[3]);
            const ImVec4& tint = s == Screen::Top ? palette->Icon : palette->Own;
            dl->AddQuadFilled(tl, tr, br, bl, Colour(palette->Panel));
            dl->AddQuadFilled(tl, tr, br, bl, Colour(tint, 0.3f * p.opacity));
            dl->AddQuad(tl, tr, br, bl, Colour(palette->Text, 0.5f), 1.5f);
            const char* const name = ScreenName(s);
            const ImVec2 size = ImGui::CalcTextSize(name);
            dl->AddText(ToImVec(p.transform.Apply({0.5f, 0.5f})) - size * 0.5f,
                        Colour(palette->TextDim), name);
        }
    }
    for (std::size_t i = 0; i < ControlLayouts::SpotCount; i++) {
        if (const auto& p = ed.layout.on_screen[i]) {
            const auto button = static_cast<ControlLayouts::Button>(i);
            DrawOnScreenButton(dl, button, *p, ed.layout.opacity, ed.selected == button);
        }
    }
    dl->PopClipRect();
    ImGui::End();
}

void Ui::DrawControlBars() {
    ControlEditor& ed = control_editor;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(ScreenWidth, ScreenHeight));
    ImGui::Begin("##control_bars", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, ScreenHeight), false);
    const ImU32 panel = Colour(palette->Panel, 0.9f / palette->Panel.w);
    const float footer = ScreenHeight - FooterHeight;
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, HeaderHeight), panel);
    dl->AddRectFilled(ImVec2(0.0f, footer), ImVec2(ScreenWidth, ScreenHeight), panel);
    DrawIcon(dl, Icon::Pad, ImVec2(Margin + 16.0f, HeaderHeight * 0.5f), 30.0f,
             Colour(palette->Accent), panel);
    const float title_x = Margin + 44.0f;
    dl->AddText(ImGui::GetFont(), TitleSize, ImVec2(title_x, (HeaderHeight - TitleSize) * 0.5f),
                Colour(palette->Text), Fit(ed.name, 260.0f).c_str());
    std::string place;
    if (ed.selected) {
        const ControlLayouts::OnScreen& p =
            *ed.layout.on_screen[static_cast<std::size_t>(*ed.selected)];
        place = TrFormat("{}: {} pixels across at {}, {}", ControlLayouts::Label(*ed.selected),
                         std::lround(p.size), std::lround(p.x), std::lround(p.y));
    } else {
        place = Tr("No button on the screen yet: put some there with Buttons.");
    }
    const char* const hint = Tr("Move: d-pad, left stick or drag. Size: L, R or pinch.");
    const float text_x = title_x + 280.0f;
    const float line = ImGui::GetFontSize();
    dl->AddText(ImVec2(text_x, HeaderHeight * 0.5f - line - 2.0f), Colour(palette->Text),
                Fit(place, ScreenWidth - Margin - text_x).c_str());
    dl->AddText(ImVec2(text_x, HeaderHeight * 0.5f + 2.0f), Colour(palette->TextDim),
                Fit(hint, ScreenWidth - Margin - text_x).c_str());
    dl->PopClipRect();

    ImGui::SetCursorScreenPos(ImVec2(Margin, footer + (FooterHeight - FooterButtonHeight) * 0.5f));
    if (FooterButton(BackGlyph(), Tr("Back"))) {
        if (ed.changed) {
            question = Question::DiscardControls;
        } else {
            CloseControlEditor();
        }
    }
    if (FooterButton(ConfirmGlyph(), Tr("Buttons"))) {
        ed.panel = EditorPanel::Tools;
        focus_first_row = true;
    }
    if (FooterButton(Glyph::Triangle, Tr("Next button"))) {
        ed.selected = NextOnScreen(ed.layout, ed.selected);
    }
    FooterButton(Glyph::Shoulders, Tr("Size"));
    if (FooterButton(Glyph::Start, Tr("Save"))) {
        ed.panel = EditorPanel::Save;
        focus_first_row = true;
    }
    ImGui::End();
}

void Ui::DrawControlButtons() {
    constexpr float Pad = 18.0f;
    constexpr float Width = 640.0f;
    ControlEditor& ed = control_editor;
    const bool deletable = ed.index.has_value();
    if (!ImGui::GetIO().MouseDown[0] && ImGui::GetActiveID() == 0) {
        ImGui::SetNextWindowFocus();
    }
    BeginCentered("##control_buttons", Width, 480.0f);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), TitleSize, ImGui::GetCursorScreenPos(),
                                        Colour(palette->Text), Tr("Buttons"));
    ImGui::Dummy(ImVec2(0.0f, TitleSize + 6.0f));
    Note(palette->TextDim, TrFormat("Left and right: the Vita input that presses it. {}: a button "
                                    "on the screen for it, or none.",
                                    ConfirmName()));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 6.0f));
    const float width = ImGui::GetContentRegionAvail().x;
    // A row's step from the pad's left and right while it is focused, or a tap on its value.
    const auto step_of = [](const Card& card, float value_left) {
        if (card.focused && ImGui::IsNavInputTest(ImGuiNavInput_DpadLeft, ImGuiInputReadMode_Repeat)) {
            return -1;
        }
        if (card.focused &&
            ImGui::IsNavInputTest(ImGuiNavInput_DpadRight, ImGuiInputReadMode_Repeat)) {
            return 1;
        }
        if (card.pressed && !card.by_pad && ImGui::GetIO().MousePos.x >= value_left) {
            return ImGui::GetIO().MousePos.x < (value_left + card.bb.Max.x) * 0.5f ? -1 : 1;
        }
        return 0;
    };
    // The 3DS buttons, then Swap screens: on the screen or not, with no Vita input of its own.
    for (std::size_t i = 0; i < ControlLayouts::SpotCount; i++) {
        const auto button = static_cast<ControlLayouts::Button>(i);
        const bool mapped = i < ControlLayouts::ButtonCount;
        ImGui::PushID(static_cast<int>(i));
        const Card card = CardItem("##button", ImVec2(width, RowHeight));
        if (focus_first_row) {
            focus_first_row = false;
            FocusLastItem();
        }
        const float value_left = card.bb.Min.x + width * 0.45f;
        auto& on_screen = ed.layout.on_screen[i];
        if (card.visible) {
            ImDrawList* const dl = ImGui::GetWindowDrawList();
            const float y = card.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
            const char* const label = Tr(ControlLayouts::Label(button));
            dl->AddText(ImVec2(card.bb.Min.x + Pad, y), Colour(palette->Text), label);
            if (on_screen) {
                const char* const tag = Tr("On screen");
                const ImVec2 size = ImGui::CalcTextSize(tag);
                const ImVec2 at(
                    card.bb.Min.x + Pad + std::max(90.0f, ImGui::CalcTextSize(label).x + 16.0f), y);
                dl->AddRectFilled(at - ImVec2(8.0f, 3.0f), at + size + ImVec2(8.0f, 3.0f),
                                  Colour(palette->Accent), size.y * 0.5f + 3.0f);
                dl->AddText(at, Colour(palette->Backdrop), tag);
            }
            if (mapped) {
                const std::string value = fmt::format(
                    "< {} >", Tr(InputNames[static_cast<std::size_t>(ed.layout.inputs[i])]));
                const float value_width = ImGui::CalcTextSize(value.c_str()).x;
                dl->AddText(ImVec2(card.bb.Max.x - Pad - value_width, y), Colour(palette->TextDim),
                            value.c_str());
            }
        }
        if (const int step = mapped ? step_of(card, value_left) : 0; step != 0) {
            constexpr int Inputs = static_cast<int>(Config::ControlInput::Count);
            ed.layout.inputs[i] = ((ed.layout.inputs[i] + step) % Inputs + Inputs) % Inputs;
            ed.changed = true;
        } else if (card.pressed &&
                   (card.by_pad || !mapped || ImGui::GetIO().MousePos.x < value_left)) {
            if (on_screen) {
                on_screen.reset();
                if (ed.selected == button) {
                    ed.selected = NextOnScreen(ed.layout, button);
                }
            } else {
                on_screen = ControlLayouts::DefaultPlace(button);
                ed.selected = button;
            }
            ed.changed = true;
        }
        ImGui::PopID();
    }
    // The on-screen buttons' opacity: the pad's left and right, or a tap on either half.
    {
        const Card card = CardItem("##opacity", ImVec2(width, RowHeight));
        if (card.visible) {
            ImDrawList* const dl = ImGui::GetWindowDrawList();
            const float y = card.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
            dl->AddText(ImVec2(card.bb.Min.x + Pad, y), Colour(palette->Text),
                        Tr("On-screen buttons' opacity"));
            const std::string value =
                fmt::format("< {}% >", std::lround(ed.layout.opacity * 100.0f));
            const float value_width = ImGui::CalcTextSize(value.c_str()).x;
            dl->AddText(ImVec2(card.bb.Max.x - Pad - value_width, y), Colour(palette->TextDim),
                        value.c_str());
        }
        int step = step_of(card, card.bb.Min.x);
        if (step != 0) {
            ed.layout.opacity = std::clamp(
                std::round(ed.layout.opacity * 20.0f + static_cast<float>(step)) / 20.0f, 0.1f,
                1.0f);
            ed.changed = true;
        }
    }
    if (deletable) {
        const Card card = CardItem("##delete", ImVec2(width, RowHeight));
        if (card.visible) {
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(card.bb.Min.x + Pad, card.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f),
                Colour(palette->Text), Tr("Delete this layout"));
        }
        if (card.pressed) {
            question = Question::DeleteControls;
        }
    }
    ImGui::PopStyleVar();
    const bool back = BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    if (back) {
        ed.panel = EditorPanel::None;
        overlay_focused = false;
    }
}

void Ui::DrawControlSave() {
    constexpr float Padding = 28.0f;
    constexpr float Width = 640.0f;
    ControlEditor& ed = control_editor;
    const std::string name = TrimName(ed.name);
    const std::optional<std::size_t> named = ControlLayouts::Find(name);
    // Save keeps the layout opened under the name, Save as new adds one: neither takes another
    // layout's name.
    const bool save_ok = !name.empty() && (!named || named == ed.index);
    const bool new_ok = !name.empty() && !named;
    const std::string uses = settings_title.empty()
                                 ? std::string{Tr("All games then use it.")}
                                 : TrFormat("{} then uses it.", settings_name);
    const char* const problem = name.empty() ? Tr("Give the layout a name.")
                                : !save_ok   ? Tr("Another layout has this name.")
                                             : nullptr;
    if (!ImGui::GetIO().MouseDown[0] && ImGui::GetActiveID() == 0) {
        ImGui::SetNextWindowFocus();
    }
    BeginCentered("##control_save", Width, 2.0f * Padding + TitleSize + 3.0f * RowHeight + 40.0f);
    ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), TitleSize, ImGui::GetCursorScreenPos(),
                                        Colour(palette->Text), Tr("Save control layout"));
    ImGui::Dummy(ImVec2(0.0f, TitleSize + 6.0f));
    const float width = ImGui::GetContentRegionAvail().x;
    const Card field = CardItem("##control_name", ImVec2(width, RowHeight));
    if (focus_first_row) {
        focus_first_row = false;
        FocusLastItem();
    }
    if (field.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float y = field.bb.GetCenter().y - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(field.bb.Min.x + 18.0f, y), Colour(palette->Text), Tr("Name"));
        const std::string value = Fit(ed.name, width - 160.0f);
        const float value_width = ImGui::CalcTextSize(value.c_str()).x;
        dl->AddText(ImVec2(field.bb.Max.x - 18.0f - value_width, y), Colour(palette->Text),
                    value.c_str());
    }
    if (field.pressed) {
        OpenKeyboard(KeyboardControlName, ed.name);
    }
    Note(problem ? palette->Own : palette->TextDim, problem ? std::string{problem} : uses);
    ImGui::Spacing();
    const ImVec2 button(ed.index ? 180.0f : 200.0f, 48.0f);
    const auto action = [&](const char* label, bool enabled) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, enabled ? 1.0f : 0.4f);
        const bool pressed = ImGui::Button(label, button);
        ImGui::PopStyleVar();
        ImGui::SameLine(0.0f, 16.0f);
        return pressed && enabled;
    };
    const bool save = action(Tr("Save"), save_ok);
    const bool save_new = ed.index && action(Tr("Save as new"), new_ok);
    const bool cancel = action(Tr("Cancel"), true) || BackPressed();
    overlay_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImGui::End();
    if (save || save_new) {
        ed.name = name;
        SaveEditedControls(save_new);
    } else if (cancel) {
        ed.panel = EditorPanel::None;
        overlay_focused = false;
    }
}

void Ui::SaveEditedControls(bool as_new) {
    ControlEditor& ed = control_editor;
    ed.layout.name = ed.name;
    const std::size_t index =
        Config::SaveControlLayout(as_new ? std::nullopt : ed.index, ed.layout);
    const int value = Config::CustomControlValue(index);
    if (settings_title.empty()) {
        Config::SetGlobal(Config::Option::ControlLayout, value);
    } else {
        Config::SetTitleValue(settings_title, Config::Option::ControlLayout, value);
    }
    Config::Save();
    LOG_INFO(Frontend, "control editor: {} saved as control layout {}", ed.name, index);
    CloseControlEditor();
}

Ui::Action Ui::DrawControlEditor() {
    ControlEditor& ed = control_editor;
    const bool covered =
        ed.panel != EditorPanel::None || editing_field >= 0 || question != Question::None;
    if (!covered) {
        EditControls();
        if (state != State::ControlEditor) {
            return Action::None;
        }
    } else {
        // Under a panel the pad and the touch are its: a press or a finger still down as it
        // closes is not the editor's.
        SceCtrlData pad{};
        sceCtrlPeekBufferPositive(0, &pad, 1);
        ed.pad = pad.buttons;
        ed.touches = 2;
        ed.preview_touch = false;
    }
    DrawControlPreview();
    if (ed.panel == EditorPanel::None && editing_field < 0) {
        // The bars hide while a finger on the preview is down: one dragging a button under them
        // presses none of theirs.
        if (!(ed.preview_touch && ed.touches > 0)) {
            DrawControlBars();
        }
    } else if (editing_field >= 0) {
        DrawKeyboard();
    } else if (ed.panel == EditorPanel::Tools) {
        DrawControlButtons();
    } else {
        DrawControlSave();
    }
    return Action::None;
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
    // The settings' right stick scrolls the description (DrawSettings), not the cursor.
    ImGui_ImplVita_MouseStickUsage(state != State::Settings);
    ImGui_ImplVita_NewFrame();

    Action action = Action::None;
    if (!status_message.empty()) {
        DrawStatusMessage();
    } else {
        // A question goes over the screen it was asked from, which is drawn as it is but takes no
        // input meanwhile.
        const bool searching = engine_search.joinable();
        const bool asking = question != Question::None || searching;
        // The question's own focus at the end of the frame before (BackPressed's): the screen
        // behind sets overlay_focused as it is drawn, unfocused while the question is up, and
        // circle would then never cancel the question.
        const bool question_focused = overlay_focused;
        std::optional<InputBlackout> blackout;
        if (asking) {
            // An item behind still held, as the game's tile whose cross opened the question is,
            // would never see the button let go (the blackout hides it) and would keep the pad's
            // moves from the question: it is let go of here.
            const ImGuiContext& g = *ImGui::GetCurrentContext();
            if (g.ActiveId != 0 &&
                (g.ActiveIdWindow == nullptr || std::strcmp(g.ActiveIdWindow->Name, "##question"))) {
                ImGui::ClearActiveID();
            }
            blackout.emplace();
        }
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
        case State::LayoutEditor:
            action = DrawLayoutEditor();
            break;
        case State::ControlEditor:
            action = DrawControlEditor();
            break;
        }
        if (asking) {
            blackout.reset();
            overlay_focused = question_focused;
            if (searching) {
                DrawEngineSearch();
            } else {
                action = DrawQuestion();
            }
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
        window.DrawOnScreenButtons();
        StatsHud::Draw();
        return;
    }
    ImGui_ImplSgxd_RenderDrawData(ImGui::GetDrawData());
}

} // namespace VitaFrontend

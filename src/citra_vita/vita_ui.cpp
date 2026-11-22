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
#include <psp2/kernel/processmgr.h>

#include "citra_vita/imgui_impl_sgxd.h"
#include "citra_vita/imgui_impl_vita.h"

#include <fmt/format.h>

#include "citra_vita/config.h"
#include "citra_vita/crash_report.h"
#include "citra_vita/emu_window_vita.h"
#include "citra_vita/hud_stats.h"
#include "citra_vita/sgxd_present.h"
#include "citra_vita/stats_hud.h"
#include "citra_vita/vita_ui.h"
#include "common/assert.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"

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

/// Where Ui keeps the titles' names between runs (Ui::title_cache): a line a file, its size, path
/// and name, tab-separated.
constexpr char TitleCacheFile[] = "ux0:data/azahar/titles.txt";

/// The title's short name in English from its SMDH (a 3DSX's own, when it has one), on one line;
/// empty when it has none.
std::string ReadShortTitle(const std::string& path) {
    const std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(path);
    std::vector<u8> smdh;
    if (!loader || loader->ReadIcon(smdh) != Loader::ResultStatus::Success ||
        !Loader::IsValidSMDH(smdh)) {
        return {};
    }
    Loader::SMDH data;
    std::memcpy(&data, smdh.data(), sizeof(data));
    const auto title = data.GetShortTitle(Loader::SMDH::TitleLanguage::English);
    const std::u16string text(title.data(), std::find(title.begin(), title.end(), u'\0'));
    std::string name = Common::UTF16ToUTF8(text);
    std::replace_if(name.begin(), name.end(), [](char c) { return c == '\n' || c == '\t'; }, ' ');
    const auto first = name.find_first_not_of(' ');
    const auto last = name.find_last_not_of(' ');
    return first == std::string::npos ? std::string{} : name.substr(first, last - first + 1);
}

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

/// The menus' colours: the 3DS's dark menus, near-black panels with light text and a soft blue
/// for what is selected, and a warm amber for a title's own setting.
namespace Palette {
const ImVec4 Backdrop{0.075f, 0.082f, 0.098f, 1.0f};
const ImVec4 Panel{0.125f, 0.137f, 0.161f, 0.97f};
const ImVec4 Row{0.176f, 0.192f, 0.224f, 1.0f};
const ImVec4 RowFocused{0.231f, 0.263f, 0.314f, 1.0f};
const ImVec4 Accent{0.369f, 0.769f, 0.941f, 1.0f};
const ImVec4 AccentDim{0.369f, 0.769f, 0.941f, 0.35f};
const ImVec4 Own{0.949f, 0.698f, 0.361f, 1.0f};
const ImVec4 Text{0.925f, 0.937f, 0.945f, 1.0f};
const ImVec4 TextDim{0.588f, 0.627f, 0.667f, 1.0f};
const ImVec4 Line{1.0f, 1.0f, 1.0f, 0.08f};
const ImVec4 SwitchOff{0.333f, 0.361f, 0.408f, 1.0f};
} // namespace Palette

ImU32 Colour(const ImVec4& colour, float alpha = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(colour.x, colour.y, colour.z, colour.w * alpha));
}

/// The screens' frame: the header with the screen's name, the footer with its buttons, the
/// margin around them, a row's height and rounding. A row is a finger high.
constexpr float Margin = 16.0f;
constexpr float HeaderHeight = 64.0f;
constexpr float FooterHeight = 56.0f;
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
    c[ImGuiCol_Text] = Palette::Text;
    c[ImGuiCol_TextDisabled] = Palette::TextDim;
    c[ImGuiCol_WindowBg] = Palette::Panel;
    c[ImGuiCol_ChildBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_PopupBg] = Palette::Panel;
    c[ImGuiCol_Border] = Palette::Line;
    c[ImGuiCol_FrameBg] = Palette::Row;
    c[ImGuiCol_FrameBgHovered] = Palette::RowFocused;
    c[ImGuiCol_FrameBgActive] = Palette::RowFocused;
    c[ImGuiCol_Button] = Palette::Row;
    c[ImGuiCol_ButtonHovered] = Palette::RowFocused;
    c[ImGuiCol_ButtonActive] = Palette::AccentDim;
    c[ImGuiCol_Header] = Palette::Row;
    c[ImGuiCol_HeaderHovered] = Palette::RowFocused;
    c[ImGuiCol_HeaderActive] = Palette::AccentDim;
    c[ImGuiCol_CheckMark] = Palette::Accent;
    c[ImGuiCol_SliderGrab] = Palette::Accent;
    c[ImGuiCol_SliderGrabActive] = Palette::Accent;
    c[ImGuiCol_Separator] = Palette::Line;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Palette::Row;
    c[ImGuiCol_ScrollbarGrabHovered] = Palette::RowFocused;
    c[ImGuiCol_ScrollbarGrabActive] = Palette::AccentDim;
    c[ImGuiCol_NavHighlight] = Palette::Accent;
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
    Refresh,
    Patch,
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
    case Icon::Refresh: {
        const float r = 0.32f * s;
        const float start = -0.25f * IM_PI;
        dl->PathArcTo(c, r, start + 0.35f, start + 1.75f * IM_PI, 20);
        dl->PathStroke(colour, false, 0.09f * s);
        // The arrow's head at the arc's start, pointing back along it.
        const ImVec2 radial(std::cos(start + 0.35f), std::sin(start + 0.35f));
        const ImVec2 back(radial.y, -radial.x);
        const ImVec2 at = c + radial * r;
        dl->AddTriangleFilled(at + back * (0.2f * s), at + radial * (0.15f * s),
                              at - radial * (0.15f * s), colour);
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
    }
}

enum class Glyph {
    Cross,
    Circle,
    Triangle,
    LeftRight,
    Shoulders,
};

/// A pad button's glyph centred at `c`, `r` its radius: the face buttons in their own colours on
/// a dark disc, as the console's own menus show them.
void DrawGlyph(ImDrawList* dl, Glyph glyph, ImVec2 c, float r) {
    const ImU32 disc = Colour(Palette::SwitchOff);
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
    case Glyph::Triangle:
        dl->AddCircleFilled(c, r, disc, 20);
        dl->AddTriangle(ImVec2(c.x, c.y - 0.48f * r), ImVec2(c.x - 0.46f * r, c.y + 0.32f * r),
                        ImVec2(c.x + 0.46f * r, c.y + 0.32f * r),
                        Colour(ImVec4(0.42f, 0.86f, 0.72f, 1.0f)), line);
        break;
    case Glyph::LeftRight:
        dl->AddCircleFilled(c, r, disc, 20);
        dl->AddTriangleFilled(ImVec2(c.x - 0.6f * r, c.y), ImVec2(c.x - 0.12f * r, c.y - 0.4f * r),
                              ImVec2(c.x - 0.12f * r, c.y + 0.4f * r), Colour(Palette::Text));
        dl->AddTriangleFilled(ImVec2(c.x + 0.6f * r, c.y), ImVec2(c.x + 0.12f * r, c.y - 0.4f * r),
                              ImVec2(c.x + 0.12f * r, c.y + 0.4f * r), Colour(Palette::Text));
        break;
    case Glyph::Shoulders: {
        // L and R, two pills.
        const ImVec2 half(0.95f * r, 0.7f * r);
        const std::pair<float, const char*> pills[] = {{c.x - 1.05f * r, "L"},
                                                       {c.x + 1.05f * r, "R"}};
        for (const auto& [x, letter] : pills) {
            dl->AddRectFilled(ImVec2(x, c.y) - half, ImVec2(x, c.y) + half, disc, 0.5f * r);
            const ImVec2 size = ImGui::CalcTextSize(letter);
            dl->AddText(ImVec2(x - size.x * 0.5f, c.y - size.y * 0.5f), Colour(Palette::Text),
                        letter);
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
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Palette::Backdrop);
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
    const ImU32 panel = Colour(Palette::Panel, 1.0f / Palette::Panel.w);
    dl->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(ScreenWidth, HeaderHeight), panel);
    dl->AddLine(ImVec2(0.0f, HeaderHeight), ImVec2(ScreenWidth, HeaderHeight),
                Colour(Palette::Line));
    DrawIcon(dl, icon, ImVec2(Margin + 16.0f, HeaderHeight * 0.5f), 30.0f, Colour(Palette::Accent),
             panel);
    const float title_x = Margin + 44.0f;
    dl->AddText(ImGui::GetFont(), TitleSize, ImVec2(title_x, (HeaderHeight - TitleSize) * 0.5f),
                Colour(Palette::Text), title);
    const float title_width = ImGui::GetFont()->CalcTextSizeA(TitleSize, FLT_MAX, 0.0f, title).x;
    // The note at the header's right on one line where it fits; else (a long game's name) over
    // two lines from the title on, and shortened only past those.
    const float note_x = title_x + title_width + 32.0f;
    const float note_room = ScreenWidth - Margin - note_x;
    if (const ImVec2 size = ImGui::CalcTextSize(note.c_str()); size.x <= note_room) {
        dl->AddText(ImVec2(ScreenWidth - Margin - size.x, (HeaderHeight - size.y) * 0.5f),
                    Colour(Palette::TextDim), note.c_str());
    } else {
        const std::string shown = Fit(note, note_room, 2);
        const ImVec2 wrapped = ImGui::CalcTextSize(shown.c_str(), nullptr, false, note_room);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(note_x, (HeaderHeight - wrapped.y) * 0.5f), Colour(Palette::TextDim),
                    shown.c_str(), nullptr, note_room);
    }

    const float footer = ScreenHeight - FooterHeight;
    dl->AddRectFilled(ImVec2(0.0f, footer), ImVec2(ScreenWidth, ScreenHeight), panel);
    dl->AddLine(ImVec2(0.0f, footer), ImVec2(ScreenWidth, footer), Colour(Palette::Line));
    ImGui::SetCursorScreenPos(ImVec2(Margin, footer + (FooterHeight - 40.0f) * 0.5f));
}

void EndBackdrop() {
    ImGui::End();
}

/// One of the footer's buttons: the pad button's glyph and what it does, a touch target for the
/// same. Laid out left to right.
bool FooterButton(Glyph glyph, const char* label) {
    constexpr float R = 13.0f;
    const ImVec2 text = ImGui::CalcTextSize(label);
    const float glyph_width = GlyphWidth(glyph, R);
    const ImVec2 size(glyph_width + 10.0f + text.x + 20.0f, 40.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(label, size);
    ImDrawList* const dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemActive()) {
        dl->AddRectFilled(at, at + size, Colour(Palette::AccentDim), size.y * 0.5f);
    }
    DrawGlyph(dl, glyph, ImVec2(at.x + 10.0f + glyph_width * 0.5f, at.y + size.y * 0.5f), R);
    dl->AddText(ImVec2(at.x + 10.0f + glyph_width + 10.0f, at.y + (size.y - text.y) * 0.5f),
                Colour(Palette::Text), label);
    ImGui::SameLine(0.0f, 12.0f);
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

Card CardItem(const char* str_id, ImVec2 size) {
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
    if (card.visible) {
        ImDrawList* const dl = window->DrawList;
        dl->AddRectFilled(card.bb.Min, card.bb.Max,
                          Colour(card.focused || held ? Palette::RowFocused : Palette::Row),
                          RowRounding);
        if (card.focused) {
            dl->AddRect(card.bb.Min + ImVec2(1.0f, 1.0f), card.bb.Max - ImVec2(1.0f, 1.0f),
                        Colour(Palette::Accent), RowRounding, ImDrawCornerFlags_All, 2.5f);
        }
    }
    return card;
}

/// A card with an icon and a label, for the pause menu's tiles and the game list's tools.
bool IconCard(const char* label, Icon icon, ImVec2 size) {
    const Card card = CardItem(label, size);
    if (card.visible) {
        ImDrawList* const dl = ImGui::GetWindowDrawList();
        const float icon_size = std::min(size.y * 0.5f, 30.0f);
        const ImVec2 icon_at(card.bb.Min.x + 14.0f + icon_size * 0.5f, card.bb.GetCenter().y);
        DrawIcon(dl, icon, icon_at, icon_size, Colour(Palette::Accent),
                 Colour(card.focused ? Palette::RowFocused : Palette::Row));
        const ImVec2 text = ImGui::CalcTextSize(label);
        dl->AddText(
            ImVec2(icon_at.x + icon_size * 0.5f + 14.0f, card.bb.GetCenter().y - text.y * 0.5f),
            Colour(Palette::Text), label);
    }
    return card.pressed;
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
    static constexpr const char* names[] = {"Display", "Speed", "Sound", "System", "Patches"};
    return names[category];
}

Icon CategoryIcon(int category) {
    static constexpr Icon icons[] = {Icon::Screens, Icon::Gauge, Icon::Speaker, Icon::Globe,
                                     Icon::Patch};
    return icons[category];
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

    ImGui_ImplVita_Init();
    ImGui_ImplSgxd_Init();
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
        title_cache[text.substr(tab1 + 1, tab2 - tab1 - 1)] = {
            std::strtoull(text.c_str(), nullptr, 10), text.substr(tab2 + 1)};
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
        std::fprintf(f, "%llu\t%s\t%s\n", static_cast<unsigned long long>(title.size), path.c_str(),
                     title.name.c_str());
    }
    std::fclose(f);
}

void Ui::ScanGames() {
    if (!title_cache_loaded) {
        LoadTitleCache();
    }
    games.clear();
    bool cache_changed = false;
    const auto scan = [&](const char* directory, const char* suffix) {
        FileUtil::FSTEntry root;
        if (!FileUtil::ScanDirectoryTree(directory, root)) {
            LOG_WARNING(Frontend, "No game directory at {}", directory);
            return;
        }
        for (const auto& entry : root.children) {
            if (entry.isDirectory || !HasRomExtension(entry.virtualName)) {
                continue;
            }
            auto cached = title_cache.find(entry.physicalName);
            if (cached == title_cache.end() || cached->second.size != entry.size) {
                cached = title_cache
                             .insert_or_assign(
                                 entry.physicalName,
                                 CachedTitle{entry.size, ReadShortTitle(entry.physicalName)})
                             .first;
                cache_changed = true;
            }
            const std::string file = entry.virtualName + suffix;
            const std::string& title = cached->second.name;
            games.push_back({entry.physicalName, title.empty() ? file : title + suffix,
                             title.empty() ? std::string{} : entry.virtualName});
        }
    };
    scan(GameDirectory, "");
#ifdef VITA_DIAGNOSTICS
    scan(HostGameDirectory, " (host0)");
#endif
    if (cache_changed) {
        SaveTitleCache();
    }
    std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) {
        return NaturalLess(a.name, b.name) ||
               (!NaturalLess(b.name, a.name) && NaturalLess(a.file, b.file));
    });
    selected_game = std::clamp(selected_game, 0, std::max(static_cast<int>(games.size()) - 1, 0));
    LOG_INFO(Frontend, "Found {} titles", games.size());
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

    // System.
    add({Option::Region,
         Category::System,
         Kind::Choice,
         "Region",
         "The console region reported to the game. Auto picks one the game accepts.",
         {"Auto", "Japan", "USA", "Europe", "Australia", "China", "Korea", "Taiwan"},
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

void Ui::ClearStatusMessage() {
    status_message.clear();
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
    category = SettingsCategories().front();
    focused_row = -1;
    focused_patch = -1;
    focus_first_row = true;
    state = State::Settings;
    overlay_focused = false;
}

std::vector<Ui::Category> Ui::SettingsCategories() const {
    std::vector<Category> shown;
    for (int c = 0; c < static_cast<int>(Category::Count); c++) {
        const auto tab = static_cast<Category>(c);
        const bool has_rows =
            tab == Category::Patches
                ? !settings_patches.empty()
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
    if (row.option == Config::Option::SpeedLimit) {
        const int vsync = settings_title.empty()
                              ? Config::Global(Config::Option::Vsync)
                              : Config::ForTitle(settings_title, Config::Option::Vsync);
        if (vsync != 0) {
            return "Off while Vsync is on: vsync already holds the game to the display's rate. "
                   "Turn Vsync off to set a speed limit.";
        }
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
                              Colour(Palette::Own), 2.0f);
        }
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(card.bb.Min.x + Pad, mid - label.y * 0.5f), Colour(Palette::Text, alpha),
                    row.title, nullptr, label_width);
        // On a title's screen its own value is amber, all games' it follows plain.
        const ImU32 value_colour = Colour(for_title && own ? Palette::Own : Palette::Text, alpha);
        switch (row.kind) {
        case Row::Kind::Toggle: {
            const ImVec2 min(value_bb.Max.x - SwitchWidth, mid - 15.0f);
            const ImVec2 max(value_bb.Max.x, mid + 15.0f);
            dl->AddRectFilled(
                min, max, Colour(value != 0 ? Palette::Accent : Palette::SwitchOff, alpha), 15.0f);
            const float knob = value != 0 ? max.x - 15.0f : min.x + 15.0f;
            dl->AddCircleFilled(ImVec2(knob, mid), 11.0f, Colour(Palette::Text, alpha), 16);
            if (for_title && marked) {
                dl->AddCircle(ImVec2(knob, mid), 11.0f, Colour(Palette::Own, alpha), 16, 2.0f);
            }
            break;
        }
        case Row::Kind::Choice: {
            const ImU32 arrows = Colour(focused ? Palette::Accent : Palette::TextDim, alpha);
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
                              Colour(Palette::SwitchOff, alpha), 4.0f);
            dl->AddRectFilled(min, ImVec2(min.x + BarWidth * t, mid + 4.0f),
                              Colour(Palette::Accent, alpha), 4.0f);
            dl->AddCircleFilled(ImVec2(min.x + BarWidth * t, mid), 10.0f,
                                Colour(Palette::Text, alpha), 16);
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
        Note(Palette::Accent, reason);
    }
    if (for_title && own) {
        Note(Palette::Own, fmt::format("Set for this game. All games: {}.",
                                       ValueText(row, Config::Global(row.option))));
    } else if (for_title) {
        Note(Palette::TextDim, "Follows the setting for all games.");
    } else if (!game_key.empty() && own) {
        Note(Palette::Own, fmt::format("{} sets its own: {}. Change it in Game settings.",
                                       game_name, ValueText(row, *own)));
    }
    if (const std::optional<int> pin = Config::PinOf(row.option)) {
        Note(Palette::Accent, fmt::format("A launch argument holds this at {} until Azahar exits.",
                                          ValueText(row, *pin)));
    }
    if (!Config::Runtime(row.option) && !game_key.empty()) {
        Note(Palette::Accent, "Takes effect when a game starts.");
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
    // The Patches tab changes nothing.
    const bool patches = category == Category::Patches;
    bool back = FooterButton(Glyph::Circle, "Back");
    if (!patches) {
        FooterButton(Glyph::LeftRight, "Change");
    }
    if (categories.size() > 1 && FooterButton(Glyph::Shoulders, "Tab") && !next_category) {
        const int at = static_cast<int>(current - categories.begin());
        next_category = categories[static_cast<std::size_t>(at + 1) % categories.size()];
    }
    if (for_title && !patches && FooterButton(Glyph::Triangle, "Use all games' setting") &&
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
                              Colour(selected ? Palette::Accent : Palette::Row), size.y * 0.5f);
            const ImU32 ink = Colour(selected ? Palette::Backdrop : Palette::Text);
            DrawIcon(dl, CategoryIcon(i), ImVec2(x + 26.0f, y + size.y * 0.5f), 22.0f, ink,
                     Colour(selected ? Palette::Accent : Palette::Row));
            dl->AddText(ImVec2(x + 46.0f, y + (size.y - text.y) * 0.5f), ink, name);
            x += size.x + 10.0f;
        }
    }
    EndBackdrop();

    if (next_category && *next_category != category) {
        category = *next_category;
        focused_row = -1;
        focused_patch = -1;
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

Ui::Action Ui::DrawGameList() {
    Action action = Action::None;
    const std::string note = fmt::format("{} {} in {}", games.size(),
                                         games.size() == 1 ? "title" : "titles", ListedDirectories);
    BeginBackdrop("##gamelist_backdrop", Icon::Home, "Azahar", note);
    bool start = !games.empty() && FooterButton(Glyph::Cross, "Start");
    bool game_settings = !games.empty() && FooterButton(Glyph::Triangle, "Game settings");
    EndBackdrop();

    BeginList("##games", ImVec2(Margin - 4.0f, HeaderHeight + 8.0f),
              ImVec2(ScreenWidth - Margin + 4.0f, ScreenHeight - FooterHeight - 8.0f));
    // The tools first, as the 3DS's HOME Menu has its own above the games.
    const ImVec2 tool(180.0f, 48.0f);
    if (IconCard("Settings", Icon::Gear, tool)) {
        OpenSettings({}, {}, State::GameList);
    }
    ImGui::SameLine();
    if (IconCard("Rescan", Icon::Refresh, tool)) {
        ScanGames();
    }
    ImGui::SameLine();
    if (IconCard("Exit", Icon::Power, tool)) {
        action = Action::ExitApplication;
    }
    ImGui::Spacing();

    if (games.empty()) {
        ImGui::TextWrapped("No titles found. Copy .3ds or .cci files into %s and choose Rescan.",
                           GameDirectory);
    }
    const float width = ImGui::GetContentRegionAvail().x;
    for (int i = 0; i < static_cast<int>(games.size()); i++) {
        ImGui::PushID(i);
        // The name wraps rather than being cut; the right end is the game's settings, for touch.
        constexpr float GearRoom = 56.0f;
        const float name_width = width - 64.0f - GearRoom;
        const ImVec2 name = ImGui::CalcTextSize(games[i].name.c_str(), nullptr, false, name_width);
        // The file's name under the title's, dimmer, where the title has its own.
        const float file_height = games[i].file.empty() ? 0.0f : ImGui::GetFontSize() + 4.0f;
        const Card card =
            CardItem("##game", ImVec2(width, std::max(RowHeight, name.y + file_height + 28.0f)));
        // Back on the list: the game selected before. Not when a tool has just opened another
        // screen, which selects its own first item.
        if (focus_first_row && i == selected_game && state == State::GameList) {
            focus_first_row = false;
            FocusLastItem();
        }
        if (ImGui::GetFocusID() == card.id) {
            selected_game = i;
        }
        const ImRect gear(card.bb.Max.x - GearRoom, card.bb.Min.y, card.bb.Max.x, card.bb.Max.y);
        if (card.pressed) {
            selected_game = i;
            const bool on_gear = !card.by_pad && gear.Contains(ImGui::GetIO().MousePos);
            (on_gear ? game_settings : start) = true;
        }
        if (card.visible) {
            ImDrawList* const dl = ImGui::GetWindowDrawList();
            const ImU32 under = Colour(card.focused ? Palette::RowFocused : Palette::Row);
            const float mid = card.bb.GetCenter().y;
            DrawIcon(dl, Icon::Cartridge, ImVec2(card.bb.Min.x + 30.0f, mid), 28.0f,
                     Colour(Palette::Accent), under);
            const float top = mid - (name.y + file_height) * 0.5f;
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(card.bb.Min.x + 64.0f, top),
                        Colour(Palette::Text), games[i].name.c_str(), nullptr, name_width);
            if (!games[i].file.empty()) {
                const std::string file = Fit(games[i].file, name_width);
                dl->AddText(ImVec2(card.bb.Min.x + 64.0f, top + name.y + 4.0f),
                            Colour(Palette::TextDim), file.c_str());
            }
            DrawIcon(dl, Icon::Gear, gear.GetCenter(), 24.0f,
                     Colour(card.focused ? Palette::Accent : Palette::TextDim), under);
        }
        ImGui::PopID();
    }
    if (state == State::GameList) {
        focus_first_row = false;
    }
    // Triangle on the list: the selected game's settings.
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // Nothing selected (a touch scrolled the selection out of view and a pad move found no item
    // beside it, so cross pressed nothing): the game selected last, back in view, next frame.
    if (state == State::GameList && focused && !games.empty() && ImGui::GetFocusID() == 0) {
        focus_first_row = true;
    }
    if (focused && !games.empty() &&
        ImGui::IsNavInputTest(ImGuiNavInput_Input, ImGuiInputReadMode_Pressed)) {
        game_settings = true;
    }
    overlay_focused = focused;
    ImGui::End();

    if (games.empty()) {
        return action;
    }
    const GameEntry& game = games[static_cast<std::size_t>(selected_game)];
    if (game_settings) {
        OpenSettings(Config::TitleKeyOf(game.path), game.name, State::GameList,
                     ProgramIdOf(game.path));
    } else if (start) {
        selected_game_path = game.path;
        action = Action::LaunchGame;
    }
    return action;
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
        dl->AddText(ImGui::GetFont(), TitleSize, at, Colour(Palette::Text), "Paused");
        const float room = ImGui::GetContentRegionAvail().x - 130.0f;
        const std::string name = Fit(game_name, room);
        const ImVec2 size = ImGui::CalcTextSize(name.c_str());
        dl->AddText(ImVec2(at.x + ImGui::GetContentRegionAvail().x - size.x,
                           at.y + (TitleSize - size.y) * 0.5f),
                    Colour(Palette::TextDim), name.c_str());
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
                                                  Colour(Palette::Row), RowRounding);
        ImGui::SetCursorScreenPos(at + ImVec2(18.0f, (52.0f - ImGui::GetFontSize()) * 0.5f));
        ImGui::Text("Slot %u", slot);
        ImGui::SetCursorScreenPos(at + ImVec2(width - 2.0f * button.x - 16.0f, 6.0f));
        if (ImGui::Button("Save", button)) {
            selected_slot = slot;
            action = Action::SaveState;
        }
        if (focus_first_row && slot == 1) {
            focus_first_row = false;
            FocusLastItem();
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (ImGui::Button("Load", button)) {
            selected_slot = slot;
            action = Action::LoadState;
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
            dl->AddText(ImVec2(card.bb.Min.x + 18.0f, y), Colour(Palette::Text), fields[i].first);
            const bool empty = fields[i].second->empty();
            const std::string value = Fit(empty ? "(none)" : *fields[i].second, width - 190.0f);
            const float value_width = ImGui::CalcTextSize(value.c_str()).x;
            dl->AddText(ImVec2(card.bb.Max.x - 18.0f - value_width, y),
                        Colour(empty ? Palette::TextDim : Palette::Text), value.c_str());
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
        Note(Palette::Accent, member);
    }
    if (!can_join) {
        Note(Palette::TextDim, "Join needs an address and a nickname.");
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
    // A panel as tall as its text, the text centred in it (a line across, wrapped lines left).
    constexpr float Width = 560.0f;
    constexpr float Padding = 28.0f;
    const ImVec2 text =
        ImGui::CalcTextSize(status_message.c_str(), nullptr, false, Width - 2.0f * Padding);
    BeginCentered("##status", Width, text.y + 2.0f * Padding);
    ImGui::SetCursorPos(ImVec2(std::max(Padding, (Width - text.x) * 0.5f), Padding));
    ImGui::PushTextWrapPos(Width - Padding);
    ImGui::TextUnformatted(status_message.c_str());
    ImGui::PopTextWrapPos();
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

// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <imgui_vita2d/imgui.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/pvf.h>

#include "citra_vita/i18n.h"
#include "citra_vita/menu_glyphs.h"
#include "common/assert.h"
#include "common/logging/log.h"

namespace VitaFrontend::MenuGlyphs {

namespace {

/// A character drawn, waiting for the atlas: its size, advance and corner (from the pen on the
/// baseline), its coverage a byte a pixel, and its rectangle in the atlas.
struct Glyph {
    u16 code;
    int width;
    int height;
    float advance;
    ImVec2 corner;
    std::vector<u8> coverage;
    int rect = -1;
};

/// Transparent pixels round each glyph's image rectangle, which its ink can pass.
constexpr int GlyphMargin = 2;
/// How far the characters sit below the font's own place for them, in the font's pixels: their
/// middle where the Latin capitals' is (ProggyClean's capitals stand lower than a Chinese
/// character's middle on the same baseline).
constexpr float GlyphDrop = 2.0f;

/// The glyphs as the last start drew them: drawing them from the system's font takes seconds,
/// reading them back a few ms. A header (CacheMagic, the key, the count), then each glyph: its
/// code, width, height and a pad (u16 each), its advance and corner (floats), its coverage.
constexpr char CacheFile[] = "ux0:data/azahar/menu_glyphs.bin";
constexpr u32 CacheMagic = 0x4c475a41; // "AZGL"

/// What the glyphs were drawn for: the characters, their size, margin and drop, and the cache's
/// layout (FNV-1a). Another key draws them again.
u32 CacheKey(const std::vector<u16>& characters, float size) {
    u32 hash = 2166136261u;
    const auto mix = [&](u32 value) {
        for (int i = 0; i < 4; i++) {
            hash = (hash ^ ((value >> (8 * i)) & 0xff)) * 16777619u;
        }
    };
    mix(1); // the layout's version
    mix(static_cast<u32>(size * 64.0f));
    mix(GlyphMargin);
    mix(static_cast<u32>(GlyphDrop * 64.0f));
    for (const u16 code : characters) {
        mix(code);
    }
    return hash;
}

/// The cache's glyphs, or none where it is missing, cut short or for another key.
std::vector<Glyph> ReadCache(u32 key, std::size_t count) {
    std::vector<u8> bytes;
    if (std::FILE* const f = std::fopen(CacheFile, "rb")) {
        std::fseek(f, 0, SEEK_END);
        bytes.resize(static_cast<std::size_t>(std::max(0L, std::ftell(f))));
        std::fseek(f, 0, SEEK_SET);
        bytes.resize(std::fread(bytes.data(), 1, bytes.size(), f));
        std::fclose(f);
    }
    std::size_t at = 0;
    const auto take = [&](void* out, std::size_t n) {
        if (at + n > bytes.size()) {
            return false;
        }
        std::memcpy(out, bytes.data() + at, n);
        at += n;
        return true;
    };
    u32 header[3]{};
    if (!take(header, sizeof(header)) || header[0] != CacheMagic || header[1] != key ||
        header[2] != count) {
        return {};
    }
    std::vector<Glyph> read(count);
    for (Glyph& glyph : read) {
        u16 sizes[4]{};
        float metrics[3]{};
        if (!take(sizes, sizeof(sizes)) || !take(metrics, sizeof(metrics))) {
            return {};
        }
        glyph.code = sizes[0];
        glyph.width = sizes[1];
        glyph.height = sizes[2];
        glyph.advance = metrics[0];
        glyph.corner = ImVec2(metrics[1], metrics[2]);
        glyph.coverage.resize(static_cast<std::size_t>(glyph.width * glyph.height));
        if (glyph.coverage.empty() || !take(glyph.coverage.data(), glyph.coverage.size())) {
            return {};
        }
    }
    return at == bytes.size() ? read : std::vector<Glyph>{};
}

void WriteCache(u32 key, const std::vector<Glyph>& drawn) {
    // Made whole and written at once: a write a glyph was slower than drawing them.
    std::vector<u8> bytes;
    const auto put = [&](const void* data, std::size_t n) {
        const auto* const from = static_cast<const u8*>(data);
        bytes.insert(bytes.end(), from, from + n);
    };
    const u32 header[3]{CacheMagic, key, static_cast<u32>(drawn.size())};
    put(header, sizeof(header));
    for (const Glyph& glyph : drawn) {
        const u16 sizes[4]{glyph.code, static_cast<u16>(glyph.width),
                           static_cast<u16>(glyph.height), 0};
        const float metrics[3]{glyph.advance, glyph.corner.x, glyph.corner.y};
        put(sizes, sizeof(sizes));
        put(metrics, sizeof(metrics));
        put(glyph.coverage.data(), glyph.coverage.size());
    }
    // A file cut short reads as no cache (ReadCache), and the next start draws again.
    std::FILE* const f = std::fopen(CacheFile, "wb");
    const bool written = f != nullptr && std::fwrite(bytes.data(), bytes.size(), 1, f) == 1;
    if (f == nullptr || std::fclose(f) != 0 || !written) {
        LOG_WARNING(Frontend, "Could not write {}", CacheFile);
    }
}

void* PvfAlloc(void*, ScePvfU32 size) {
    return std::malloc(size);
}

void* PvfRealloc(void*, void* old, ScePvfU32 size) {
    return std::realloc(old, size);
}

void PvfFree(void*, void* pointer) {
    std::free(pointer);
}

/// The characters drawn from the console's Chinese font at `size` pixels.
std::vector<Glyph> DrawFromFont(const std::vector<u16>& characters, float size) {
    ScePvfError error = 0;
    ScePvfInitRec init{nullptr, 1, nullptr, nullptr, PvfAlloc, PvfRealloc, PvfFree};
    const ScePvfLibId lib = scePvfNewLib(&init, &error);
    ASSERT_MSG(lib != nullptr && error == 0,
               "menus: the system's font library did not start ({:#010x})",
               static_cast<u32>(error));
    // Points at 72 to the inch: a point a pixel.
    scePvfSetResolution(lib, 72.0f, 72.0f);
    ScePvfFontStyleInfo style{};
    style.languageCode = SCE_PVF_LANGUAGE_C;
    const ScePvfFontIndex index = scePvfFindOptimumFont(lib, &style, &error);
    ASSERT_MSG(index >= 0 && error == 0, "menus: no Chinese system font ({:#010x})",
               static_cast<u32>(error));
    // Read from the file as glyphs are drawn: the font is 8 MiB, the menus' characters a few
    // hundred.
    const ScePvfFontId pvf = scePvfOpen(lib, index, SCE_PVF_FILEBASEDSTREAM, &error);
    ASSERT_MSG(pvf != nullptr && error == 0,
               "menus: the Chinese system font did not open ({:#010x})", static_cast<u32>(error));
    scePvfSetCharSize(pvf, size, size);

    std::vector<Glyph> drawn;
    drawn.reserve(characters.size());
    for (const u16 code : characters) {
        // The bitmap's size from its image rectangle and its place from the metrics' bearings:
        // the character information's own bitmap fields read 0 (retail 3.60).
        ScePvfCharInfo info{};
        ScePvfIrect rect{};
        const ScePvfError got = scePvfGetCharInfo(pvf, code, &info);
        const ScePvfError sized = scePvfGetCharImageRect(pvf, code, &rect);
        ASSERT_MSG(got == 0 && sized == 0,
                   "menus: the Chinese system font has no U+{:04X} ({:#010x}, {:#010x})", code,
                   static_cast<u32>(got), static_cast<u32>(sized));
        ASSERT_MSG(rect.width > 0 && rect.height > 0,
                   "menus: U+{:04X} is {}x{} in the Chinese system font", code, rect.width,
                   rect.height);
        // Placed from the pen on the baseline (Draw moves it under the font's ascent), with a
        // margin round the rectangle, which the ink reaches past, and lowered so that the
        // characters' middle is the Latin capitals'.
        const ScePvfIGlyphMetricsInfo& m = info.glyphMetrics;
        Glyph glyph{
            code,
            rect.width + 2 * GlyphMargin,
            rect.height + 2 * GlyphMargin,
            static_cast<float>(m.horizontalAdvance64) / 64.0f,
            ImVec2(static_cast<float>(m.horizontalBearingX64) / 64.0f - GlyphMargin,
                   GlyphDrop - static_cast<float>(m.horizontalBearingY64) / 64.0f - GlyphMargin),
            {}};
        glyph.coverage.resize(static_cast<std::size_t>(glyph.width * glyph.height));
        // The image's position is the pen's, on the baseline: the bitmap's corner is the bearings
        // away from it.
        ScePvfUserImageBufferRec image{};
        image.pixelFormat = SCE_PVF_USERIMAGE_DIRECT8;
        image.xPos64 = GlyphMargin * 64 - m.horizontalBearingX64;
        image.yPos64 = GlyphMargin * 64 + m.horizontalBearingY64;
        image.rect.width = static_cast<ScePvfU16>(glyph.width);
        image.rect.height = static_cast<ScePvfU16>(glyph.height);
        image.bytesPerLine = static_cast<ScePvfU16>(glyph.width);
        image.buffer = glyph.coverage.data();
        const ScePvfError drawn_now = scePvfGetCharGlyphImage(pvf, code, &image);
        ASSERT_MSG(drawn_now == 0, "menus: U+{:04X} not drawn ({:#010x})", code,
                   static_cast<u32>(drawn_now));
        drawn.push_back(std::move(glyph));
    }
    scePvfClose(pvf);
    scePvfDoneLib(lib);
    return drawn;
}

ImFont* glyph_font = nullptr;
std::vector<Glyph> glyphs;

} // Anonymous namespace

void Add(ImFontAtlas& atlas, ImFont* font, float size) {
    const u64 start = sceKernelGetProcessTimeWide();
    const std::vector<u16> characters = I18n::Characters();
    const u32 key = CacheKey(characters, size);
    glyphs = ReadCache(key, characters.size());
    const bool cached = !glyphs.empty();
    if (!cached) {
        glyphs = DrawFromFont(characters, size);
        WriteCache(key, glyphs);
    }
    glyph_font = font;
    for (Glyph& glyph : glyphs) {
        glyph.rect =
            atlas.AddCustomRectFontGlyph(font, static_cast<ImWchar>(glyph.code), glyph.width,
                                         glyph.height, glyph.advance, glyph.corner);
    }
    // The translations' wrap points: a glyph of no advance and nothing drawn (one transparent
    // pixel, as ImGui takes no empty rectangle).
    glyphs.push_back({I18n::WrapPoint, 1, 1, 0.0f, ImVec2(0.0f, 0.0f), {0}});
    glyphs.back().rect =
        atlas.AddCustomRectFontGlyph(font, static_cast<ImWchar>(I18n::WrapPoint), 1, 1, 0.0f);
    LOG_INFO(Frontend, "menus: {} glyphs {} at {} pixels in {} ms", glyphs.size(),
             cached ? "from the cache" : "from the Chinese system font", size,
             (sceKernelGetProcessTimeWide() - start) / 1000);
}

void Draw(ImFontAtlas& atlas) {
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    atlas.GetTexDataAsRGBA32(&pixels, &width, &height);
    auto* const texels = reinterpret_cast<u32*>(pixels);
    for (const Glyph& glyph : glyphs) {
        const ImFontAtlasCustomRect* r = atlas.GetCustomRectByIndex(glyph.rect);
        ASSERT_MSG(r->IsPacked(), "menus: no room in the atlas for U+{:04X}", glyph.code);
        for (int y = 0; y < glyph.height; y++) {
            u32* row = texels + (r->Y + y) * width + r->X;
            for (int x = 0; x < glyph.width; x++) {
                row[x] = IM_COL32(255, 255, 255, glyph.coverage[y * glyph.width + x]);
            }
        }
        // ImGui places a glyph from the line's top: the baseline is the font's ascent down.
        auto* const placed = const_cast<ImFontGlyph*>(glyph_font->FindGlyphNoFallback(glyph.code));
        ASSERT_MSG(placed != nullptr, "menus: U+{:04X} is not the font's", glyph.code);
        placed->Y0 += glyph_font->Ascent;
        placed->Y1 += glyph_font->Ascent;
    }
    // The pixels are the atlas's now.
    glyphs.clear();
    glyphs.shrink_to_fit();
    glyph_font = nullptr;
}

} // namespace VitaFrontend::MenuGlyphs

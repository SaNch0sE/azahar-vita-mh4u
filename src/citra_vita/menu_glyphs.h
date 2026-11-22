// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

struct ImFont;
struct ImFontAtlas;

/**
 * The menus' glyphs past Latin-1 (I18n::Characters), from the console's own Chinese font
 * (sa0:data/font/pvf/cn0.pvf, through ScePvf), as glyphs of the menus' font, every one whichever
 * language is chosen, so that a change of language needs no new atlas. Drawing them takes
 * seconds: they are kept on the memory card for the next start, drawn again when the characters
 * change.
 */
namespace VitaFrontend::MenuGlyphs {

/// Draws the characters at `size` pixels and reserves a glyph rectangle of the atlas for each, in
/// `font`: before the atlas is built.
void Add(ImFontAtlas& atlas, ImFont* font, float size);

/// Once the atlas is built (GetTexDataAsRGBA32): the glyphs' pixels into it, white with their
/// coverage as alpha as ImGui's own, and each glyph placed on the font's baseline.
void Draw(ImFontAtlas& atlas);

} // namespace VitaFrontend::MenuGlyphs

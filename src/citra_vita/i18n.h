// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"

/**
 * The menus' languages: English, the text as the code writes it, and Simplified Chinese, a table
 * of translations keyed by that text (i18n_zh_hans.cpp). Text is translated where it is shown: a
 * literal drawn at once goes through Tr, a table's literal kept for later (a setting's name and
 * choices) is marked Translatable and goes through Tr where it is drawn. tools/i18n_check.py, run
 * by the build, finds every literal in Tr, TrFormat and Translatable and stops the build where the
 * table lacks one, or where a translation's {} fields differ from its English's.
 *
 * The language is Config::Option::Language (SetLanguage); the menus show the next frame in it.
 * Any thread.
 */
namespace VitaFrontend::I18n {

/// Config::Option::Language's values, in its order.
enum class Language : int {
    English,
    SimplifiedChinese,
    Count,
};

/// The console's language as the menus have it, the setting's default: Simplified Chinese where
/// the system's is, English otherwise.
[[nodiscard]] Language ConsoleLanguage();

void SetLanguage(Language language);

/// `english` in the menus' language: itself in English, or the table's translation of it with its
/// wrap points (itself where the table has none: text that is not the menus', such as a file's
/// name).
[[nodiscard]] const char* Tr(const char* english);

/// fmt::format with `english`'s translation as the format: its {} fields filled in the same order.
template <typename... Args>
[[nodiscard]] std::string TrFormat(const char* english, Args&&... args) {
    return fmt::format(fmt::runtime(Tr(english)), std::forward<Args>(args)...);
}

/// A literal kept as English and translated where it is shown (Tr): only a mark for the check.
[[nodiscard]] constexpr const char* Translatable(const char* english) {
    return english;
}

/// Where a translation's line may break (U+3000, a blank to ImGui's wrapping, as a space is):
/// between Chinese characters, not before closing punctuation nor after opening. Tr's texts carry
/// it; the menus' font draws it as nothing, a glyph of no width (MenuGlyphs).
constexpr u16 WrapPoint = 0x3000;

/// The characters any language's text draws with past Latin-1 (the menus' font has Latin-1 alone),
/// each once, ascending, WrapPoint not among them: the glyphs MenuGlyphs adds to the menus' font,
/// so that switching the language needs no new font.
[[nodiscard]] std::vector<u16> Characters();

} // namespace VitaFrontend::I18n

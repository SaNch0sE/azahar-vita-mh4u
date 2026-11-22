// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <span>

namespace VitaFrontend::I18n {

/// One text of the menus: its English, as the code writes it, and its translation.
struct Translation {
    const char* english;
    const char* text;
};

/// The Simplified Chinese table (i18n_zh_hans.cpp).
[[nodiscard]] std::span<const Translation> SimplifiedChinese();

/// The language setting's choices, each in its own language (Language's order).
[[nodiscard]] std::span<const char* const> LanguageNames();

} // namespace VitaFrontend::I18n

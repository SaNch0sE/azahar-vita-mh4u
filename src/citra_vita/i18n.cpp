// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>

#include <psp2/apputil.h>
#include <psp2/system_param.h>

#include "citra_vita/i18n.h"
#include "citra_vita/i18n_tables.h"
#include "common/logging/log.h"

namespace VitaFrontend::I18n {

namespace {

std::atomic<Language> language{Language::English};

/// A table by its English text.
using Lookup = std::unordered_map<std::string_view, const char*>;

/// The code point at `text` (UTF-8), and `text` moved past it.
u32 NextCodePoint(const char*& text) {
    const auto byte = [&] { return static_cast<u8>(*text++); };
    const u32 first = byte();
    if (first < 0x80) {
        return first;
    }
    const int more = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : 1;
    u32 code = first & (0x3f >> more);
    for (int i = 0; i < more && (static_cast<u8>(*text) & 0xc0) == 0x80; i++) {
        code = code << 6 | (byte() & 0x3f);
    }
    return code;
}

void AppendUtf8(std::string& out, u32 code) {
    if (code < 0x80) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
        out.push_back(static_cast<char>(0xc0 | code >> 6));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xe0 | code >> 12));
        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
}

/// A Chinese character or full-width punctuation, between which a line may break.
bool Wide(u32 code) {
    return code >= 0x2e80 || code == 0x2026 || code == 0x201c || code == 0x201d;
}

/// Punctuation a line may not start with, and punctuation it may not end with.
bool Closing(u32 code) {
    return std::u32string_view{U"，。、：；！？）」』”…,.:;!?)"}.find(
               static_cast<char32_t>(code)) != std::u32string_view::npos;
}
bool Opening(u32 code) {
    return code == U'（' || code == U'「' || code == U'『' || code == U'“' || code == U'(';
}

/// `text` with WrapPoint where a line may break: ImGui breaks lines at blanks alone, and a run of
/// Chinese has none, so a sentence too long for the rest of a line went to the next line whole.
std::string WithWrapPoints(const char* text) {
    std::string out;
    u32 last = 0;
    while (*text != '\0') {
        const u32 code = NextCodePoint(text);
        if (last != 0 && (Wide(last) || Wide(code)) && last != ' ' && code != ' ' && last != '\n' &&
            code != '\n' && !Closing(code) && !Opening(last)) {
            AppendUtf8(out, WrapPoint);
        }
        AppendUtf8(out, code);
        last = code;
    }
    return out;
}

const Lookup& SimplifiedChineseLookup() {
    // The texts with their wrap points, kept where the lookup's pointers stay valid.
    static std::deque<std::string> texts;
    static const Lookup lookup = [] {
        Lookup made;
        for (const Translation& t : SimplifiedChinese()) {
            made.emplace(t.english, texts.emplace_back(WithWrapPoints(t.text)).c_str());
        }
        return made;
    }();
    return lookup;
}

} // Anonymous namespace

Language ConsoleLanguage() {
    SceAppUtilInitParam init{};
    SceAppUtilBootParam boot{};
    // Already up is as good (the call refuses a second init).
    sceAppUtilInit(&init, &boot);
    int system = 0;
    if (const int got = sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &system); got < 0) {
        LOG_ERROR(Frontend, "The system's language did not read: {:#010x}", static_cast<u32>(got));
        return Language::English;
    }
    return system == SCE_SYSTEM_PARAM_LANG_CHINESE_S ? Language::SimplifiedChinese
                                                     : Language::English;
}

void SetLanguage(Language chosen) {
    language.store(chosen, std::memory_order_relaxed);
}

const char* Tr(const char* english) {
    if (language.load(std::memory_order_relaxed) != Language::SimplifiedChinese) {
        return english;
    }
    const Lookup& lookup = SimplifiedChineseLookup();
    const auto found = lookup.find(english);
    return found != lookup.end() ? found->second : english;
}

std::vector<u16> Characters() {
    std::vector<u16> characters;
    const auto add = [&](const char* text) {
        while (*text != '\0') {
            // Past the Basic Multilingual Plane is past ImGui's 16-bit characters: the table
            // keeps to it.
            if (const u32 code = NextCodePoint(text);
                code > 0xff && code <= 0xffff && code != WrapPoint) {
                characters.push_back(static_cast<u16>(code));
            }
        }
    };
    for (const Translation& t : SimplifiedChinese()) {
        add(t.text);
    }
    // The language setting's own choices, each in its language.
    for (const char* name : LanguageNames()) {
        add(name);
    }
    std::sort(characters.begin(), characters.end());
    characters.erase(std::unique(characters.begin(), characters.end()), characters.end());
    return characters;
}

} // namespace VitaFrontend::I18n

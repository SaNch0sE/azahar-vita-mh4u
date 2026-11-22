// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel {
class Process;
}

namespace Loader {

/// A patch built in for a title, as a menu shows it: in English, which a menu translates.
struct TitlePatchInfo {
    const char* name;        ///< a few words
    const char* description; ///< what it changes, for the player
};

/// The player's language for TitlePatchesWarning: `translate` returns a text's translation, or the
/// text itself where it has none. Without one, English.
void SetTitlePatchesTranslator(const char* (*translate)(const char* english));

/// The patches built in for the title, in the order they go in; empty for a title with none.
[[nodiscard]] std::vector<TitlePatchInfo> TitlePatchesOf(u64 title_id);

/**
 * Writes the code patches built in for the process's title into its code: after the title is
 * loaded, and again after a save state's load, which restores the code the state was saved with.
 * A patch goes in only where every word it replaces is the one it was made for (another version
 * of the title keeps its code).
 * @returns for each of the title's patches (TitlePatchesOf's order), whether it is in the code:
 *     written now, or there already (a state saved with it).
 */
std::vector<bool> ApplyTitlePatches(Core::System& system, const Kernel::Process& process);

/**
 * What the player can do when the patches built in for a game do not run on the copy loaded:
 * another region's copy (its product code, `product_code`, the patched one's but for the region's
 * letter: the patches are one title ID's), or the patched title with other code (another
 * revision, `remaster_version`, or an update; `in` from ApplyTitlePatches). Names the copy they
 * are made for. Empty when they run, or when the game has none.
 */
[[nodiscard]] std::string TitlePatchesWarning(u64 title_id, std::string_view product_code,
                                              u16 remaster_version, const std::vector<bool>& in);

/// What Azahar sets an engine setting to where the player has not: the game's own, off or on.
enum class EngineDefault : u8 { Game, Off, On };

/// One of the engine settings of Capcom's MT Framework: a bit of its render config, which the
/// player can set for a title. Its bit in a
/// title is the one the title's code registers under one of `properties` (the bit an accessor that
/// registration names sets or reads), else `fixed_bit`; engines give some bits other meanings.
struct EngineSetting {
    std::array<const char*, 3> properties; ///< the engine's names for it, null past the last
    s8 fixed_bit;            ///< where the title's code names none of them; -1: not shown then
    EngineDefault azahar;    ///< every anti-aliasing off, the others the game's
    const char* name;        ///< in English, which a menu translates
    const char* description; ///< what it changes, for the player
};

constexpr std::size_t EngineSettingCount = 10;

/// The settings, in the order the Patches tab lists them.
[[nodiscard]] std::span<const EngineSetting> EngineSettings();

constexpr std::array<s8, EngineSettingCount> NoEngineBits = [] {
    std::array<s8, EngineSettingCount> bits{};
    bits.fill(-1);
    return bits;
}();

/// A title's engine settings: whether its code has the render config (known once its code was
/// searched: at its start, or by the settings of a title not started yet), the bits its code starts
/// with, each setting's bit in its engine, and the player's choices (a bit in `on` or `off`; in
/// neither, the setting's EngineDefault).
struct EngineChoices {
    bool scanned = false; ///< the title's code was searched (FindRenderConfig) once
    bool found = false;
    u32 defaults = 0;
    std::array<s8, EngineSettingCount> bits = NoEngineBits; ///< -1: the title has no such setting
    u32 on = 0;
    u32 off = 0;
};

/// Whether the setting (EngineSettings' index) is on for the title.
[[nodiscard]] bool EngineSettingOn(const EngineChoices& choices, std::size_t setting);

/// The bits the guard sets and clears for the title: the player's choices, else the settings'
/// EngineDefault where it is on or off, of its settings only.
struct EngineGuardBits {
    u32 on;
    u32 off;
};
[[nodiscard]] EngineGuardBits GuardBitsOf(const EngineChoices& choices);

/// The title's, from the engine settings file.
[[nodiscard]] EngineChoices EngineChoicesOf(u64 title_id);
/// The player's choice for `bit` of the title's settings, kept in the file.
void SetEngineChoice(u64 title_id, u32 bit, bool on);

/// The render config in a title's code: the defaults its constructor's call sites pass (literal
/// words, the high byte the constructor masks off clear: the game's own bits), and where the word
/// lives. Found by the engine's code, whatever the title: the constructor masks a stack argument's
/// high bits; each call site's literal comes from running the caller forward to the call.
struct RenderConfig {
    struct Word {
        VAddr addr;
        u32 value;
    };
    u32 constructors = 0;
    std::vector<Word> defaults;
    /// The config word's readers: each `tst` or `and` of one of its bits just after a load of it
    /// (`add rB, rT, #0x6000 or #0x6800; [add rB, rB, #imm;] ldr rV, [rB, #imm]`). The word is
    /// the one read for the most different bits.
    u32 word_offset = 0;
    std::vector<Word> readers;
    /// The global the config's readers load its object's address from (the most of them), or 0.
    VAddr global = 0;
    /// Each setting's (EngineSettings') bit in this title, -1 for none.
    std::array<s8, EngineSettingCount> bits = NoEngineBits;
};
[[nodiscard]] RenderConfig FindRenderConfig(Kernel::Process& process);
/// The same in a code image: the text at `text_addr`, the read-only data at `ro_addr`, the data
/// at `data_addr`.
[[nodiscard]] RenderConfig FindRenderConfig(u64 program_id, VAddr text_addr,
                                            std::span<const u8> text, VAddr ro_addr,
                                            std::span<const u8> ro, VAddr data_addr,
                                            std::span<const u8> data);

/// The render config words the emulator keeps at the player's bits while the title runs (the native
/// core's engine guard): the global holding the config object's address (0: none), the default
/// its constructor stores in two words in a row (the engine's request and current setting, found
/// in the object by it), the bits set and cleared. Set at each title's load.
struct EngineGuardTarget {
    VAddr global = 0;
    u32 initial = 0;
    u32 on = 0;
    u32 off = 0;
    bool operator==(const EngineGuardTarget&) const = default;
};
void SetEngineGuardTarget(const EngineGuardTarget& target);
[[nodiscard]] const EngineGuardTarget& EngineGuardTargetNow();

/// Records that the title's code has the render config, starting with `defaults`, and the settings
/// its engine has, for the Patches tab.
void NoteEngineFound(u64 title_id, u32 defaults, const std::array<s8, EngineSettingCount>& bits);
/// Records that the title's code has none: its later loads search it no more (a search reads the
/// whole code, ~30 ns a byte through the guest memory's mapping).
void NoteEngineAbsent(u64 title_id);

} // namespace Loader

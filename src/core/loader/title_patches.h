// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
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

/// What a title's built-in patches ask of the renderer, as bits: work done another way for that
/// title alone, with the same picture, where it is measured to be faster. Every other title is
/// drawn as before. Set by ApplyTitlePatches for the title loaded.
enum TitleTuningBit : u32 {
    /// Geometry shader draws: an invocation's room is its program's bound past the usual share
    /// (RasterizerSgxd::GeometryStage), and merged runs stop where their vertices lie far apart
    /// (RenderThread::DrawMerged).
    TuningGeometryBatches = 1u << 0,
    /// A screen the frontend's layout does not show (RenderThread::hidden_screen) is not drawn:
    /// its draws are left out as an unseen right eye's are.
    TuningHiddenScreen = 1u << 1,
    /// The HUD scale option applies: the passes that draw the top screen's 2D layer over its
    /// finished 3D picture are drawn at that scale, the 3D picture at the 3DS's resolution
    /// (RasterizerSgxd::SetHudScale). A sharper picture, not the same one.
    TuningHudScale = 1u << 2,
    /// The Even frame pacing option applies (RenderThread::even_pacing): experimental, offered
    /// where it has been tried.
    TuningEvenPacing = 1u << 3,
};
inline std::atomic<u32> title_tuning{0};
/// The tuning bits a title's built-in patches ask for (a menu's: the title need not be running).
[[nodiscard]] u32 TitleTuningOf(u64 title_id);

/// A frontend setting a tuned title starts from: its key and value as the settings file writes
/// them (the Vita's config.cpp), set when the title gets settings of its own, and the player's to
/// change after.
struct TitleSettingDefault {
    const char* key;
    const char* value;
};
/// The settings `title_id` starts from (none for most titles).
[[nodiscard]] std::span<const TitleSettingDefault> TitleSettingDefaultsOf(u64 title_id);
[[nodiscard]] inline bool TitleTuned(u32 bit) {
    return (title_tuning.load(std::memory_order_relaxed) & bit) != 0;
}

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

/// An engine setting that keeps a field of one of the engine's systems at a value the player
/// chose: the field the title's code registers under `property` (the frame rate cap, the game's
/// speed), in the object its class's constructor stores in a global (or the render config's, for
/// a field registered with the config's bits).
struct EngineValue {
    const char* property;
    u8 type; ///< its MT Framework property type: 6 a u32, 12 an f32
    /// The choices' words (a float's bits); the first is the game's own, which the guard leaves.
    std::array<u32, 4> choices;
    std::array<const char*, 4> labels; ///< in English, which a menu translates; null past the last
    const char* name;
    const char* description;
};

constexpr std::size_t EngineValueCount = 3;

[[nodiscard]] std::span<const EngineValue> EngineValues();

/// An effect the player can leave out: an engine filter class, and every class derived from it,
/// whose render returns at once.
struct EngineFilter {
    const char* class_name;
    const char* name;
    const char* description;
};

constexpr std::size_t EngineFilterCount = 9;

[[nodiscard]] std::span<const EngineFilter> EngineFilters();

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
    u32 values = 0;                           ///< the EngineValues the title has, a bit each
    u32 filters = 0;                          ///< the EngineFilters the title has, a bit each
    std::array<u8, EngineValueCount> value{}; ///< the player's choice of each (0: the game's own)
    u32 filters_off = 0;                      ///< the filters the player left out
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
/// The player's choice of the title's value setting (EngineValues' index), kept in the file.
void SetEngineValue(u64 title_id, std::size_t value, u8 choice);
/// Whether the player left out the title's filter (EngineFilters' index), kept in the file.
void SetEngineFilterOff(u64 title_id, std::size_t filter, bool off);

/// The render config in a title's code: the defaults its constructor's call sites pass (literal
/// words, the high byte the constructor masks off clear: the game's own bits), and where the word
/// lives. Found by the engine's code, whatever the title: the constructor masks a stack argument's
/// high bits; each call site's literal comes from running the caller forward to the call.
struct RenderConfig {
    struct Word {
        VAddr addr;
        u32 value;
        bool operator==(const Word&) const = default;
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
    /// Each value setting's field: the globals its object's constructors store it in (none: the
    /// title has no such field) and its offset in the object.
    struct Field {
        std::vector<VAddr> globals;
        u32 offset = 0;
    };
    std::array<Field, EngineValueCount> fields{};
    /// Each filter's render functions (its class's and the classes' derived from it), their first
    /// words.
    std::array<std::vector<Word>, EngineFilterCount> filters{};
};
[[nodiscard]] RenderConfig FindRenderConfig(Kernel::Process& process);
/// The same in a code image: the text at `text_addr`, the read-only data at `ro_addr`, the data
/// at `data_addr`.
[[nodiscard]] RenderConfig FindRenderConfig(u64 program_id, VAddr text_addr,
                                            std::span<const u8> text, VAddr ro_addr,
                                            std::span<const u8> ro, VAddr data_addr,
                                            std::span<const u8> data);

/// What the emulator keeps while the title runs (the native core's engine guard), set at each
/// title's load: the render config words at the player's bits (the global holding the config
/// object's address, 0: none; the default its constructor stores in two words in a row, the
/// engine's request and current setting, found in the object by it; the bits set and cleared),
/// the fields at the player's values (in each object a global holds) and the functions that
/// return at once (the filters left out).
struct EngineGuardTarget {
    VAddr global = 0;
    u32 initial = 0;
    u32 on = 0;
    u32 off = 0;
    struct Value {
        std::vector<VAddr> globals;
        u32 offset = 0;
        u32 value = 0;
        bool operator==(const Value&) const = default;
    };
    std::vector<Value> values;
    std::vector<RenderConfig::Word> returns;
    bool operator==(const EngineGuardTarget&) const = default;
};
void SetEngineGuardTarget(const EngineGuardTarget& target);
[[nodiscard]] const EngineGuardTarget& EngineGuardTargetNow();
/// The target for a title's render config and the player's choices.
[[nodiscard]] EngineGuardTarget EngineGuardTargetOf(const RenderConfig& config,
                                                    const EngineChoices& choices);

/// Records that the title's code has the render config and the settings its engine has, for the
/// Patches tab.
void NoteEngineFound(u64 title_id, const RenderConfig& config);
/// Records that the title's code has none: its later loads search it no more (a search reads the
/// whole code, ~30 ns a byte through the guest memory's mapping).
void NoteEngineAbsent(u64 title_id);

} // namespace Loader

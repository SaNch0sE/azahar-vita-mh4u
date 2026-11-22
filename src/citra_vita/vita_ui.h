// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "citra_vita/config.h"
#include "citra_vita/multiplayer.h"
#include "common/common_types.h"
#include "common/math_util.h"
#include "common/named_thread.h"
#include "core/loader/title_patches.h"
#include "core/savestate.h"

#include <imgui_vita2d/imgui.h>

namespace Core {
class System;
}

namespace VitaFrontend {

class EmuWindowVita;

/**
 * The on-screen interface: a game list before anything is running and a pause menu over a
 * running title, drawn with Dear ImGui through sgxd (imgui_impl_sgxd). One menu owns the loop
 * while it is open; the pad is the primary input, with touch alongside.
 *
 * The settings screen is shared between the game list and the pause menu, and edits either the
 * values for all games or one title's own (Config): the same rows, the title's showing which of
 * them it sets itself. A row is one line, its label and its value; the selected row's description
 * and state (why it is greyed out, whether a title sets its own) show in full beside the list,
 * so that nothing has to be cut to fit. A title with patches built in (Loader::TitlePatchesOf)
 * has a tab more that lists them, which nothing on the screen changes.
 *
 * While a title runs the interface is the sgxd render thread's (SgxdRenderer::RenderThread), as
 * the window is: it builds and draws the stats overlay in each present, and runs the pause menu
 * the PS button opens (EmuWindowVita::PollEvents). The run loop reads only SelectedSlot, after
 * the menu has closed. Before and after a title, every method runs on the emulation thread,
 * which then presents.
 */
class Ui {
public:
    /// The game list's banner, in display pixels: the 3DS top screen's 400x240 at 1.15x, the
    /// panel's height, in its middle (BannerPlayer shows it there; DrawGameList leaves a hole for
    /// it).
    [[nodiscard]] static Common::Rectangle<u32> BannerRect();
    [[nodiscard]] static u32 BannerBackdrop();
    /// The title whose banner the panel shows: the selected one while the game list shows with
    /// a title in focus (not the tools in the footer); empty otherwise.
    [[nodiscard]] std::string BannerTitle() const;
    /// Selects the first title whose name or path holds `text` (any case), as the pad would:
    /// the command channel's `select`. False when no title does.
    bool SelectTitle(const std::string& text);

    /// What the UI is asking the frontend to do, returned from Update.
    enum class Action {
        None,
        LaunchGame,
        ResumeGame,
        QuitGame,
        ExitApplication,
        SaveState,
        LoadState,
        TogglePin,
    };

    explicit Ui(Core::System& system, EmuWindowVita& window);
    ~Ui();

    /// Builds this frame's interface and returns what the user asked for. Call once per frame,
    /// before Draw.
    Action Update();

    /// Issues the built frame's draw commands. Must be called inside an SgxdPresent frame.
    /// In game the frame is the stats overlay (StatsHud), drawn with no ImGui frame.
    void Draw();

    /// The game the user chose, valid after Action::LaunchGame.
    [[nodiscard]] const std::string& SelectedGamePath() const {
        return selected_game_path;
    }

    std::string TogglePin();

    /// The savestate slot the user chose, valid after Action::SaveState / Action::LoadState.
    [[nodiscard]] u32 SelectedSlot() const {
        return selected_slot;
    }

    /// The title a launch has loaded, for the pause menu's game settings: its settings' key
    /// (Config::RunningTitle) and its file's name, as the game list shows it. Before EnterGame.
    void SetGame(const std::string& path);
    /// Switches to the in-game state, where nothing is drawn over the title.
    void EnterGame();
    /// Back to the game list, where the pause menu's "Quit game" leaves the interface: after a
    /// quit that did not come through the menu (the command channel).
    void LeaveGame();

    /// Opens the pause menu over a running title.
    void OpenPauseMenu();

    /// Shows a full-screen message instead of a menu, for the stretches where the frontend is
    /// busy and the display would otherwise sit on a stale frame.
    void SetStatusMessage(std::string message);
    void SetProgress(float fraction);
    void ClearStatusMessage();

private:
    enum class State {
        GameList,
        Settings,
        InGame,
        Pause,
        Savestates,
        Multiplayer,
    };

    /// The settings screen's groups, its tabs. Patches holds no rows: a title's built-in patches.
    enum class Category {
        Display,
        Speed,
        Sound,
        System,
        Patches,
        Count,
    };

    /// One row of the settings screen: an option of Config's, and how it is shown and changed.
    /// The values are Config's: a toggle's 0 or 1, a choice's index, a slider's number.
    struct Row {
        enum class Kind { Toggle, Choice, Slider };

        Config::Option option;
        Category category;
        Kind kind;
        const char* title;
        const char* description;
        std::vector<const char*> choices;
        /// A slider's step for each press of the pad, its unit, and what its 0 shows instead of
        /// a number (null: the number).
        int step;
        const char* unit;
        const char* zero;
    };

    struct GameEntry {
        std::string path;
        std::string name; ///< the title's own short name (its SMDH's), else the file's
        std::string file; ///< the file's name under the title's; empty when `name` is it
        std::string publisher; ///< its SMDH's, or empty
        bool has_icon{};       ///< its SMDH's large icon is in the icon cache
    };
    /// The titles' names read from their SMDH, by path, with the file's size they were read at: a
    /// scan reads only files new or changed since.
    struct CachedTitle {
        u64 size;
        std::string name;
        std::string publisher;
        bool has_icon{};
        /// From a cache of before publishers and icons were kept: read the file again.
        bool stale{};
    };
    /// A slot of the font atlas a title's 48x48 icon is written into (SgxdPresent::UpdateTexture),
    /// for as long as the game list shows it. `used`: the frame that last drew it.
    struct IconSlot {
        std::string path;
        u64 used{};
        int x{}, y{};
        ImVec2 uv0{}, uv1{};
    };
    std::vector<IconSlot> icon_slots;
    std::vector<int> icon_rects; ///< the atlas's custom rects, reserved before it was built
    std::unordered_set<std::string> icon_missing; ///< titles whose cached icon would not read
    u64 frame{}; ///< ImGui frames built, for the slots' ages
    /// The slot holding the title's icon, written now if no slot does; null when the title has
    /// no icon or every slot was drawn in the last few frames.
    const IconSlot* IconFor(const GameEntry& game);
    std::unordered_map<std::string, CachedTitle> title_cache;
    bool title_cache_loaded = false;
    void LoadTitleCache();
    void SaveTitleCache() const;
    std::vector<Core::SaveStateInfo> slot_states;

    void BuildRows();
    void ScanGames();

    /// Opens the settings screen on the values for all games (an empty `title`) or the title's
    /// own (`name`: what the screen calls it; `program_id`: its patches'). Back returns to `back`.
    void OpenSettings(std::string title, std::string name, State back, u64 program_id = 0);

    Action DrawGameList();
    Action DrawSettings();
    Action DrawPauseMenu();
    Action DrawSavestates();
    Action DrawMultiplayer();
    /// The on-screen keyboard over the Multiplayer screen, editing `edit_text`. Not the system's
    /// keyboard dialog: it draws through GXM, and handing the GPU back to Sony's driver around
    /// it while sgxd holds a title's state is too risky.
    void DrawKeyboard();
    void DrawStatusMessage();

    /// The tabs the settings screen shows: all of them but Patches for all games, those with
    /// options a title can set itself for a title, and Patches when it has some.
    [[nodiscard]] std::vector<Category> SettingsCategories() const;
    void DrawRow(const Row& row, std::size_t index);
    void DrawRowInfo();
    void StepRow(const Row& row, int delta);
    /// The value the screen shows and edits: the title's own (or all games' where it has none),
    /// or all games'.
    [[nodiscard]] int RowValue(const Row& row) const;
    void SetRowValue(const Row& row, int value);
    /// Why the row cannot be changed on this screen now, or null when it can.
    [[nodiscard]] const char* DisabledReason(const Row& row) const;
    [[nodiscard]] std::string ValueText(const Row& row, int value) const;
    void DrawPatchRow(std::size_t index);
    void DrawPatchInfo();

    /// True when the user pressed cancel and this overlay was the thing focused, which is what
    /// separates "close the menu" from "leave the child the cancel already left" - ImGui has
    /// consumed the press by the time this runs.
    [[nodiscard]] bool BackPressed() const;

    Core::System& system;
    EmuWindowVita& window;

    State state{State::GameList};
    std::string selected_game_path;
    u32 selected_slot{};
    std::string status_message;
    float status_progress = -1.0f; ///< SetProgress's; below 0, no bar

    std::vector<GameEntry> games;
    int selected_game{};
    /// The game grid's scroll in pixels, eased towards the selected tile's column; whether a
    /// touch has dragged it away from the selection (it stays where the finger left it until the
    /// selection moves); a tile the grid's arrows select at the next frame (-1: none); and the
    /// selection when the current touch began (a tap on the selected tile opens it).
    float carousel_scroll{};
    bool carousel_free{};
    int carousel_focus = -1;
    int tap_selected = -1;
    /// The scroll the tiles were drawn at last frame, where ImGui measures the pad's step from.
    float carousel_drawn{};
    /// The pad was on the tools in the footer last frame: coming back up, it returns to the
    /// selected tile rather than the one nearest the tool.
    bool tools_focused{};
    /// Whether the selected game has a bubble (LiveArea::IsPinned), for the footer: asked again
    /// when the selection changes.
    std::string pin_checked_path;
    bool pin_checked{};
    std::string game_key;
    std::string game_name;

    std::vector<Row> rows;
    /// The settings screen: the title it edits (empty: all games), its name, the screen Back
    /// returns to, the tab, the selected row (an index into rows, -1: none yet), and whether
    /// the tab's first row is to be selected (a new tab, the screen just opened).
    std::string settings_title;
    std::string settings_name;
    State settings_back{State::GameList};
    Category category{Category::Display};
    int focused_row = -1;
    bool focus_first_row{};
    /// The patches built in for the title the screen edits, whether each is in its code when it
    /// is the running title (empty when it is not), and the selected one (-1: none yet).
    std::vector<Loader::TitlePatchInfo> settings_patches;
    std::vector<bool> settings_patches_in;
    int focused_patch = -1;

    /// The Multiplayer screen's fields, and the one the keyboard edits (-1: none).
    Multiplayer::Fields room_fields{};
    int editing_field = -1;
    std::string edit_text;
    bool keyboard_shift{};
    bool overlay_focused{};
    /// Whether the last Update built an ImGui frame (a menu or the status message), which Draw
    /// then draws; in game it builds none and Draw draws the stats overlay.
    bool imgui_frame{};
};

} // namespace VitaFrontend

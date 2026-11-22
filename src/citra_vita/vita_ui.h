// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "citra_vita/multiplayer.h"
#include "common/common_types.h"

namespace Core {
class System;
}

namespace VitaFrontend {

class EmuWindowVita;

/**
 * The on-screen interface: a game list before anything is running and a pause menu over a
 * running title, drawn with Dear ImGui through sgxd (imgui_impl_sgxd).
 *
 * Structure follows DSVita's, which is the working reference for this shape of interface on this
 * console: one menu that owns the loop while it is open, a settings body shared between the
 * pre-launch and in-game paths with the in-game one showing only what can be changed while a
 * title runs, and gamepad navigation as the primary input with touch alongside it.
 *
 * While a title runs the interface is the sgxd render thread's (SgxdRenderer::RenderThread), as
 * the window is: it builds and draws the stats overlay in each present, and runs the pause menu
 * the PS button opens (EmuWindowVita::PollEvents). The run loop reads only SelectedSlot, after
 * the menu has closed. Before and after a title, every method runs on the emulation thread,
 * which then presents.
 */
class Ui {
public:
    /// What the UI is asking the frontend to do, returned from Update.
    enum class Action {
        None,
        LaunchGame,
        ResumeGame,
        QuitGame,
        ExitApplication,
        SaveState,
        LoadState,
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

    /// The savestate slot the user chose, valid after Action::SaveState / Action::LoadState.
    [[nodiscard]] u32 SelectedSlot() const {
        return selected_slot;
    }

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
    void ClearStatusMessage();

private:
    enum class State {
        GameList,
        GameSettings,
        InGame,
        Pause,
        PauseSettings,
        Savestates,
        Multiplayer,
    };

    /// One row of the settings body. The value is read and written through closures so a row can
    /// name any setting without this file knowing the shape of the settings structure.
    struct Setting {
        enum class Kind { Toggle, Choice, Slider };

        const char* title;
        const char* description;
        Kind kind;
        /// Whether the setting can be changed while a title is running. The pause menu shows
        /// only these; the rest would need a reboot to take effect and pretending otherwise
        /// is how a menu comes to lie about what the emulator is doing.
        bool runtime;
        std::function<bool()> get_bool;
        std::function<void(bool)> set_bool;
        std::function<int()> get_choice;
        std::function<void(int)> set_choice;
        std::vector<const char*> choices;
        std::function<int()> get_slider;
        std::function<void(int)> set_slider;
        int slider_min;
        int slider_max;
    };

    struct GameEntry {
        std::string path;
        std::string name;
    };

    void BuildSettings();
    void ScanGames();

    Action DrawGameList();
    Action DrawGameSettings();
    Action DrawPauseMenu();
    Action DrawPauseSettings();
    Action DrawSavestates();
    Action DrawMultiplayer();
    /// The on-screen keyboard over the Multiplayer screen, editing `edit_text`. Not the system's
    /// keyboard dialog: it draws through GXM, and handing the GPU back to Sony's driver around
    /// it while sgxd holds a title's state is too risky.
    void DrawKeyboard();
    void DrawStatusMessage();

    /// Draws the settings body, showing only runtime-changeable rows when asked.
    void DrawSettingsBody(bool runtime_only);
    void DrawSetting(Setting& setting, std::size_t index);
    void DrawPatchRow(std::size_t index);
    void DrawPatchInfo();

    /// True when the user pressed cancel and this overlay was the thing focused, which is what
    /// separates "close the menu" from "leave the combo box the cancel already closed" - ImGui
    /// has consumed the press by the time this runs.
    [[nodiscard]] bool BackPressed() const;

    Core::System& system;
    EmuWindowVita& window;

    State state{State::GameList};
    std::string selected_game_path;
    u32 selected_slot{};
    std::string status_message;

    std::vector<GameEntry> games;
    int selected_game{};
    std::vector<Setting> settings;
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

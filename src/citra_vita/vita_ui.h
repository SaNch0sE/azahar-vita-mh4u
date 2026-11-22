// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "citra_vita/adhoc.h"
#include "citra_vita/config.h"
#include "citra_vita/multiplayer.h"
#include "citra_vita/rom_check.h"
#include "citra_vita/control_layouts.h"
#include "citra_vita/screen_layout.h"
#include "common/common_types.h"
#include "common/math_util.h"
#include "common/named_thread.h"
#include "core/loader/title_patches.h"
#include "core/savestate.h"

#include <imgui_vita2d/imgui.h>

namespace Cheats {
class CheatBase;
}

namespace Core {
class System;
}

namespace VitaFrontend {

class EmuWindowVita;

/**
 * A full-screen notice in the menus' look, drawn through GXM (imgui_impl_gxm) until cross or its
 * footer's button is pressed: before sgxd takes the GPU (the start-up notice, an error that ends
 * the start), or when it could not take it. GXM comes up for it and goes down after, and the
 * notice has an ImGui context of its own for the while. `hint`, when there is one, is a dimmer
 * line under the text.
 */
void ShowGxmNotice(const char* title, const std::string& text, const std::string& hint,
                   const char* button);

/// Whether SceAppMgr has the system's UI over this app (isSystemUiOverlaid): its impose state,
/// which the quick menu (a long press of the PS button) sets, or the display's power save.
bool SystemUiOverlaid();

/// Whether the shell's display plane shows a picture (azaharShellPlaneShown). It stays up a little
/// past SystemUiOverlaid, and the GPU is not taken until it is gone. Reads the display's registers,
/// so only with the display on, and where the system's UI has been seen: not at every start.
bool ShellPlaneShown();

/**
 * The on-screen interface: a game list before anything is running and a pause menu over a
 * running title, drawn with Dear ImGui through sgxd (imgui_impl_sgxd). One menu owns the loop
 * while it is open; the pad is the primary input, with touch alongside.
 *
 * The settings screen is shared between the game list and the pause menu, and edits either the
 * values for all games or one title's own (Config).
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
        InstallTitle,
        CheckFile,
        ClearData,
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

    /// Installs the CIA the Install CIA screen chose, else the selected one, onto the emulated SD
    /// card (Service::AM::InstallCIA), its progress shown through `present` (which draws the
    /// interface), then lists the title it installed and selects it: what happened, for the
    /// status message. After Action::InstallTitle.
    std::string InstallSelected(const std::function<void()>& present);

    /// Installs the CCI a game's Installed tab asked for as a CIA of it would be installed
    /// (Service::AM::InstallCCI), its progress shown through `present`, then reads the tab
    /// again: what happened, for the status message. Through InstallSelected.
    std::string InstallRom(const std::function<void()>& present);

    /// Checks the file a game's File tab chose, else the selected title's, against the checksums
    /// its format carries (RomCheck), its progress shown through `present`: what it found, for a
    /// notice. After Action::CheckFile, or the command channel's `check`.
    std::string CheckSelected(const std::function<void()>& present);

    /// Whether to launch `path` now: its quick check (RomCheck::QuickCheck, about a second, its
    /// status shown through `present`) and a full check remembered for the same file found
    /// nothing wrong, or the player answered the question a damaged file raises with Launch
    /// anyway. Otherwise the question is up (Launch anyway, Full check, Cancel) and the list
    /// shows it. Before a launch from the game list or a bubble.
    bool LaunchChecked(const std::string& path, const std::function<void()>& present);

    /// Deletes the part of a game's data its Data tab's question was agreed to, its progress shown
    /// through `present`, then reads the tab again: what happened, for a notice. After
    /// Action::ClearData; no title running.
    std::string ClearData(const std::function<void()>& present);

    /// Lists the titles again (at the start, after an install, the command channel's `rescan`),
    /// keeping the selected one selected.
    void ScanGames();

    /// The title of the option's row in the settings, in the menus' language: a crash's notice
    /// names the settings it asks the player to set back (Config::WriteChangedFromDefaults).
    [[nodiscard]] std::string SettingTitle(Config::Option option) const;
    [[nodiscard]] std::string SettingDefault(Config::Option option) const;

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
        InstallPicker,
        LayoutEditor,
        ControlEditor,
    };

    /// The settings screen's groups, its tabs. Patches, Cheats, Installed, File and Data hold no
    /// rows: a title's built-in patches, its cheat file's cheats, what of it is installed, its
    /// file's check, and its save data and shader cache.
    enum class Category {
        Display,
        Speed,
        Sound,
        Controls,
        System,
        Patches,
        Cheats,
        Installed,
        File,
        Data,
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
        bool installer{};      ///< a CIA, which the list installs instead of opening
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

    /// Opens the settings screen on the values for all games (an empty `title`) or the title's
    /// own (`name`: what the screen calls it; `program_id`: its patches'). Back returns to `back`.
    /// `path`: the title's file, from the game list (its File tab), else empty.
    void OpenSettings(std::string title, std::string name, State back, u64 program_id = 0,
                      std::string path = {});

    Action DrawGameList();
    Action DrawSettings();
    Action DrawPauseMenu();
    Action DrawSavestates();
    Action DrawMultiplayer();
    /// The Install CIA screen: the memory devices, then a folder's folders and CIA files; a
    /// file's cross installs it (picked_cia, Action::InstallTitle). With picker_folders, the Add
    /// ROM folder screen: folders and the titles in them, square adding the folder shown.
    Action DrawInstallPicker();
    void OpenPickerDir(std::string dir);
    void OpenRomFolderPicker();
    /// Leaves the picker: for the game list, or for the settings when it chose a ROM folder
    /// (Install CIA's folder in picker_dir again).
    void ClosePicker();
    struct PickerEntry {
        std::string name;
        std::string path;
        u64 size{};
        bool folder{};
    };
    std::string picker_dir; ///< the folder shown, kept for the next time the screen opens
    /// The picker chooses a ROM folder for the settings (square takes the folder shown) instead of
    /// a CIA; Install CIA's folder waits in picker_cia_dir meanwhile.
    bool picker_folders{};
    std::string picker_cia_dir;
    std::vector<PickerEntry> picker_entries;
    std::string picked_cia;
    /// The on-screen keyboard over the Multiplayer screen and the Settings screen's console name,
    /// editing `edit_text`. Not the system's
    /// keyboard dialog: it draws through GXM, and handing the GPU back to Sony's driver around
    /// it while sgxd holds a title's state is too risky. Square deletes; back asks first where
    /// the text changed (Question::DiscardText).
    void DrawKeyboard();
    void OpenKeyboard(int field, std::string text);
    void CloseKeyboard();
    void DrawStatusMessage();
    /// A question over the screen before something a press could do by accident, or that the
    /// player has to agree to; Cancel has the focus first, and circle cancels too. Pin: the
    /// selected game's bubble added to the LiveArea or taken off it (Action::TogglePin).
    /// UsbSetting, UsbLaunch: USB video taking the port from another plugin
    /// (UvcStream::PortTaken), asked when USB video is turned on or when a game starts with it on;
    /// Take over lets the camera have the port (that plugin stops working until a reboot); at a
    /// game's start Play without turns USB video off and starts it, Cancel starts nothing.
    Action DrawQuestion();
    /// Damaged: the file about to launch failed its check (LaunchChecked): Launch anyway, Full
    /// check, Cancel. DiscardLayout: the layout editor left with changes not saved. DeleteLayout:
    /// the custom layout the editor opened on deleted. ClearData: the Data tab's part deleted.
    /// QuitGame, ExitApplication: the pause menu's, which end the running game. SaveOver,
    /// LoadSlot: the Save states screen's save over a used slot (selected_slot) and load of one.
    /// DiscardText: the keyboard left with its text changed.
    enum class Question {
        None,
        Pin,
        QuitGame,
        ExitApplication,
        SaveOver,
        LoadSlot,
        DiscardText,
        UsbSetting,
        UsbLaunch,
        Damaged,
        DiscardLayout,
        DeleteLayout,
        DiscardControls,
        DeleteControls,
        ClearData,
    };
    Question question{Question::None};
    std::string question_game;  ///< Pin: the game's name
    bool question_pinned{};     ///< Pin: its bubble is there now (the question takes it off)
    int usb_prompt_value{};     ///< UsbSetting: the screen the setting was turned to

    /// The tabs the settings screen shows: all of them but Patches and Cheats for all games,
    /// those with options a title can set itself for a title, Patches when it has some, and
    /// Cheats for a title with a program ID.
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
    /// One of the Patches tab's engine settings (an MT Framework title's render config bits): its
    /// switch, which cross, a touch or the pad's left and right flips; kept at once, applied at the
    /// title's next start.
    void DrawEngineRow(std::size_t index);
    void DrawEngineInfo();
    /// The Patches tab's first opening for a title never started: its code searched for engine
    /// settings on a worker (AppLoader::ScanEngine), under a panel, once.
    void StartEngineSearch();
    void DrawEngineSearch();
    void DrawCheatRow(std::size_t index);
    void DrawCheatInfo();
    void DrawInstalledRow(std::size_t index);
    void DrawInstalledInfo();
    void ScanInstalled();

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
    /// The title's engine settings (found: its code has the render config, known once it ran), and
    /// the selected one (-1: none, or a patch).
    Loader::EngineChoices settings_engine;
    int focused_engine = -1;
    /// What the description panel shows (the tab and each list's selection): a change scrolls it
    /// back to its top.
    std::array<int, 8> info_shown{};
    /// StartEngineSearch's: whether this screen asked for the search, its end, and its worker
    /// (last: joined before what it writes goes).
    bool engine_search_asked = false;
    std::atomic<bool> engine_search_done{false};
    Common::NamedThread engine_search;
    /// The title's program ID (0: none, or all games), its cheats as the cheat engine holds them
    /// (its file's, read when the screen opens), and the selected one (-1: none yet).
    u64 settings_program_id{};
    std::vector<std::shared_ptr<Cheats::CheatBase>> settings_cheats;
    int focused_cheat = -1;
    /// What of the title is installed on the emulated SD card (its game, update, DLC), read when
    /// the screen opens from the game list (ScanInstalled), after the row that installs the
    /// title's CCI when its game is not installed; the selected part, and the one a first cross
    /// armed for removal (-1: none).
    struct InstalledPart {
        const char* kind;
        u64 title_id;
        u64 size;
        u16 version;
        bool install;
        /// The install row's CCI is NCCH-encrypted (the install refuses it).
        bool encrypted;
    };
    std::vector<InstalledPart> settings_installed;
    std::optional<u16> settings_rom_version;
    std::string install_rom_path;
    std::string install_rom_name;
    bool install_requested{};
    int focused_installed = -1;
    int armed_installed = -1;
    /// The File tab: the title's file (OpenSettings' path), the check it asked for (CheckSelected
    /// takes it), and each file's last check's outcome, by path.
    std::string settings_path;
    std::string check_path;
    std::string check_name;
    bool check_requested{};
    std::unordered_map<std::string, std::string> check_results;
    /// The damaged file's question's text, and its answer Launch anyway (LaunchChecked's next
    /// call launches).
    std::string damaged_text;
    bool launch_anyway{};
    /// A full check's outcome, kept on the memory card for the launches after (CheckRecordFile):
    /// the file's size then, the outcome, the part (English).
    struct CheckRecord {
        u64 size;
        RomCheck::Outcome outcome;
        std::string part;
    };
    std::unordered_map<std::string, CheckRecord> check_records;
    bool check_records_loaded{};
    const CheckRecord* RememberedCheck(const std::string& path);
    void RememberCheck(const std::string& path, const RomCheck::Result& result);
    /// The System tab's last row, all games' only: the 3DS's user name (Config::ConsoleName), which
    /// cross edits on the keyboard. Its info beside the list while it has the focus (focused_row
    /// ConsoleNameRow).
    void DrawConsoleNameRow();
    void DrawConsoleNameInfo();
    static constexpr int ConsoleNameRow = -2;
    /// The System tab's rows after it, all games' only: each ROM folder the player added
    /// (Config::RomFolders), which cross pressed twice takes off, then the row whose cross opens
    /// the folder picker to add one (picker_folders). Their info beside the list (focused_row
    /// RomFolderRow with focused_rom_folder, or AddRomFolderRow).
    void DrawRomFolderRows();
    void DrawRomFolderInfo();
    static constexpr int RomFolderRow = -5;
    static constexpr int AddRomFolderRow = -6;
    int focused_rom_folder = -1;
    int armed_rom_folder = -1;
    void DrawFileRow();
    /// The Data tab, a title's with a program ID: its save data (the save archive and the title's
    /// extdata) and its shader cache, each with the folders deleting it removes and their size,
    /// read when the tab is first shown (ScanData). Cross asks (Question::ClearData), from the
    /// game list only: the running title has its files open.
    enum class DataKind { SaveData, ShaderCache };
    struct DataPart {
        DataKind kind;
        const char* name;
        std::vector<std::string> folders;
        u64 bytes{};
        u32 files{};
    };
    std::vector<DataPart> settings_data;
    bool data_scanned{};
    int focused_data = -1;
    int clear_data = -1; ///< the part the question asks about, for ClearData
    void ScanData();
    void DrawDataRow(std::size_t index);
    void DrawDataInfo();
    /// The Display tab's last row: the layout editor, opened on the screen layout the tab shows.
    /// Its info beside the list while it has the focus (focused_row LayoutEditorRow).
    void DrawLayoutEditorRow();
    void DrawLayoutEditorInfo();
    static constexpr int LayoutEditorRow = -3;

    /**
     * The layout editor: the whole display is its preview, each screen where the layout being made
     * places it (EmuWindowVita::PreviewLayout): a running game's own screens, or in the game list a
     * card for each. The pad and touch move, size and turn the selected screen; a panel of tools
     * holds the rest of its options, and the save panel the layout's name. Opened from the settings
     * screen, which it goes back to; saving makes the layout the one those settings choose.
     */
    Action DrawLayoutEditor();
    void OpenLayoutEditor();
    void CloseLayoutEditor();
    void EditLayout();
    void DrawLayoutPreview();
    void DrawLayoutBars();
    void DrawLayoutTools();
    void DrawLayoutSave();
    /// Saves the layout under the save panel's name: over the custom layout opened, or as a new
    /// one; the settings edited choose it, and the editor closes.
    void SaveEditedLayout(bool as_new);
    enum class EditorPanel { None, Tools, Save };
    struct LayoutEditor {
        ScreenLayouts::ScreenLayout layout;
        std::optional<std::size_t> index;
        std::string name;
        ScreenLayouts::Screen selected{ScreenLayouts::Screen::Top};
        EditorPanel panel{EditorPanel::None};
        bool changed{};
        bool dirty{};
        /// The pad's buttons last frame, and how long each repeating one (the d-pad's four, L, R)
        /// has been held.
        u32 pad{};
        std::array<float, 6> held_for{};
        /// The fingers on the front panel last frame (two at most), whether that touch is the
        /// preview's (it began on neither bar nor under a panel), and whether it drags a screen.
        int touches{};
        std::array<ScreenLayouts::Point, 2> touch{};
        bool preview_touch{};
        bool dragging{};
        /// The header and the footer go while an edit runs on (a drag, a stick held) and come back
        /// once it rests: how long it has run and rested.
        float busy{};
        float idle{};
        bool bars_hidden{};
    };
    LayoutEditor editor;

    /// The Controls tab's row under Control layout: the control layout editor, opened on the
    /// control layout the tab shows; its info beside the list (focused_row ControlEditorRow).
    void DrawControlEditorRow();
    void DrawControlEditorInfo();
    static constexpr int ControlEditorRow = -4;
    /**
     * The control layout editor: the whole display its preview, the layout's on-screen buttons
     * where it places them over a running game's screens (in the game list, over cards where the
     * screen layout puts the screens). The pad and touch move and size the selected on-screen
     * button; the Buttons panel holds each 3DS button's Vita input and whether it is on screen,
     * and the save panel the layout's name. Opened from the settings screen, which it goes back
     * to; saving makes the layout the one those settings choose.
     */
    Action DrawControlEditor();
    void OpenControlEditor();
    void CloseControlEditor();
    void EditControls();
    void DrawControlPreview();
    void DrawControlBars();
    void DrawControlButtons();
    void DrawControlSave();
    void SaveEditedControls(bool as_new);
    struct ControlEditor {
        ControlLayouts::ControlLayout layout;
        std::optional<std::size_t> index;
        std::string name;
        std::optional<ControlLayouts::Button> selected;
        EditorPanel panel{EditorPanel::None};
        bool changed{};
        u32 pad{};
        std::array<float, 6> held_for{};
        int touches{};
        std::array<ScreenLayouts::Point, 2> touch{};
        bool preview_touch{};
        bool dragging{};
        ScreenLayouts::ScreenLayout screens;
    };
    ControlEditor control_editor;
    ScreenLayouts::ScreenLayout SettingsScreenLayout() const;
    void DrawFileInfo();

    /// The Multiplayer screen's fields, and the one the keyboard edits (-1: none; 0-3 the room's
    /// address, port, nickname and password, KeyboardConsoleName the console name,
    /// KeyboardLayoutName the layout editor's name field).
    Multiplayer::Fields room_fields{};
    static constexpr int KeyboardConsoleName = 4;
    static constexpr int KeyboardLayoutName = 5;
    static constexpr int KeyboardControlName = 6;
    int editing_field = -1;
    std::string edit_text;
    std::string edit_original;  ///< the text the keyboard opened on
    std::string keyboard_error; ///< why OK did not take the text, under it
    bool keyboard_shift{};
    bool keyboard_refocus{}; ///< back from DiscardText's Cancel: the keyboard takes the pad again
    bool overlay_focused{};
    /// Whether the last Update built an ImGui frame (a menu or the status message), which Draw
    /// then draws; in game it builds none and Draw draws the stats overlay.
    bool imgui_frame{};
};

} // namespace VitaFrontend

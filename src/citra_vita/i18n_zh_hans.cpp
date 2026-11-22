// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The menus in Simplified Chinese: each text by its English, as the code writes it (I18n::Tr).
// tools/i18n_check.py, run by the build, checks the table against the code's literals.

#include <array>

#include "citra_vita/i18n_tables.h"

namespace VitaFrontend::I18n {

namespace {

constexpr std::array table{std::to_array<Translation>({
    {"(another version's: can't be loaded)", "（来自其他版本，无法读取）"},
    {"(none)", "（无）"},
    {"1 frame", "1 帧"},
    {"2 frames", "2 帧"},
    {"3 frames", "3 帧"},
    {"4 frames", "4 帧"},
    {"a console with the same ID is in that room", "该房间中已有相同 ID 的主机"},
    {"A launch argument holds this at {} until Azahar exits.",
     "启动参数将此项固定为 {}，直到 Azahar 退出。"},
    {"Add ROM folder", "添加 ROM 文件夹"},
    {"Add this folder", "添加此文件夹"},
    {"Address", "地址"},
    {"All games", "所有游戏"},
    {"an unknown error", "未知错误"},
    {"Another plugin, such as vitastream, is using the USB port. Take it over for USB video? That "
     "plugin stops working until you reboot. Play without turns USB video off.",
     "其他插件（如 vitastream）正在使用 USB 端口。要接管该端口用于 USB 视频吗？在重启主机之前，该"
     "插件将无法使用。选择“直接游戏”会关闭 USB 视频。"},
    {"Another plugin, such as vitastream, is using the USB port. Take it over for USB video? That "
     "plugin stops working until you reboot.",
     "其他插件（如 vitastream）正在使用 USB 端口。要接管该端口用于 USB 视频吗？在重启主机之前，该"
     "插件将无法使用。"},
    {"another region", "其他地区"},
    {"Anti-aliasing (2x MSAA)", "抗锯齿（2x MSAA）"},
    {"Applied", "已应用"},
    {"Applied to the running game.", "已应用到正在运行的游戏。"},
    {"Applied when the game starts, if your copy is the version it was made for.",
     "如果你的游戏版本与补丁对应的版本一致，将在游戏启动时应用。"},
    {"Australia", "澳大利亚"},
    {"Auto", "自动"},
    {"Azahar can't start", "Azahar 无法启动"},
    {"Azahar could not restart to use azaharnative.skprx ({:#010x}). Start Azahar again.",
     "Azahar 无法重新启动以使用 azaharnative.skprx（{:#010x}）。请再次启动 Azahar。"},
    {"Azahar crashed and started again.", "Azahar 崩溃后已重新启动。"},
    {"Azahar is about to take over the GPU. Until you close it, notifications, the quick menu and "
     "plugin menus such as PSVshell's won't show, and the PS button won't leave Azahar.\n\nUse "
     "them now if you need them, then press X to continue.",
     "Azahar 即将接管 GPU。在你关闭 Azahar 之前，通知、快捷菜单以及 PSVshell "
     "等插件的菜单都不会显示，按 PS 按钮也无法离开 Azahar。\n\n如有需要，请现在使用，然后按 × "
     "继续。"},
    {"Azahar needs firmware 3.60, 3.65, 3.68 or 3.73.",
     "Azahar 需要 3.60、3.65、3.68 或 3.73 版本的系统软件。"},
    {"Azahar was updated. Reboot your console to finish the update.",
     "Azahar 已更新。请重启主机以完成更新。"},
    {"Back", "返回"},
    {"banned from that room", "已被该房间封禁"},
    {"Before Azahar starts", "Azahar 启动之前"},
    {"Below full speed, slows the sound down with the game instead of letting it break up.",
     "游戏低于全速时，让声音随游戏一起变慢，而不是断断续续。"},
    {"Bottom screen", "下屏"},
    {"Built in", "内置"},
    {"Built into Azahar for this game. It can't be turned off.",
     "由 Azahar 为此游戏内置，无法关闭。"},
    {"C-stick", "C 摇杆"},
    {"Cancel", "取消"},
    {"Change", "更改"},
    {"Changing the LiveArea...", "正在更改 LiveArea…"},
    {"Cheats", "金手指"},
    {"China", "中国"},
    {"Choose a CIA to install.", "请选择要安装的 CIA。"},
    {"CIA file: install it to play", "CIA 文件：安装后即可游玩"},
    {"Circle", "○ 按钮"},
    {"Close", "关闭"},
    {"Command buffer reported", "上报命令缓冲区"},
    {"Continue", "继续"},
    {"Controls", "按键"},
    {"Could not load this title: {}", "无法加载此游戏：{}"},
    {"Could not pin this title. Install Azahar's vpk again with VitaShell, then try again.",
     "无法固定此游戏。请用 VitaShell 重新安装 Azahar 的 vpk，然后重试。"},
    {"Could not pin this title. Restart the Vita, then try again.",
     "无法固定此游戏。请重启 Vita，然后重试。"},
    {"could not read the system mode", "无法读取系统模式"},
    {"Could not unpin this title. Restart the Vita, then try again.",
     "无法取消固定此游戏。请重启 Vita，然后重试。"},
    {"Cross", "× 按钮"},
    {"Cross again to remove", "再按 × 删除"},
    {"D-pad", "十字键"},
    {"D-pad down", "十字键下"},
    {"D-pad left", "十字键左"},
    {"D-pad right", "十字键右"},
    {"D-pad up", "十字键上"},
    {"Dark", "深色"},
    {"Decrypt this CIA, then install it again.", "请先解密此 CIA，然后重新安装。"},
    {"Default", "默认"},
    {"Delete", "删除"},
    {"Display", "显示"},
    {"Down", "方向键下"},
    {"Downloadable content", "追加内容"},
    {"Draws the top screen at a higher resolution: sharper in the Single screen and Large screen "
     "layouts, but much more GPU work (about 2.25 times at 1.5x), so slow games get slower. Can "
     "crash games that use a lot of video memory.",
     "以更高分辨率绘制上屏：在“单屏”和“大屏”布局下更清晰，但 GPU 负担大幅增加（1.5x 时约为 2.25 "
     "倍），本来就慢的游戏会更慢。占用显存较多的游戏可能会崩溃。"},
    {"Dump frame", "转储画面"},
    {"Empty", "空"},
    {"Enter an address and a nickname to join.", "输入地址和昵称后即可加入。"},
    {"Europe", "欧洲"},
    {"Exit", "退出"},
    {"Exit Azahar", "退出 Azahar"},
    {"Exit Azahar? {}'s progress since it last saved is lost.",
     "要退出 Azahar 吗？{} 上次存档之后的进度将会丢失。"},
    {"Fewer model writes", "减少模型写入"},
    {"Fewer texture writes", "减少纹理写入"},
    {"Follows the setting for all games.", "沿用所有游戏的设置。"},
    {"For full speed, play {} from the {} copy ({}), revision {}, without an update. This copy is "
     "from {} ({}), so it runs without Azahar's speed patches.",
     "如需全速运行，请使用 {} 的{}版（{}）修订版 {}，且不要安装更新数据。你的游戏是{}版（{}），因"
     "此不会应用 Azahar 的加速补丁。"},
    {"For full speed, play {} from the {} copy ({}), revision {}, without an update. This copy is "
     "revision {} or has an update installed, so {} of its {} speed patches are left out.",
     "如需全速运行，请使用 {0} 的{1}版（{2}）修订版 {3}，且不要安装更新数据。你的游戏是修订版 "
     "{4}，或已安装更新数据，因此 {6} 个加速补丁中有 {5} 个未应用。"},
    {"Force New 3DS mode", "强制 New 3DS 模式"},
    {"Frame skip limit", "跳帧上限"},
    {"Free {} MiB on the memory card, then install it again.",
     "请在存储卡上腾出 {} MiB 空间，然后重新安装。"},
    {"From {}.", "来自 {}。"},
    {"Game settings", "游戏设置"},
    {"Games made for 3D draw each picture twice, once for each eye, and the Vita shows one. On "
     "leaves the right eye's picture undrawn, which is faster. Turn it off for a game whose top "
     "screen stays grey, black or still.",
     "为 3D 设计的游戏会为左右眼各绘制一次画面，而 Vita 只显示其中一个。开启后不绘制右眼画面，速度更"
     "快。如果某个游戏的上屏一直是灰色、黑色或静止不动，请为它关闭此选项。"},
    {"GBA titles are not supported", "不支持 GBA 游戏"},
    {"Host ad hoc", "创建本地联机"},
    {"Hosting an ad hoc room at {}", "已创建本地联机房间，地址：{}"},
    {"How fast the game may run, in percent of full speed. No limit runs it as fast as the console"
     " can. Lower it to save battery.",
     "游戏的最高运行速度，以全速的百分比表示。“无限制”会让游戏以主机能达到的最快速度运行。调低可以"
     "省电。"},
    {"How opaque the small screen of the overlay layouts is.", "叠加布局中小屏幕的不透明度。"},
    {"How the two screens are arranged. Overlay: one screen fills the display and the other sits "
     "small and see-through in the bottom-right corner, where you can still touch it. Large "
     "overlay: the same with a bigger corner screen.",
     "两个屏幕的排列方式。叠加：一个屏幕占满画面，另一个以半透明小窗显示在右下角，仍可触摸。大号叠"
     "加：同上，但角落的屏幕更大。"},
    {"Hybrid", "混合"},
    {"In a room", "已加入房间"},
    {"In room \"{}\" ({} of {})", "已加入房间“{}”（{}/{} 人）"},
    {"In the ad hoc room at {}", "已加入 {} 的本地联机房间"},
    {"Install", "安装"},
    {"Install CIA", "安装 CIA"},
    {"Installed", "已安装"},
    {"Installed from a CIA. Removing it takes the game back to its own version.",
     "从 CIA 安装。删除后游戏将恢复为未更新的版本。"},
    {"Installed from a CIA. Removing it takes the game off the list.",
     "从 CIA 安装。删除后游戏将从列表中移除。"},
    {"Installed from a CIA. Removing it takes this content out of the game.",
     "从 CIA 安装。删除后游戏中将不再有此内容。"},
    {"Installed game", "游戏本体"},
    {"Installing {}...", "正在安装 {}…"},
    {"it is encrypted. Decrypt it, then try again.", "文件已加密。请解密后重试。"},
    {"Japan", "日本"},
    {"Join", "加入"},
    {"Join a room hosted by Azahar on a PC (Multiplayer > Host Room) or a room server, or play ad "
     "hoc with Vitas nearby: one hosts, the others join. Then everyone plays the same game in its "
     "local wireless mode.",
     "加入由电脑版 Azahar（Multiplayer > Host Room）或房间服务器创建的房间；也可以与附近的 Vita "
     "进行本地联机：一台创建，其余加入。然后所有人在同一款游戏的本地无线模式中游玩。"},
    {"Join ad hoc", "加入本地联机"},
    {"Join again", "重新加入"},
    {"Joined 2D quads", "合并 2D 方块"},
    {"Joining the ad hoc network...", "正在加入本地联机网络…"},
    {"Joining {}...", "正在加入 {}…"},
    {"kicked by the room's host", "已被房主踢出"},
    {"Korea", "韩国"},
    {"L and R", "L 和 R"},
    {"Language", "语言"},
    {"Large overlay", "大号叠加"},
    {"Large screen", "大屏"},
    {"Leave", "离开"},
    {"Left", "方向键左"},
    {"Left eye only", "仅绘制左眼"},
    {"Left out", "未应用"},
    {"Left out: your copy is a different version from the one it was made for.",
     "未应用：你的游戏版本与补丁对应的版本不同。"},
    {"Light", "浅色"},
    {"Load", "读取"},
    {"Load slot {}? The game goes back to {}, and the progress since then is lost.",
     "要读取槽位 {} 吗？游戏将回到 {} 的状态，此后的进度将会丢失。"},
    {"Loading slot {}...", "正在读取槽位 {}…"},
    {"Loading...", "正在加载…"},
    {"Looking for an ad hoc room...", "正在寻找本地联机房间…"},
    {"Memory devices", "存储设备"},
    {"Menu theme", "菜单主题"},
    {"Model files reported", "上报模型文件"},
    {"Multiplayer", "多人游戏"},
    {"Multiplayer is not available", "多人游戏不可用"},
    {"Native", "原生"},
    {"Native while Anti-aliasing is on. Turn Anti-aliasing off to choose a resolution.",
     "开启抗锯齿时固定为原生分辨率。关闭抗锯齿后才能选择分辨率。"},
    {"Nickname", "昵称"},
    {"No cheats", "没有金手指"},
    {"No patches", "没有补丁"},
    {"Azahar has no patches or engine settings for this game.",
     "Azahar 没有此游戏的补丁或引擎设置。"},
    {"Azahar has no patches for this game. Its engine settings, if it has any, show here once it "
     "has started.",
     "Azahar 没有此游戏的补丁。如果它有引擎设置，游戏启动过一次后会显示在这里。"},
    {"Looking for this game's engine settings...", "正在查找此游戏的引擎设置…"},
    {"No folders or CIA files here.", "这里没有文件夹或 CIA 文件。"},
    {"No folders or titles here.", "这里没有文件夹或游戏。"},
    {"No limit", "无限制"},
    {"no such user", "用户不存在"},
    {"No titles found. Copy .3ds or .cci files into {} or a ROM folder of the settings' System "
     "tab, or choose Install CIA.",
     "未找到游戏。请将 .3ds 或 .cci 文件复制到 {} 或设置“系统”标签页中的 ROM 文件夹，或选择“安装 CIA”。"},
    {"None", "无"},
    {"Not in a room", "未加入房间"},
    {"Not in a room: {}", "未加入房间：{}"},
    {"Not on the ad hoc network", "未连接本地联机网络"},
    {"Not on the ad hoc network: {}", "未连接本地联机网络：{}"},
    {"Not started: another plugin is using the USB port. Turn USB video off and on again to take "
     "it over.",
     "未启动：其他插件正在使用 USB 端口。关闭再打开 USB 视频即可接管。"},
    {"Not started: the USB port could not be set up. Reboot and try again.",
     "未启动：无法设置 USB 端口。请重启后重试。"},
    {"Off", "关"},
    {"Off while Vsync is on. Turn Vsync off to set a speed limit.",
     "垂直同步开启时不可用。关闭垂直同步后才能设置速度限制。"},
    {"Off.", "已关闭。"},
    {"OK", "确定"},
    {"On", "开"},
    {"On or off", "开/关"},
    {"On: active now.", "已开启：正在生效。"},
    {"On: applied while the game runs.", "已开启：游戏运行时生效。"},
    {"Open", "打开"},
    {"Open the folder, then press square to add it.", "打开文件夹，然后按 □ 添加。"},
    {"Overlay", "叠加"},
    {"Overlay opacity", "叠加不透明度"},
    {"Parent folder", "上一级"},
    {"Password", "密码"},
    {"Patches", "补丁"},
    {"Paused", "已暂停"},
    {"permission denied", "权限不足"},
    {"Pin", "固定"},
    {"Pin to LiveArea", "固定到 LiveArea"},
    {"Pin {} to the LiveArea as a bubble of its own?", "要将 {} 作为独立图标固定到 LiveArea 吗？"},
    {"Pinned: its LiveArea bubble starts the game in Azahar.",
     "已固定：点击其 LiveArea 图标即可在 Azahar 中启动游戏。"},
    {"Port", "端口"},
    {"Play without", "直接游戏"},
    {"Press X or O to continue.", "按 × 或 ○ 继续。"},
    {"Put this game's Gateway codes in {}: a [name] line for each cheat, then its code lines. "
     "Reopen this screen to see them.",
     "请将此游戏的 Gateway 代码写入 {}：每个金手指先写一行 [名称]，再写它的代码行。重新打开此画面"
     "即可看到。"},
    {"Puts the touch screen where the top screen would be.", "把触摸屏放到上屏的位置。"},
    {"Quit game", "退出游戏"},
    {"Rear touch, left half", "背面触摸板左半边"},
    {"Rear touch, right half", "背面触摸板右半边"},
    {"Region", "地区"},
    {"ROM folder", "ROM 文件夹"},
    {"Remove", "删除"},
    {"Remove the azaharnative.skprx line from your taiHEN config.txt (ux0:tai/config.txt or "
     "ur0:tai/config.txt), then reboot your console.",
     "请从 taiHEN 的 config.txt（ux0:tai/config.txt 或 ur0:tai/config.txt）中删除 "
     "azaharnative.skprx 这一行，然后重启主机。"},
    {"Removing it from the list keeps its files.", "从列表中删除后，文件夹中的文件仍会保留。"},
    {"Removing it keeps the game's save data.", "删除后游戏存档仍会保留。"},
    {"Resume", "继续游戏"},
    {"Right", "方向键右"},
    {"Right stick", "右摇杆"},
    {"Runs every game as on a New 3DS. Games with New 3DS enhancements turn them on, which needs "
     "more of the Vita's CPU and memory, so they can get slower. Off: only games made for the New "
     "3DS alone run as on one. A save state keeps the mode it was saved in.",
     "让所有游戏都像在 New 3DS 上一样运行。带有 New 3DS 增强功能的游戏会启用这些功能，这需要更多的 "
     "Vita CPU 和内存，因此可能变慢。关闭时，只有 New 3DS 专用游戏会以该模式运行。即时存档会保留保存"
     "时的模式。"},
    {"Save", "保存"},
    {"Save over slot {}? Its state from {} is replaced.",
     "要覆盖槽位 {} 吗？{} 保存的即时存档将被替换。"},
    {"Save states", "即时存档"},
    {"Saving slot {}...", "正在保存到槽位 {}…"},
    {"Screen layout", "屏幕布局"},
    {"Sends one screen to a PC over the USB cable, as a 400x240 webcam named Azahar. The Vita "
     "keeps its own layout: to see only the other screen, use Single screen (with Swap screens if "
     "needed).",
     "通过 USB 线将一个屏幕发送到电脑，在电脑上显示为名为 Azahar 的 400x240 摄像头。Vita "
     "上的屏幕布局不变：如果只想在 Vita 上看到另一个屏幕，请使用“单屏”（必要时配合“交换屏幕”）。"},
    {"Set for this game, the same as all games for now. Changing it for all games won't change it "
     "here.",
     "已为此游戏单独设置，目前与所有游戏的设置相同。更改所有游戏的设置不会影响此游戏。"},
    {"Set for this game. All games: {}.", "已为此游戏单独设置。所有游戏：{}。"},
    {"Settings", "设置"},
    {"Shows a notice as Azahar starts, while the quick menu, notifications and plugin menus such "
     "as PSVshell's still work. Off: Azahar takes over the GPU at once.",
     "Azahar 启动时显示提示，此时快捷菜单、通知以及 PSVshell 等插件菜单仍可使用。关：Azahar "
     "立即接管 GPU。"},
    {"Shows each picture whole at the display's refresh and holds the game to the display's rate. "
     "Off: pictures show at once and may tear, and Speed limit sets the pace.",
     "每幅画面在屏幕刷新时完整显示，并让游戏与屏幕刷新率同步。关：画面立即显示，可能出现撕裂，速度"
     "由“速度限制”决定。"},
    {"How the top screen fills the display in the Single screen and overlay layouts, with Swap "
     "screens off. Off: the 3DS's shape, with black bars at the sides. Widescreen hack: shows more "
     "of the scene at the sides; it can cause glitches, as some games leave the new edges empty or "
     "draw them wrong. Widescreen stretch: stretches the picture to the display's width.",
     "在“单屏”和叠加布局中（“交换屏幕”关闭时），上屏如何填满屏幕。关：保持 3DS 的比例，两侧有黑边。"
     "扩展宽屏：两侧显示更多场景；可能导致画面异常，部分游戏的新增边缘会是空白或绘制错误。拉伸宽屏："
     "将画面拉伸至屏幕宽度。"},
    {"Side by side", "并排"},
    {"Single screen", "单屏"},
    {"Skip the 3D right eye", "跳过 3D 右眼画面"},
    {"Slot {}", "槽位 {}"},
    {"Slot {} can't be loaded in a multiplayer room. Leave the room, then load it again.",
     "在多人游戏房间中无法读取槽位 {}。请离开房间，然后重新读取。"},
    {"Slot {} could not be loaded, so the game was stopped. Start the game again, and save to "
     "that slot again before you load it.",
     "无法读取槽位 {}，游戏已停止。请重新启动游戏，并在读取该槽位之前重新保存到该槽位。"},
    {"Slot {} could not be loaded. Save to it again, or load another slot.",
     "无法读取槽位 {}。请重新保存到该槽位，或读取其他槽位。"},
    {"Slot {} was not saved. Make sure the memory card has free space, then save again.",
     "槽位 {} 未能保存。请确认存储卡有可用空间，然后重新保存。"},
    {"Smooths jagged edges on the top screen. It costs GPU time, so slow games get slower, and the"
     " top screen stays at Native resolution while it is on. Can crash games that use a lot of "
     "video memory.",
     "平滑上屏的锯齿边缘。会占用 GPU 时间，本来就慢的游戏会更慢；开启时上屏保持原生分辨率。占用显"
     "存较多的游戏可能会崩溃。"},
    {"Sound", "声音"},
    {"Space", "空格"},
    {"Speed", "速度"},
    {"Speed limit", "速度限制"},
    {"Square", "□ 按钮"},
    {"Startup notice", "启动提示"},
    {"Stretch audio", "音频拉伸"},
    {"Swap screens", "交换屏幕"},
    {"System", "系统"},
    {"Tab", "切换分页"},
    {"Taiwan", "台湾"},
    {"Take over", "接管"},
    {"Take {}'s bubble off the LiveArea?", "要从 LiveArea 移除 {} 的图标吗？"},
    {"Takes effect when a game starts.", "下次启动游戏时生效。"},
    {"the connection to the room was lost", "与房间的连接已断开"},
    {"The console region reported to the game. Auto picks one the game accepts.",
     "报告给游戏的主机地区。“自动”会选择游戏接受的地区。"},
    {"the CPU could not start: azaharnative could not take a core",
     "CPU 无法启动：azaharnative 无法占用核心"},
    {"The game crashed.", "游戏崩溃了。"},
    {"The game crashed. Saving a crash report...", "游戏崩溃了。正在保存崩溃报告…"},
    {"The game draws its 3D scene twice, once per eye, even with 3D off. Only one picture is "
     "shown, so only the left eye's is drawn.",
     "即使关闭 3D，游戏也会为两只眼睛各绘制一次 3D 场景。由于只显示一幅画面，现在只绘制左眼的画面"
     "。"},
    {"The game list also shows the titles in these folders, besides ux0:data/azahar/roms: on a "
     "USB or SD2Vita card (uma0:), for example. Only the files in the folder itself, not in "
     "folders inside it.",
     "除 ux0:data/azahar/roms 外，游戏列表也会显示这些文件夹中的游戏：例如 USB 或 SD2Vita "
     "存储卡（uma0:）上的文件夹。只读取文件夹本身中的文件，不包括其子文件夹。"},
    {"The game draws its text and menus one small square at a time. Squares drawn one after "
     "another are joined into one draw.",
     "游戏逐个绘制文字和菜单的小方块。现在把连续绘制的方块合并为一次绘制。"},
    {"The game rewrites a texture's settings every time it uses one. Now they are written only "
     "when they change, which saves work.",
     "游戏每次使用纹理时都会重写其设置。现在只在设置变化时写入，减少了工作量。"},
    {"The game rewrites each model's visibility every frame. Now it is written only when it "
     "changes, which saves work.",
     "游戏每帧都会重写每个模型的可见性。现在只在变化时写入，减少了工作量。"},
    {"The game takes turns between two buffers for each frame's GPU commands. With three, it waits"
     " less for the GPU.",
     "游戏轮流使用两个缓冲区存放每帧的 GPU 命令。改用三个后，等待 GPU 的时间更少。"},
    {"The game tells Azahar where the model and texture files it loads are, so Azahar has less to "
     "keep track of.",
     "游戏会向 Azahar 上报所加载的模型和纹理文件的位置，从而减少 Azahar 需要追踪的内容。"},
    {"The game tells Azahar which command buffer each frame fills, so Azahar has less to keep "
     "track of.",
     "游戏会向 Azahar 上报每帧填充的是哪个命令缓冲区，从而减少 Azahar 需要追踪的内容。"},
    {"The game waits for the GPU to finish a picture before reading it, as on a 3DS. Off can run "
     "faster, but may cause flickering, outdated or missing pictures and broken effects.",
     "游戏在读取画面前会等待 GPU 绘制完成，与在 3DS 上一样。关闭后运行可能更快，但也可能导致画面闪"
     "烁、过时或缺失，以及特效错误。"},
    {"The install stopped. Install the CIA again.", "安装已中断。请重新安装 CIA。"},
    {"The language of Azahar's menus. A game keeps its own.",
     "Azahar 菜单使用的语言。不影响游戏内的语言。"},
    {"the MAC address is taken in that room", "该房间中已有相同的 MAC 地址"},
    {"The menus' colours: dark grey, or the light grey of the 3DS HOME Menu.",
     "菜单的颜色：深灰色，或 3DS HOME 菜单的浅灰色。"},
    {"the network failed", "网络出错"},
    {"the nickname is taken in that room", "该房间中昵称已被使用"},
    {"the room did not answer", "房间没有响应"},
    {"the room is full", "房间已满"},
    {"the room runs another version of the protocol", "房间使用的协议版本不同"},
    {"The title's bubble is off the LiveArea.", "已从 LiveArea 移除此游戏的图标。"},
    {"The Vita input that presses the 3DS's A.", "对应 3DS A 键的 Vita 按键。"},
    {"The Vita input that presses the 3DS's B.", "对应 3DS B 键的 Vita 按键。"},
    {"The Vita input that presses the 3DS's d-pad down.", "对应 3DS 十字键下的 Vita 按键。"},
    {"The Vita input that presses the 3DS's d-pad left.", "对应 3DS 十字键左的 Vita 按键。"},
    {"The Vita input that presses the 3DS's d-pad right.", "对应 3DS 十字键右的 Vita 按键。"},
    {"The Vita input that presses the 3DS's d-pad up.", "对应 3DS 十字键上的 Vita 按键。"},
    {"The Vita input that presses the 3DS's L.", "对应 3DS L 键的 Vita 按键。"},
    {"The Vita input that presses the 3DS's R.", "对应 3DS R 键的 Vita 按键。"},
    {"The Vita input that presses the 3DS's Select.", "对应 3DS SELECT 键的 Vita 按键。"},
    {"The Vita input that presses the 3DS's Start.", "对应 3DS START 键的 Vita 按键。"},
    {"The Vita input that presses the 3DS's X.", "对应 3DS X 键的 Vita 按键。"},
    {"The Vita input that presses the 3DS's Y.", "对应 3DS Y 键的 Vita 按键。"},
    {"The Vita input that presses the New 3DS's ZL (and the Circle Pad Pro's). A rear touch half "
     "is held while a finger is anywhere on it.",
     "对应 New 3DS ZL 键（以及扩展滑杆的 ZL 键）的 Vita 按键。手指放在背面触摸板某一半的任意位置时"
     "，即视为按住该半边。"},
    {"The Vita input that presses the New 3DS's ZR (and the Circle Pad Pro's). A rear touch half "
     "is held while a finger is anywhere on it.",
     "对应 New 3DS ZR 键（以及扩展滑杆的 ZR 键）的 Vita 按键。手指放在背面触摸板某一半的任意位置时"
     "，即视为按住该半边。"},
    {"This CIA cannot be read. Make it again from your game.",
     "无法读取此 CIA。请从你的游戏重新制作。"},
    {"This CIA is gone. Choose it again with Install CIA.",
     "此 CIA 已不存在。请通过“安装 CIA”重新选择。"},
    {"Three command buffers", "三个命令缓冲区"},
    {"Title {:016X}.", "游戏 ID {:016X}。"},
    {"To report it, send these files:", "如需报告，请发送以下文件："},
    {"To report it, send this file:", "如需报告，请发送此文件："},
    {"Top screen", "上屏"},
    {"Top screen resolution", "上屏分辨率"},
    {"Triangle", "△ 按钮"},
    {"unknown error", "未知错误"},
    {"unknown file type", "未知的文件类型"},
    {"Unpin", "取消固定"},
    {"unsupported file format", "不支持的文件格式"},
    {"Up", "方向键上"},
    {"Update", "更新数据"},
    {"USA", "美国"},
    {"USB video", "USB 视频"},
    {"Use all games' setting", "使用所有游戏的设置"},
    {"Vsync", "垂直同步"},
    {"Wait for GPU results", "等待 GPU 结果"},
    {"What the right stick moves: the 3DS's C-stick (the New 3DS's, and the Circle Pad Pro's in "
     "games made for it), L and R (left and right, for games that turn the camera with them), or "
     "the d-pad.",
     "右摇杆的作用：3DS 的 C 摇杆（New 3DS 的，以及支持扩展滑杆的游戏中扩展滑杆的摇杆）、L 和 R"
     "（左右推动，用于以 L、R 转动视角的游戏）或十字键。"},
    {"When the GPU falls behind, the busier screen skips frames so the game keeps its speed. This "
     "sets how many frames in a row it may skip: higher keeps heavy games faster but less smooth. "
     "Off draws every frame and the game slows down instead.",
     "GPU 跟不上时，较忙的屏幕会跳帧，以保持游戏速度。此项设置最多可连续跳过的帧数：越高，负担重的"
     "游戏越快，但越不流畅。关：绘制每一帧，游戏会变慢。"},
    {"Widescreen", "宽屏"},
    {"Widescreen hack", "扩展宽屏"},
    {"Widescreen stretch", "拉伸宽屏"},
    {"wrong password", "密码错误"},
    {"You can turn this screen off in Settings > System > Startup notice.",
     "可在“设置 > 系统 > 启动提示”中关闭此画面。"},
    {"{} could not be loaded ({:#010x}). Reinstall the vpk.",
     "无法加载 {}（{:#010x}）。请重新安装 vpk。"},
    {"{} is installed.", "{} 已安装。"},
    {"{} sets its own: {}. Change it in Game settings.",
     "{} 有单独的设置：{}。可在“游戏设置”中更改。"},
    {"{} title", "{} 个游戏"},
    {"{} to {}, in Game settings", "{}设为{}（游戏设置）"},
    {"{} to {}, in Settings", "{}设为{}（设置）"},
    {"{} titles", "{} 个游戏"},
    {"{}: its downloadable content is installed.", "{}：追加内容已安装。"},
    {"{}: its update is installed.", "{}：更新数据已安装。"},
})};

constexpr std::array<const char*, 2> language_names{"English", "简体中文"};

} // Anonymous namespace

std::span<const Translation> SimplifiedChinese() {
    return table;
}

std::span<const char* const> LanguageNames() {
    return language_names;
}

} // namespace VitaFrontend::I18n

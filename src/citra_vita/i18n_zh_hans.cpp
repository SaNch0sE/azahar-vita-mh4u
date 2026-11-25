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
    {", hidden", "，已隐藏"},
    {"1 frame", "1 帧"},
    {"2 frames", "2 帧"},
    {"3 frames", "3 帧"},
    {"3DS CPU clock", "3DS CPU 频率"},
    {"3DS size", "3DS 原始大小"},
    {"4 frames", "4 帧"},
    {"a console with the same ID is in that room", "该房间中已有相同 ID 的主机"},
    {"A launch argument holds this at {} until Azahar exits.",
     "启动参数将此项固定为 {}，直到 Azahar 退出。"},
    {"Add ROM folder", "添加 ROM 文件夹"},
    {"Add this folder", "添加此文件夹"},
    {"Address", "地址"},
    {"All games", "所有游戏"},
    {"All games then use it.", "之后所有游戏都将使用它。"},
    {"an unknown error", "未知错误"},
    {"Another layout has this name.", "已有其他布局使用此名称。"},
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
    {"Bring to front", "置于最前"},
    {"Built in", "内置"},
    {"Built into Azahar for this game. It can't be turned off.",
     "由 Azahar 为此游戏内置，无法关闭。"},
    {"Buttons", "按键"},
    {"Buttons below", "下方的按键"},
    {"C-stick", "C 摇杆"},
    {"Cancel", "取消"},
    {"{} seems to be stuck. Turn off Multicore in the game's settings, then restart the game.",
     "{} 似乎卡住了。请在游戏设置中关闭“多核”，然后重新启动游戏。"},
    {"Centre on the display", "在显示屏上居中"},
    {"Change", "更改"},
    {"Changing the LiveArea...", "正在更改 LiveArea…"},
    {"Cheats", "金手指"},
    {"Check", "检查"},
    {"Check file", "检查文件"},
    {"Checking {}...", "正在检查 {}…"},
    {"China", "中国"},
    {"Choose a CIA to install.", "请选择要安装的 CIA。"},
    {"Choose a game to check.", "请选择要检查的游戏。"},
    {"CIA file: install it to play", "CIA 文件：安装后即可游玩"},
    {"Circle", "○ 按钮"},
    {"Close", "关闭"},
    {"Command buffer reported", "上报命令缓冲区"},
    {"Console name", "主机名称"},
    {"Continue", "继续"},
    {"Control layout", "按键布局"},
    {"Control layout editor", "按键布局编辑器"},
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
    {"D-pad", "十字键"},
    {"D-pad down", "十字键下"},
    {"D-pad left", "十字键左"},
    {"D-pad right", "十字键右"},
    {"D-pad up", "十字键上"},
    {"Dark", "深色"},
    {"Data", "数据"},
    {"Decrypt this CIA, then install it again.", "请先解密此 CIA，然后重新安装。"},
    {"Decrypt this game, then install it again.", "请先解密此游戏，然后重新安装。"},
    {"Default", "默认"},
    {"Delete", "删除"},
    {"Delete the control layout {}? The games that use it go back to the buttons in the "
     "Controls tab.",
     "删除按键布局 {}？使用它的游戏将改回“操作”标签页中的按键。"},
    {"Delete the layout {}? The games that use it go back to Default.",
     "要删除布局 {} 吗？使用它的游戏将恢复为“默认”。"},
    {"Delete this layout", "删除此布局"},
    {"Delete {}'s save data? The game starts over as new. This can't be undone.",
     "要删除 {} 的存档吗？游戏将从头开始，且无法撤销。"},
    {"Delete {}'s shader cache? The game stutters for a while as it makes its shaders again.",
     "要删除 {} 的着色器缓存吗？游戏重新生成着色器时会卡顿一段时间。"},
    {"Deleting {}'s save data...", "正在删除 {} 的存档…"},
    {"Deleting {}'s shader cache...", "正在删除 {} 的着色器缓存…"},
    {"Discard", "放弃"},
    {"Discard what you typed?", "要放弃输入的内容吗？"},
    {"Display", "显示"},
    {"Down", "方向键下"},
    {"Download Play", "下载游玩"},
    {"Downloadable content", "追加内容"},
    {"Draws both screens at a higher resolution: sharper wherever a screen is shown larger than on "
     "the 3DS, but much more GPU work (about 2.25 times at 1.5x), so slow games get slower. Can "
     "crash games that use a lot of video memory.",
     "以更高分辨率绘制上下两屏：画面显示得比 3DS 上大时更清晰，但 GPU 负担大幅增加（1.5x 时约为 "
     "2.25 倍），本来就慢的游戏会更慢。占用显存较多的游戏可能会崩溃。"},
    {"Dump frame", "转储画面"},
    {"Dutch", "荷兰语"},
    {"Empty", "空"},
    {"English", "英语"},
    {"Enter a port from 1 to 65535.", "请输入 1 到 65535 之间的端口号。"},
    {"Enter an address and a nickname to join.", "输入地址和昵称后即可加入。"},
    {"Europe", "欧洲"},
    {"Exit", "退出"},
    {"Exit Azahar", "退出 Azahar"},
    {"Exit Azahar? {}'s progress since it last saved is lost.",
     "要退出 Azahar 吗？{} 上次存档之后的进度将会丢失。"},
    {"extra content", "追加内容"},
    {"Fewer model writes", "减少模型写入"},
    {"Fewer texture writes", "减少纹理写入"},
    {"File", "文件"},
    {"Fill the display", "填满显示屏"},
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
    {"French", "法语"},
    {"From {}.", "来自 {}。"},
    {"Full check", "完整检查"},
    {"Dismiss", "关闭"},
    {"Game settings", "游戏设置"},
    {"Games made for 3D draw each picture twice, once for each eye, and the Vita shows one. On "
     "leaves the right eye's picture undrawn, which is faster. Turn it off for a game whose top "
     "screen stays grey, black or still.",
     "为 3D 设计的游戏会为左右眼各绘制一次画面，而 Vita 只显示其中一个。开启后不绘制右眼画面，速度更"
     "快。如果某个游戏的上屏一直是灰色、黑色或静止不动，请为它关闭此选项。"},
    {"GBA titles are not supported", "不支持 GBA 游戏"},
    {"German", "德语"},
    {"Give the layout a name.", "请为布局命名。"},
    {"Host ad hoc", "创建本地联机"},
    {"Hosting an ad hoc room at {}", "已创建本地联机房间，地址：{}"},
    {"How fast the emulated 3DS CPU runs, in percent of a 3DS's. Lower can speed up games the "
     "Vita's CPU is too slow for; higher can smooth games that slow down on a 3DS too. Either can "
     "make a game freeze or misbehave.",
     "模拟的 3DS CPU 的运行速度，以 3DS 的百分比表示。调低可以让 Vita 的 CPU 跑不动的游戏变快；调高"
     "可以让在 3DS 上也会变慢的游戏更流畅。两者都可能让游戏卡死或出错。"},
    {"How fast the game may run, in percent of full speed. No limit runs it as fast as the console"
     " can. Lower it to save battery.",
     "游戏的最高运行速度，以全速的百分比表示。“无限制”会让游戏以主机能达到的最快速度运行。调低可以"
     "省电。"},
    {"How opaque the small screen of the overlay layouts is.", "叠加布局中小屏幕的不透明度。"},
    {"How the two screens are arranged. Overlay: one screen fills the display and the other sits "
     "small and see-through in the bottom-right corner, where you can still touch it. Large "
     "overlay: the same with a bigger corner screen. Your own layouts, made in the Layout editor "
     "below, follow these.",
     "两个屏幕的排列方式。叠加：一个屏幕占满画面，另一个以半透明小窗显示在右下角，仍可触摸。大号叠"
     "加：同上，但角落的屏幕更大。你自己的布局（在下方的“布局编辑器”中制作）排在这些之后。"},
    {"Hybrid", "混合"},
    {"In a room", "已加入房间"},
    {"In front", "已在最前"},
    {"In room \"{}\" ({} of {})", "已加入房间“{}”（{}/{} 人）"},
    {"In the ad hoc room at {}", "已加入 {} 的本地联机房间"},
    {"Install", "安装"},
    {"Install CIA", "安装 CIA"},
    {"Install to system", "安装到系统"},
    {"Installed", "已安装"},
    {"Installed from a CIA. Removing it takes the game back to its own version.",
     "从 CIA 安装。删除后游戏将恢复为未更新的版本。"},
    {"Installed from a CIA. Removing it takes the game off the list.",
     "从 CIA 安装。删除后游戏将从列表中移除。"},
    {"Installed from a CIA. Removing it takes this content out of the game.",
     "从 CIA 安装。删除后游戏中将不再有此内容。"},
    {"Installed game", "游戏本体"},
    {"Installing {}...", "正在安装 {}…"},
    {"Installs the game onto the emulated SD card from this file, as installing a CIA of it "
     "does. The installed game shows in the list beside this file, and both use the same save "
     "data.",
     "从此文件将游戏安装到模拟的 SD 卡，与安装它的 CIA 相同。安装后的游戏会显示在列表中此文件的旁边，两者共用同一份存档。"},
    {"it is encrypted. Decrypt it, then try again.", "文件已加密。请解密后重试。"},
    {"Internal resolution", "内部分辨率"},
    {"Italian", "意大利语"},
    {"Japan", "日本"},
    {"Japanese", "日语"},
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
    {"Korean", "韩语"},
    {"L and R", "L 和 R"},
    {"Language", "语言"},
    {"Large overlay", "大号叠加"},
    {"Large screen", "大屏"},
    {"Launch anyway", "仍然启动"},
    {"Layout editor", "布局编辑器"},
    {"Layout name", "布局名称"},
    {"Leave", "离开"},
    {"Leave the control layout editor? The changes are not saved.", "离开按键布局编辑器？更改不会保存。"},
    {"Leave the layout editor? The changes are not saved.", "要离开布局编辑器吗？更改不会被保存。"},
    {"Left", "方向键左"},
    {"Left and right: the Vita input that presses it. {}: a button on the screen for it, or "
     "none.",
     "左右：按下它的 Vita 输入。{}：为它在屏幕上放一个按钮，或取消。"},
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
    {"Make a control layout of your own: the Vita input each 3DS button answers to, and "
     "buttons on the screen that a finger on the display presses, such as ZL and ZR without "
     "the rear touch panel. It opens on the Control layout above: a layout of your own is "
     "changed, the buttons below are the start of a new one.",
     "制作你自己的按键布局：每个 3DS 按键对应的 Vita 输入，以及手指在屏幕上按下的屏幕按钮，例如不用背面触摸板的 ZL 和 ZR。它从上方的“按键布局”打开：你自己的布局会被修改，下方的按键则作为新布局的起点。"},
    {"Memory devices", "存储设备"},
    {"Menu theme", "菜单主题"},
    {"Model files reported", "上报模型文件"},
    {"Motion controls", "体感操作"},
    {"Even frame pacing (experimental)", "均匀帧节奏（实验性）"},
    {"Fast shadows", "快速阴影"},
    {"Smooth picture at Native", "原生分辨率平滑画面"},
    {"At Internal resolution Native, scales the screens to the display smoothly instead of pixel "
     "for pixel: softer, without the uneven blocky pixels. Free on the GPU. Higher resolutions are "
     "always smooth.",
     "在内部分辨率为原生时，以平滑方式将画面缩放到屏幕，而不是逐像素缩放：更柔和，没有不均匀的块状像素。不占用 GPU。更高分辨率始终是平滑的。"},
    {"Speed options", "速度选项"},
    {"Offers Per-vertex effect lighting and HUD resolution for this game and starts it with Fast "
     "shadows and Parallel geometry on.",
     "为此游戏提供逐顶点特效光照和 HUD 分辨率选项，并默认开启快速阴影和并行几何。"},
    {"Reads the game's shadow map once per pixel instead of four times. Shadows stay in place but get hard, stepped edges; the GPU does less at every lit pixel in shadowed scenes, about 15% of the 3D picture in a Monster Hunter 4 Ultimate field.",
     "每个像素只读取一次游戏的阴影贴图，而不是四次。阴影位置不变，但边缘变硬、呈阶梯状；在有阴影的场景中，GPU 在每个受光像素上的工作量减少，在《怪物猎人4 终极版》的野外约占 3D 画面的 15%。"},
    {"Per-vertex effect lighting", "逐顶点特效光照"},
    {"Parallel geometry", "并行几何"},
    {"Builds effect particles side by side instead of one after another. Much faster where effects "
     "pile up (a wyvern taking off); an effect that relies on the one before it could look wrong.",
     "同时构建特效粒子，而不是逐个构建。在特效大量叠加时（如飞龙起飞）快得多；依赖前一个粒子的特效可能显示异常。"},
    {"Lights blended effects such as mist and smoke at their corners instead of at every pixel. They may look slightly different; the GPU does much less work when such effects cover the screen.",
     "在半透明特效（如雾气和烟尘）的顶点处计算光照，而不是在每个像素上计算。它们看起来可能略有不同；当这类特效覆盖屏幕时，GPU 的工作量会大大减少。"},
    {"Experimental. Shows the game's pictures at an even beat of two refreshes: one that is ready "
     "early waits for its turn. For a game that runs at 30 frames a second and still looks choppy. "
     "It works with Vsync on, and can show a picture up to two refreshes (33 ms) later.",
     "实验性功能。以每两次刷新一帧的均匀节奏显示游戏画面：提前完成的画面会等待自己的时机。适用于以每秒 30 帧运行但仍显得卡顿的游戏。需要开启垂直同步，画面最多可能晚两次刷新（33 毫秒）显示。"},
    {"HUD resolution", "HUD 分辨率"},
    {"Draws the top screen's HUD, text and menus at a higher resolution over the 3D picture, which "
     "stays at the internal resolution: sharper text for little GPU work. It applies above the "
     "internal resolution, for example 1.5x with Internal resolution at 1.25x.",
     "以更高的分辨率在 3D 画面之上绘制上屏的 HUD、文字和菜单，3D 画面保持内部分辨率：文字更清晰，GPU 开销很小。仅在高于内部分辨率时生效，例如内部分辨率为 1.25x 时选择 1.5x。"},
    {"SaNch0sE optimizations", "SaNch0sE 优化"},
    {"Speed-ups made for this game on the Vita, with the same picture. Its sprite, grass and "
     "particle batches get the room they need, and stay short where their sprites lie far apart "
     "in the game's data. A screen the layout does not show is not drawn.",
     "为本游戏在 Vita 上所做的提速，画面不变。精灵、草和粒子的批次获得所需的空间，并在精灵于游戏数据中相距较远时保持较短。布局中未显示的屏幕不会被绘制。"},
    {"Touch a hidden bottom screen", "触摸隐藏的下屏"},
    {"With a layout that shows the top screen alone, touching the display still presses the "
     "bottom screen where it would be. Turn it off so that a brush of the display presses "
     "nothing.",
     "在只显示上屏的布局中，触摸屏幕仍会按到下屏原本所在的位置。关闭后，误触屏幕不会按到任何东西。"},
    {"Move: d-pad, left stick or drag. Size: L, R or pinch.", "移动：方向键、左摇杆或拖动。大小：L、R 或双指缩放。"},
    {"Move: d-pad, left stick or drag. Size: L, R or pinch. Stretch: right stick.",
     "移动：方向键、左摇杆或拖动。缩放：L、R 或双指捏合。拉伸：右摇杆。"},
    {"Multicore", "多核"},
    {"Multiplayer", "多人游戏"},
    {"Multiplayer is not available", "多人游戏不可用"},
    {"Name", "名称"},
    {"Native", "原生"},
    {"Native while Anti-aliasing is on. Turn Anti-aliasing off to choose a resolution.",
     "开启抗锯齿时固定为原生分辨率。关闭抗锯齿后才能选择分辨率。"},
    {"Needs {} MiB on the memory card.", "需要存储卡上 {} MiB 的空间。"},
    {"New layout", "新布局"},
    {"Next button", "下一个按钮"},
    {"Nickname", "昵称"},
    {"No button on the screen yet: put some there with Buttons.", "屏幕上还没有按钮：用“按键”放置一些。"},
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
    {"Nothing to delete.", "没有可删除的内容。"},
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
    {"On screen", "屏幕上"},
    {"On-screen buttons' opacity", "屏幕按钮的不透明度"},
    {"On: active now.", "已开启：正在生效。"},
    {"On: applied while the game runs.", "已开启：游戏运行时生效。"},
    {"Opacity", "不透明度"},
    {"Open", "打开"},
    {"Open the folder, then press square to add it.", "打开文件夹，然后按 □ 添加。"},
    {"Other screen", "另一个屏幕"},
    {"Overlay", "叠加"},
    {"Overlay opacity", "叠加不透明度"},
    {"Parent folder", "上一级"},
    {"Password", "密码"},
    {"Packing the crash report...", "正在打包崩溃报告…"},
    {"Patches", "补丁"},
    {"Paused", "已暂停"},
    {"PS + Circle", "PS + ○"},
    {"PS + Cross", "PS + ×"},
    {"PS + Down", "PS + 方向键下"},
    {"PS + L", "PS + L"},
    {"PS + Left", "PS + 方向键左"},
    {"PS + R", "PS + R"},
    {"PS + Right", "PS + 方向键右"},
    {"PS + Select", "PS + Select"},
    {"PS + Square", "PS + □"},
    {"PS + Start", "PS + Start"},
    {"PS + Triangle", "PS + △"},
    {"PS + Up", "PS + 方向键上"},
    {"permission denied", "权限不足"},
    {"Pin", "固定"},
    {"Pin to LiveArea", "固定到 LiveArea"},
    {"Pin {} to the LiveArea as a bubble of its own?", "要将 {} 作为独立图标固定到 LiveArea 吗？"},
    {"Pinned: its LiveArea bubble starts the game in Azahar.",
     "已固定：点击其 LiveArea 图标即可在 Azahar 中启动游戏。"},
    {"Place the two screens yourself: move, size and turn each one and see it on the display "
     "as you go, then save it under a name. It opens on the Screen layout above: a layout of "
     "your own is changed, a built-in one is the start of a new one. Your layouts follow the "
     "built-in ones in Screen layout.",
     "自行摆放两个屏幕：移动、缩放和旋转每个屏幕，并在显示屏上实时查看效果，然后命名保存。编辑器从"
     "上方的“屏幕布局”打开：如果是你自己的布局，则修改它；如果是内置布局，则以它为起点新建布局。"
     "你的布局排在“屏幕布局”中内置布局之后。"},
    {"Port", "端口"},
    {"Portuguese", "葡萄牙语"},
    {"Play without", "直接游戏"},
    {"Press X or O to continue.", "按 × 或 ○ 继续。"},
    {"Put this game's Gateway codes in {}: a [name] line for each cheat, then its code lines. "
     "Reopen this screen to see them.",
     "请将此游戏的 Gateway 代码写入 {}：每个金手指先写一行 [名称]，再写它的代码行。重新打开此画面"
     "即可看到。"},
    {"Puts the touch screen where the top screen would be.", "把触摸屏放到上屏的位置。"},
    {"Quit game", "退出游戏"},
    {"Quit the game to delete it.", "退出游戏后才能删除。"},
    {"Quit {}? Progress since the game last saved is lost.",
     "要退出 {} 吗？游戏上次存档之后的进度将会丢失。"},
    {"Reads the whole file and compares every part with the checksums 3DS files carry, to find a "
     "damaged or incomplete copy. A large game takes a few minutes.",
     "读取整个文件，并将每个部分与 3DS 文件自带的校验值比对，以找出损坏或不完整的副本。较大的游戏"
     "需要几分钟。"},
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
    {"Revision {}.", "修订版 {}。"},
    {"Right", "方向键右"},
    {"Right stick", "右摇杆"},
    {"Runs every game as on a New 3DS. Games with New 3DS enhancements turn them on, which needs "
     "more of the Vita's CPU and memory, so they can get slower. Off: only games made for the New "
     "3DS alone run as on one. A save state keeps the mode it was saved in.",
     "让所有游戏都像在 New 3DS 上一样运行。带有 New 3DS 增强功能的游戏会启用这些功能，这需要更多的 "
     "Vita CPU 和内存，因此可能变慢。关闭时，只有 New 3DS 专用游戏会以该模式运行。即时存档会保留保存"
     "时的模式。"},
    {"Runs the 3DS's CPU cores on two of the Vita's cores at once, which is faster in most games. "
     "Turn it off for a game that freezes or misbehaves with it on.",
     "在 Vita 的两个核心上同时运行 3DS 的 CPU 核心，大多数游戏会更快。如果某个游戏开启后卡死或运行异"
     "常，请为它关闭此选项。"},
    {"Russian", "俄语"},
    {"Save", "保存"},
    {"Save as new", "另存为新布局"},
    {"Save control layout", "保存按键布局"},
    {"Save data", "存档"},
    {"Save layout", "保存布局"},
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
    {"Set by the control layout. Change it in the control layout editor, or choose Buttons "
     "below.",
     "由按键布局设定。请在按键布局编辑器中更改，或选择“下方的按键”。"},
    {"Set for this game, the same as all games for now. Changing it for all games won't change it "
     "here.",
     "已为此游戏单独设置，目前与所有游戏的设置相同。更改所有游戏的设置不会影响此游戏。"},
    {"Set for this game. All games: {}.", "已为此游戏单独设置。所有游戏：{}。"},
    {"Settings", "设置"},
    {"Shader cache", "着色器缓存"},
    {"Show this screen", "显示此屏幕"},
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
    {"Shows the speed and frame rates in the top-right corner while a game runs.",
     "游戏运行时在右上角显示速度和帧率。"},
    {"Side by side", "并排"},
    {"Simplified Chinese", "简体中文"},
    {"Simulate 3DS GPU timings", "模拟 3DS GPU 耗时"},
    {"Single screen", "单屏"},
    {"Size", "大小"},
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
    {"Smooths jagged edges on the top screen. It costs GPU time, so slow games get slower, and "
     "Internal resolution stays at Native while it is on. Can crash games that use a lot of video "
     "memory.",
     "平滑上屏的锯齿边缘。会占用 GPU 时间，本来就慢的游戏会更慢；开启时内部分辨率保持原生。占用显"
     "存较多的游戏可能会崩溃。"},
    {"Sound", "声音"},
    {"Space", "空格"},
    {"Spanish", "西班牙语"},
    {"Speed", "速度"},
    {"Speed limit", "速度限制"},
    {"Shift", "大小写"},
    {"Square", "□ 按钮"},
    {"Square: delete", "□ 按钮：删除"},
    {"Startup notice", "启动提示"},
    {"Stats overlay", "性能信息"},
    {"Stretch audio", "音频拉伸"},
    {"Stretch over the display", "拉伸至整个显示屏"},
    {"Swap screens", "交换屏幕"},
    {"Swap screens shortcut", "交换屏幕快捷键"},
    {"Swaps the screens while you play. With PS: hold the PS button and press the other; the game "
     "sees neither, and the PS button alone still opens this menu as you let go of it. A rear "
     "touch half also presses the 3DS button it is mapped to.",
     "游戏中交换上下屏幕。PS 组合：按住 PS 按钮再按另一个按钮，游戏不会收到这两个按钮；单独按 PS "
     "按钮时，松开后仍会打开此菜单。背面触摸板的半边同时也会按下它所对应的 3DS 按键。"},
    {"System", "系统"},
    {"System language", "系统语言"},
    {"Tab", "切换分页"},
    {"Taiwan", "台湾"},
    {"Take over", "接管"},
    {"Take {}'s bubble off the LiveArea?", "要从 LiveArea 移除 {} 的图标吗？"},
    {"Takes effect when a game starts.", "下次启动游戏时生效。"},
    {"The 3DS's language, which games show their text in. With region Auto, a language "
     "the game's region lacks gives that region's own.",
     "3DS 的语言，游戏以此语言显示文字。地区为“自动”时，若游戏的地区没有该语言，则使用该地区自己的语言。"},
    {"the connection to the room was lost", "与房间的连接已断开"},
    {"The 3DS's user name, which games show: in greetings, as a save's owner, to other players. "
     "Up to 10 characters. Empty: AZAHAR.",
     "3DS 的用户名称，游戏会显示它：问候语、存档的所有者、其他玩家看到的名字。最多 10 个字符。留"
     "空：AZAHAR。"},
    {"The console region reported to the game. Auto picks one the game accepts.",
     "报告给游戏的主机地区。“自动”会选择游戏接受的地区。"},
    {"the CPU could not start: azaharnative could not take a core",
     "CPU 无法启动：azaharnative 无法占用核心"},
    {"the game", "游戏本体"},
    {"The game crashed.", "游戏崩溃了。"},
    {"The game crashed. Saving a crash report...", "游戏崩溃了。正在保存崩溃报告…"},
    {"The game crashed. Saving a crash report: {}, about {}:{:02} left...",
     "游戏崩溃了。正在保存崩溃报告：{}，大约还剩 {}:{:02}…"},
    {"The game crashed. Saving a crash report: {}...", "游戏崩溃了。正在保存崩溃报告：{}…"},
    {"The game draws its 3D scene twice, once per eye, even with 3D off. Only one picture is "
     "shown, so only the left eye's is drawn.",
     "即使关闭 3D，游戏也会为两只眼睛各绘制一次 3D 场景。由于只显示一幅画面，现在只绘制左眼的画面"
     "。"},
    {"The game hears that the GPU is done only after the time a 3DS's GPU would take. It can "
     "steady games whose speed follows their frame time, but games that wait for the GPU lose "
     "frames.",
     "游戏要等到 3DS 的 GPU 所需的时间过后，才会得知 GPU 已完成。这可以让按帧耗时调整速度的游戏更"
     "稳定，但等待 GPU 的游戏会掉帧。"},
    {"The game list also shows the titles in these folders, besides ux0:data/azahar/roms: on a "
     "USB or SD2Vita card (uma0:), for example. Only the files in the folder itself, not in "
     "folders inside it.",
     "除 ux0:data/azahar/roms 外，游戏列表也会显示这些文件夹中的游戏：例如 USB 或 SD2Vita "
     "存储卡（uma0:）上的文件夹。只读取文件夹本身中的文件，不包括其子文件夹。"},
    {"The game draws its text and menus one small square at a time. Squares drawn one after "
     "another are joined into one draw.",
     "游戏逐个绘制文字和菜单的小方块。现在把连续绘制的方块合并为一次绘制。"},
    {"The game file is revision {}.", "游戏文件为修订版 {}。"},
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
    {"The game's own screens show in the editor.", "编辑器中显示的是游戏自己的画面。"},
    {"The install stopped. Install the CIA again.", "安装已中断。请重新安装 CIA。"},
    {"The install stopped. Install the game again.", "安装已中断。请重新安装游戏。"},
    {"The language of Azahar's menus. A game keeps its own.",
     "Azahar 菜单使用的语言。不影响游戏内的语言。"},
    {"the MAC address is taken in that room", "该房间中已有相同的 MAC 地址"},
    {"the manual", "电子说明书"},
    {"The menus' colours: dark grey, or the light grey of the 3DS HOME Menu.",
     "菜单的颜色：深灰色，或 3DS HOME 菜单的浅灰色。"},
    {"the network failed", "网络出错"},
    {"the nickname is taken in that room", "该房间中昵称已被使用"},
    {"the room did not answer", "房间没有响应"},
    {"the room is full", "房间已满"},
    {"the room runs another version of the protocol", "房间使用的协议版本不同"},
    {"The programs Azahar made for this game's graphics, by every version of Azahar that ran it, "
     "kept so that the next start need not make them again. Deleting it frees their space; the "
     "game stutters for a while as it makes them again.",
     "Azahar 为此游戏的画面生成的程序（包括运行过此游戏的每个 Azahar 版本生成的），保存下来以免下次"
     "启动时重新生成。删除后可释放空间；游戏重新生成这些程序时会卡顿一段时间。"},
    {"The title's bubble is off the LiveArea.", "已从 LiveArea 移除此游戏的图标。"},
    {"the update data", "更新数据"},
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
    {"This file is encrypted: decrypt it, then install it.", "此文件已加密：请先解密，然后安装。"},
    {"This game does not use the default settings. Before you report the crash, set these back to "
     "their defaults and try again: {}",
     "此游戏未使用默认设置。报告崩溃之前，请将以下设置恢复为默认值后再试一次：{}"},
    {"This game file cannot be read. Make it again from your game.", "无法读取此游戏文件。请从你的游戏重新制作。"},
    {"This game file is gone.", "此游戏文件已不存在。"},
    {"Three command buffers", "三个命令缓冲区"},
    {"Tilting and turning the Vita moves the 3DS's motion sensors, for games that steer or aim "
     "with them. Its screen is the touch screen, held as you would hold the 3DS open. Off, the "
     "sensors stay still.",
     "倾斜和转动 Vita 即可带动 3DS 的体感传感器，供用体感转向或瞄准的游戏使用。Vita 的屏幕相当于触摸屏，"
     "握法与打开的 3DS 相同。关闭后传感器保持静止。"},
    {"Title {:016X}.", "游戏 ID {:016X}。"},
    {"To report it, send these files:", "如需报告，请发送以下文件："},
    {"To report it, send this file:", "如需报告，请发送此文件："},
    {"Tools", "工具"},
    {"Top screen", "上屏"},
    {"Traditional Chinese", "繁体中文"},
    {"Triangle", "△ 按钮"},
    {"Turn", "旋转"},
    {"unknown error", "未知错误"},
    {"unknown file type", "未知的文件类型"},
    {"Unpin", "取消固定"},
    {"unsupported file format", "不支持的文件格式"},
    {"Up", "方向键上"},
    {"Update", "更新数据"},
    {"USA", "美国"},
    {"USB video", "USB 视频"},
    {"Use all games' setting", "使用所有游戏的设置"},
    {"Version {}.", "版本 {}。"},
    {"Vsync", "垂直同步"},
    {"Wait for GPU results", "等待 GPU 结果"},
    {"What the right stick moves: the 3DS's C-stick (the New 3DS's, and the Circle Pad Pro's in "
     "games made for it), L and R (left and right, for games that turn the camera with them), or "
     "the d-pad.",
     "右摇杆的作用：3DS 的 C 摇杆（New 3DS 的，以及支持扩展滑杆的游戏中扩展滑杆的摇杆）、L 和 R"
     "（左右推动，用于以 L、R 转动视角的游戏）或十字键。"},
    {"What the game saved on the emulated SD card: its progress and its own options. Deleting it "
     "starts the game over as new. Its savestates and extra data stay.",
     "游戏保存在模拟 SD 卡上的数据：游戏进度和游戏自身的设置。删除后游戏将从头开始。即时存档和追加"
     "数据会保留。"},
    {"When the GPU falls behind, the busier screen skips frames so the game keeps its speed. This "
     "sets how many frames in a row it may skip: higher keeps heavy games faster but less smooth. "
     "Off draws every frame and the game slows down instead.",
     "GPU 跟不上时，较忙的屏幕会跳帧，以保持游戏速度。此项设置最多可连续跳过的帧数：越高，负担重的"
     "游戏越快，但越不流畅。关：绘制每一帧，游戏会变慢。"},
    {"Which Vita input presses each 3DS button. Buttons below: the rows below this one. Your "
     "own layouts, made in the control layout editor, have a mapping of their own and can put "
     "buttons on the screen, which a finger on the display presses.",
     "每个 3DS 按键由哪个 Vita 输入按下。下方的按键：此行下方的各行。你在按键布局编辑器中制作的布局有自己的映射，并可在屏幕上放置按钮，用手指在屏幕上按下。"},
    {"Widescreen", "宽屏"},
    {"Widescreen hack", "扩展宽屏"},
    {"Widescreen stretch", "拉伸宽屏"},
    {"Anti-aliasing", "抗锯齿"},
    {"Right eye render", "右眼渲染"},
    {"Top screen 2x anti-aliasing", "上屏 2 倍抗锯齿"},
    {"Top screen 4x anti-aliasing", "上屏 4 倍抗锯齿"},
    {"Skip bottom screen anti-aliasing", "跳过下屏抗锯齿"},
    {"Bottom screen 2x anti-aliasing", "下屏 2 倍抗锯齿"},
    {"Bottom screen 4x anti-aliasing", "下屏 4 倍抗锯齿"},
    {"Vertical sync", "垂直同步"},
    {"Triple buffer wait", "三重缓冲等待"},
    {"Skip frames when slow", "落后时跳帧"},
    {"Draws the 3D scene twice a frame and blends the two for smoother edges. It costs a lot "
     "of speed.",
     "每帧把 3D 场景绘制两次并混合，使边缘更平滑。会大幅降低速度。"},
    {"Draws the scene a second time for the right eye of the 3D effect. Off, the scene is drawn "
     "once, which is faster.",
     "为 3D 效果的右眼再绘制一次场景。关闭后场景只绘制一次，速度更快。"},
    {"Draws the top screen at twice its resolution and scales it down for smoother edges. "
     "Slower.",
     "以两倍分辨率绘制上屏再缩小，使边缘更平滑。速度更慢。"},
    {"Draws the top screen at four times its resolution and scales it down for smoother edges. "
     "Much slower.",
     "以四倍分辨率绘制上屏再缩小，使边缘更平滑。速度慢得多。"},
    {"Leaves out the bottom screen's anti-aliasing, which is faster.",
     "不对下屏进行抗锯齿，速度更快。"},
    {"Draws the bottom screen at twice its resolution and scales it down for smoother edges. "
     "Slower.",
     "以两倍分辨率绘制下屏再缩小，使边缘更平滑。速度更慢。"},
    {"Draws the bottom screen at four times its resolution and scales it down for smoother "
     "edges. Much slower.",
     "以四倍分辨率绘制下屏再缩小，使边缘更平滑。速度慢得多。"},
    {"The game waits for the screen to refresh before it shows a new frame.",
     "游戏在显示新的一帧之前等待屏幕刷新。"},
    {"The game waits for a frame to finish when it draws with three frame buffers.",
     "游戏使用三个帧缓冲绘制时等待一帧完成。"},
    {"The game leaves frames undrawn when it falls behind, to keep up its speed.",
     "游戏落后时不绘制部分帧，以保持速度。"},
    {"The game's own: on.", "游戏自身：开启。"},
    {"The game's own: off.", "游戏自身：关闭。"},
    {"Azahar turns it off unless you turn it on.", "除非你开启，否则 Azahar 会将其关闭。"},
    {"Azahar turns it on unless you turn it off.", "除非你关闭，否则 Azahar 会将其开启。"},
    {"The running game keeps what it started with: restart it to apply this.",
     "正在运行的游戏保持启动时的设置：重新启动游戏后生效。"},
    {"Applied when the game starts.", "游戏启动时应用。"},
    {"Frame rate limit", "帧率上限"},
    {"Game speed", "游戏速度"},
    {"Show fewer frames", "减少显示帧"},
    {"Game default", "游戏默认"},
    {"1 in 2", "每 2 帧 1 帧"},
    {"1 in 3", "每 3 帧 1 帧"},
    {"1 in 4", "每 4 帧 1 帧"},
    {"The most frames a second the game runs. At 60, a game made for 30 runs at 60 when the "
     "emulator keeps up, and slower when it can't.",
     "游戏每秒运行的最多帧数。设为 60 时，为 30 帧设计的游戏在模拟器跟得上时以 60 帧运行，跟不上时会变慢。"},
    {"How fast the game's time runs: 1.5x and 2x make everything move faster, 0.5x slower.",
     "游戏时间流逝的快慢：1.5 倍和 2 倍让一切更快，0.5 倍更慢。"},
    {"The game shows only one of every two, three or four frames it makes, which can let it "
     "run faster.",
     "游戏在它生成的每两、三或四帧中只显示一帧，可能让它运行得更快。"},
    {"Bloom", "泛光"},
    {"Depth of field", "景深"},
    {"Bokeh", "散景"},
    {"Colour correction", "色彩校正"},
    {"Radial blur", "径向模糊"},
    {"Motion blur", "动态模糊"},
    {"Outlines", "轮廓线"},
    {"Heat haze", "热浪扭曲"},
    {"Screen noise", "画面噪点"},
    {"The glow around bright lights and surfaces.", "明亮光源和表面周围的光晕。"},
    {"The blur on what is far from where the camera focuses.", "远离镜头焦点之处的模糊。"},
    {"The shapes of out-of-focus lights in the depth of field blur.", "景深模糊中失焦光点的形状。"},
    {"The game's colour grading: its tones, brightness and contrast.",
     "游戏的调色：色调、亮度与对比度。"},
    {"The blur streaking out from a point, for impacts and roars.",
     "从一点向外拉伸的模糊，用于冲击和咆哮。"},
    {"The blur of the scene when the camera moves fast.", "镜头快速移动时场景的模糊。"},
    {"The outlines drawn around characters and objects.", "角色和物体周围绘制的轮廓线。"},
    {"The shimmer over heat and fire.", "热源和火焰上方的扭曲闪动。"},
    {"The noise and lines laid over the screen in some scenes.", "某些场景中叠加在画面上的噪点和线条。"},
    {"Game default: the game sets it itself.", "游戏默认：由游戏自己设定。"},
    {"Off, the game skips drawing this effect, which is faster.", "关闭后游戏不绘制此效果，速度更快。"},
    {"wrong password", "密码错误"},
    {"You can turn this screen off in Settings > System > Startup notice.",
     "可在“设置 > 系统 > 启动提示”中关闭此画面。"},
    {"Your kernel plugins leave too little memory for Azahar. Remove some lines under *KERNEL in "
     "your taiHEN config.txt (ux0:tai/config.txt or ur0:tai/config.txt), then reboot your console.",
     "内核插件占用的内存过多，Azahar 无法启动。请在 taiHEN 的 config.txt（ux0:tai/config.txt 或 "
     "ur0:tai/config.txt）中删除 *KERNEL 下的部分插件行，然后重启主机。"},
    {"{} (hidden)", "{}（已隐藏）"},
    {"{} can't be checked: its file type carries no checksums.",
     "无法检查 {}：这种文件类型不带校验值。"},
    {"{} can't be read as a 3DS game.", "无法将 {} 作为 3DS 游戏读取。"},
    {"{} could not be loaded ({:#010x}). Reinstall the vpk.",
     "无法加载 {}（{:#010x}）。请重新安装 vpk。"},
    {"{} files or folders could not be deleted. Restart the Vita, then delete them again.",
     "有 {} 个文件或文件夹无法删除。请重启 Vita，然后再删除一次。"},
    {"{} in {} files.", "{}，共 {} 个文件。"},
    {"{} is damaged in {}. Copy it again from your game.", "{} 的{}已损坏。请从你的游戏重新复制。"},
    {"{} is encrypted, so it can't be checked. Decrypt it first.",
     "{} 已加密，无法检查。请先解密。"},
    {"{} is incomplete: the file ends early. Copy it again from your game.",
     "{} 不完整：文件提前结束。请从你的游戏重新复制。"},
    {"{} is installed.", "{} 已安装。"},
    {"{} is intact: every part matches its checksum.", "{} 完好：每个部分都与校验值一致。"},
    {"{} might be corrupted: the file ends early. It can crash or misbehave. Launch it anyway?",
     "{} 可能已损坏：文件提前结束。运行时可能崩溃或出错。仍要启动吗？"},
    {"{} might be corrupted: {} doesn't match its checksum. It can crash or misbehave. Launch it "
     "anyway?",
     "{} 可能已损坏：{}与校验值不符。运行时可能崩溃或出错。仍要启动吗？"},
    {"{} sets its own: {}. Change it in Game settings.",
     "{} 有单独的设置：{}。可在“游戏设置”中更改。"},
    {"{} then uses it.", "之后 {} 将使用它。"},
    {"{} again to remove", "再按 {} 删除"},
    {"{} title", "{} 个游戏"},
    {"{} to {}, in Game settings", "{}设为{}（游戏设置）"},
    {"{} to {}, in Settings", "{}设为{}（设置）"},
    {"{}'s save data is deleted.", "{} 的存档已删除。"},
    {"{}'s shader cache is deleted.", "{} 的着色器缓存已删除。"},
    {"{} titles", "{} 个游戏"},
    {"{}: its downloadable content is installed.", "{}：追加内容已安装。"},
    {"{}: its update is installed.", "{}：更新数据已安装。"},
    {"{}: {} pixels across at {}, {}", "{}：直径 {} 像素，位于 {}, {}"},
    {"{}: {}x{} at {}, {}, turned {} degrees, {}% opaque{}",
     "{}：{}x{}，位于 {}, {}，旋转 {} 度，不透明度 {}%{}"},
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

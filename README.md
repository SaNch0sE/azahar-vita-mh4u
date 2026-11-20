![Azahar Emulator](https://azahar-emu.org/resources/images/logo/azahar-name-and-logo.svg)

# Monster Hunter 4 Ultimate for the PS Vita

A port of the [Azahar](https://github.com/azahar-emu/azahar) 3DS emulator, built on
[Grarak's PS Vita port](https://github.com/Grarak/azahar) and tuned for one game: **Monster Hunter 4
Ultimate**. It installs as its own app (title ID `MH4UVITA0`), keeps everything in `ux0:data/mh4u`,
and is played on one screen with the [Heavenfall mod](https://github.com/popzol/4u-heavenfall-mod-beta),
which moves the touch screen's menus to the top screen.

Status: **alpha**. Hunts, the village and the menus run at or near 30 fps; the quest counter still
drops below that. Expect bugs, and back up your saves.

This repository contains no game files. You need your own copy of the game.

## What you need

| What | Where it comes from |
| --- | --- |
| A PS Vita or PS TV with [HENkaku/Ensō](https://vita.hacks.guide/) (taiHEN) and VitaShell | the Vita hacks guide |
| `MH4U.vpk` and `mh4unative.skprx` | this repository's [releases](https://github.com/SaNch0sE/azahar-vita-mh4u/releases) |
| `CapUnlocker.skprx` | [GrapheneCt/CapUnlocker releases](https://github.com/GrapheneCt/CapUnlocker/releases) |
| Monster Hunter 4 Ultimate, **USA**, decrypted (`.3ds`, `.cci` or `.cxi`) | dumped from your own cartridge or 3DS ([how to dump](https://3ds.hacks.guide/dumping-titles-and-game-cartridges.html)); decrypted, for example with GodMode9 |
| Recommended: the game's **v1.3 (v1.1)** update, decrypted `.cia` | dumped from your own 3DS (see step 2) |
| The Heavenfall mod, made from your own game | the [web installer](https://popzol.github.io/4u-heavenfall-mod-beta/) (below) |

Only the American release (title `0004000000126300`) has been tested. The European one may run, but
the port's speedups and cheat file are made for the American game.

## Install

### 1. The app and the kernel plugins

1. Copy `MH4U.vpk` to the Vita and install it with VitaShell.
2. Copy `mh4unative.skprx` and `CapUnlocker.skprx` to `ur0:tai/`.
3. Add both under the `*KERNEL` section of `ur0:tai/config.txt` (if your setup uses
   `ux0:tai/config.txt` instead, edit that one):

   ```
   *KERNEL
   ur0:tai/mh4unative.skprx
   ur0:tai/CapUnlocker.skprx
   ```

4. Reboot the Vita.

`mh4unative.skprx` lets the Vita run the game's ARM code directly. It does nothing until the app
calls it. The app refuses to start, with a message, if either plugin is missing or too old.

### 2. The game

1. Create the folder `ux0:data/mh4u/roms/`. The app also creates it on its first start.
2. Put your decrypted game (`.3ds`, `.cci` or `.cxi`) in it.
3. **Recommended: the USA game with the v1.3 (v1.1) update.** Put the decrypted update `.cia`
   in the same folder; on its next start the app installs it, with a progress bar, and after
   that you can delete the `.cia`. (A mod made with the web installer from the base game brings
   its own copy of the game's program and replaces the update's while the mod is active, so the
   game also runs without the update.)
   (Any other `.cia` in that folder, such as DLC, is installed the same way.)

### 3. The Heavenfall mod

The port is meant to be played with the mod's single-screen mode.

1. On a PC or phone, open the web installer: **https://popzol.github.io/4u-heavenfall-mod-beta/**.
   Your game is never uploaded; everything runs in your browser.
2. Choose your **base game** file: the same decrypted `.3ds` (cartridge or eShop dump) that is
   in `roms/`. Not the update `.cia`: the installer refuses it ("this is the game update, not the
   game itself").
3. Options:
   - **Single Screen mode**: on (the default).
   - **HUD size**: **80%**. The port draws the HUD, text and menus sharper than the 3DS does
     (the app's **HUD resolution** setting, 1.33x by default), so 80% stays readable on the
     Vita's screen and leaves more of the picture free. 90% or 100% if you prefer it bigger.
   - **Aspect ratio**: tick **Allow other aspect ratios (EXPERIMENTAL)** and choose **16:9**, so
     the game fills the Vita's screen with correct proportions (the app's **Picture shape**
     setting is Wide by default to match). This is the setup we play and measure (16:9). 5:3, the default,
     works too and keeps the 3DS shape.
   - Keep the **other / quality-of-life module on**. The port needs it: without it the game
     crashes about 15 seconds after boot.
   - Layered Gear and the mod's cheats ("Enable cheats"): your choice. The port doesn't need
     them.
4. Press **Make the mod**, download the zip, and extract it. Inside is a folder named
   `0004000000126300`.
5. Copy that folder to `ux0:data/mh4u/load/mods/`, so that you end up with
   `ux0:data/mh4u/load/mods/0004000000126300/romfs` and `.../exefs`. To save space, you
   only need `romfs/system` and the language folder you play in (`romfs/eng` for English).

To update the mod or change its options, delete that folder and copy the new one in. Files
left behind would still be loaded.

The mod is also a Python program (`python3 -m core --rom <game> --code <update.cia> --out
<folder> --single-screen --quality-of-life ...`, see
[docs/vita-mh4u-single-screen.md](./docs/vita-mh4u-single-screen.md)), which can build it from
the update's executable too. The port runs either build: the web installer's build was tested
from a fresh `ux0:data/mh4u`, and the update-based build is the one played most.

### 4. First start

Start **Monster Hunter 4 Ultimate** from the LiveArea. In the game, then:

- The game is capped at 30 fps from the first start by the port's cheat file (the **30 FPS Cap**
  code, on by default). Without a cap the game tries to run at 60, which the Vita can't keep up.
- Once you reach the START menu (after the prologue): third page, **Mod Settings**, **Display**.
  This is kept in your save:
  - **Aspect ratio**: 16:9, if you made the mod with it.
  - **FPS: 30** (recommended, optional). Normally it's the same as the cheat, but if the GPU
    falls behind, the mod's cap keeps the game at full speed and only skips frames, where the
    cheat lets the game itself slow down. Both caps on together is fine.
  - 3D Mode off with no dithering, and Bloom and Depth Blur off. These are the web installer's
    defaults; check them if you built the mod another way.
- **Select + Start** opens the port's pause menu. Under **Settings**:
  - **Picture shape**: **3DS (5:3)** keeps the game's proportions, with bars at the sides.
    **Wide (16:9)** fills the screen: with a 5:3 mod it stretches the picture sideways, and with
    a 16:9 mod the proportions are correct.

## The port's files: `ux0:data/mh4u`

| Path | What |
| --- | --- |
| `roms/` | your game (and the update `.cia`, if you use one, until it is installed) |
| `load/mods/0004000000126300/` | the Heavenfall mod |
| `cheats/0004000000126300.txt` | the cheat file, copied from the app on first start: **30 FPS Cap** on (it works with a mod built from the base game, like the web installer's, and with one built from the update), and 3D, dithering and bloom codes off (the mod does them). Your own edits are kept. |
| `sdmc/`, `nand/`, `sysdata/` | the game's saves, the installed update, system data |
| `states/` | save states (pause menu, **Save states**, slots 1-9) |
| `video.txt` | the pause menu's picture settings |
| `reports/` | bug reports (pause menu, **Bug report**); the newest 3 are kept |

## Pause menu (Select + Start)

- **Save states**: nine slots, each with Load and Save.
- **Settings**: screen layout, upscale filter (Linear is the fastest; Sharp bilinear costs about
  1 fps), FXAA, FPS counter (the speed and frame rate in the top right corner, on by default),
  picture shape, render scale (above 1x is much slower, and applies on
  the next start).
- **Bug report**: saves one zip for reporting a problem (see below).
- **Exit game**.

## Reporting a problem

Right after the problem happens, open the pause menu and choose **Bug report**:

- **Save report** (about 1 MB) writes `ux0:data/mh4u/reports/mh4u_report_<date>_<time>.zip` with
  the app and game versions, your mod folder's file list, your settings and cheat file, the game's
  speed over the last two minutes, your in-game save and extra data, and the newest crash dump.
- **Save report with the newest save state** adds your most recent save state (about 30 MB), for
  problems that only show at one moment: save a state there first, then make the report.

Copy the zip to a PC (VitaShell's FTP or USB) and fill in the
[bug report form](https://github.com/SaNch0sE/azahar-vita-mh4u/issues/new?template=bug_report.yml):
what happened, and either which save state to load and what to do after loading it, or the steps
in the game from your save. Attach the zip there. GitHub takes files up
to 25 MB; a report with a save state is larger, so share that one through a cloud drive link.

What you share: your hunter's name, guild cards and progress (in the save). A save state also
holds the game's program as it was in memory. No game files are included otherwise.

The app keeps no log file while you play: the two minutes of speed lines live in memory and are
written only into a report. The newest 3 reports are kept; making a new one deletes older ones.

## Known issues

- The quest counter runs below 30 fps.
- The first steps into an area you have not seen yet can stutter once while shaders are made. They
  are saved to `programs.bin` and load quickly after that.
- After moving to another area, the frame rate can dip for a few seconds before it settles at 30.
  It is worst the first time you visit an area after installing a new version: the shaders saved
  in `ux0:data/mh4u/programs.bin` are rebuilt for each new build, so the first visits make them
  again. Later visits are faster.
- A save state carries the mod's code as it was when the state was saved: after changing the mod,
  load your in-game save once instead of an older state.

## Build

Needs Linux, [VitaSDK](https://vitasdk.org), clang/lld (21) and CMake with Ninja:

```sh
git clone --recursive https://github.com/SaNch0sE/azahar-vita-mh4u
cd azahar-vita-mh4u
git -C externals/libressl apply ../../patches/vita/libressl-vita.patch   # once, see patches/vita
export VITASDK=/usr/local/vitasdk
cmake -S . -B build-vita-rel -GNinja \
  -DCMAKE_TOOLCHAIN_FILE=CMakeModules/toolchain-vita-clang.cmake \
  -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=ON
cmake --build build-vita-rel
```

- The app: `build-vita-rel/src/citra_vita/citra_vita.vpk`.
- The kernel plugin: `build-vita-rel/vita_native/mh4unative.skprx`.

`-DENABLE_LTO=OFF` gives the diagnostic build: logging, statistics and the switches in
`src/video_core/renderer_gxm/gxm_flags.h`. [DEVELOPMENT.md](./DEVELOPMENT.md) explains how the port
works, and [docs/vita-mh4u-performance.md](./docs/vita-mh4u-performance.md) records what was
measured and changed.

## Projects we learned from

What each one gave this port, and in what form:

- [Azahar](https://github.com/azahar-emu/azahar): the emulator itself (code base).
- [Grarak/azahar](https://github.com/Grarak/azahar): the PS Vita port this fork is built on: native execution through the `azaharnative` kernel plugin, the USSE shader emitters, the GXM renderer and the frontend (code base).
- [Grarak/mh3-wii](https://github.com/Grarak/mh3-wii): the choice of render resolutions offered in its start menu (idea for the render scale option).
- [TuizaVita](https://github.com/TutuiFC/TuizaVita-3ds-emu): clamping the time-stretched audio before conversion and the sleeping-wait wrap for pthreads-embedded (small pieces of code); mipmapped textures and a render scale below 1x (ideas); its worker-thread file reads led us to the real cause of our area-crossing stutter (a mod file opened per read).
- [DSVita](https://github.com/Grarak/DSVita): the shape of the audio output (48 kHz port, buffer size), the pause menu's structure, and mirrored addressing on linear textures (reference design).
- [vitaGL](https://github.com/Rinnegatamante/vitaGL): suballocating one GPU memory block, and loading the Razor GPU capture and live metrics (approach).
- [Vita3K](https://github.com/Vita3K/Vita3K): running and debugging the port off the console (tool).
- Programs from Sony's `psp2cgc` and the `psp2shaderperf` report: reference output for instruction encodings and scheduling bits that the USSE emitters reproduce (reference).
- [kubridge](https://github.com/bythos14/kubridge): the logging approach in the kernel plugin (idea).
- FXAA (Timothy Lottes) and the sharp-bilinear upscale (libretro shaders): the algorithms behind the image options, rewritten as USSE programs (algorithms).

## Credits
- The Azahar Team
- vitaGL
- Vita3K
- @TheIronUniverse for livearea assets

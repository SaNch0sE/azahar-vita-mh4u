# MH4U for PS Vita 0.3.1-alpha

Monster Hunter 4 Ultimate on the PS Vita, through a port of the Azahar 3DS emulator tuned for this
one game. Hunts, the village and menus run at 30 fps in the places we test; the quest counter is
still slower. Alpha: back up your saves.

No game files are included: you need your own decrypted copy of the game (USA).

## Downloads

- `MH4U.vpk`: the app (title ID `MH4UVITA0`).
- `mh4unative.skprx`: the kernel plugin it needs.
- Also needed, not ours: `CapUnlocker.skprx` from https://github.com/GrapheneCt/CapUnlocker/releases

## Install

Updating from 0.3 or 0.2: install the new `MH4U.vpk` over the old one. Your saves and settings in
`ux0:data/mh4u` stay. The kernel plugin is unchanged.

1. **The app.** Copy `MH4U.vpk` to the Vita and install it with VitaShell.
2. **The kernel plugins.** Copy `mh4unative.skprx` and `CapUnlocker.skprx` to `ur0:tai/` and add
   them to the `*KERNEL` section of `ur0:tai/config.txt` (or `ux0:tai/config.txt` if that is the
   one your console uses), then reboot:
   ```
   *KERNEL
   ur0:tai/mh4unative.skprx
   ur0:tai/CapUnlocker.skprx
   ```
3. **The game.** Create `ux0:data/mh4u/roms/` and put your decrypted Monster Hunter 4 Ultimate (USA)
   `.3ds` (or `.cci`/`.cxi`) in it. You don't need the game's v1.1 update with a mod from the web
   installer: that mod brings its own copy of the game's program, built from the base game, which
   replaces the update's. (If you build the mod yourself from the update with Heavenfall's
   command-line tool, put the decrypted update `.cia` in the same folder too; the app installs it
   on its next start.)
4. **The Heavenfall mod** (menus on one screen; the port is made to be played with it):
   1. Open the web installer, https://popzol.github.io/4u-heavenfall-mod-beta/, on a PC or phone.
      Your game is never uploaded.
   2. Choose your **base game** file: the same decrypted `.3ds` (cartridge or eShop dump) you put
      in `roms/`. Not the update `.cia`: the installer refuses it.
   3. Options:
      - **Single Screen mode**: on.
      - **HUD size**: **100%** (or 90%). The default 70% is hard to read on the Vita.
      - **Aspect ratio**: tick **Allow other aspect ratios (EXPERIMENTAL)** and choose **16:9**.
        That's the setup we play and measure. 5:3 works too.
      - Keep the quality-of-life ("other") module **on**: the port needs it.
      - Layered Gear and the mod's cheats: your choice.
   4. Press **Make the mod**, download the zip and extract it. You get a folder named
      `0004000000126300`.
   5. Copy that folder to `ux0:data/mh4u/load/mods/`, so that you end up with
      `ux0:data/mh4u/load/mods/0004000000126300/romfs`. You only need `romfs/system` and your
      language's folder (`romfs/eng` for English). To change the mod later, delete that folder
      first and copy the new one in.
5. **Start the game.** It is capped at 30 fps from the first start by the port's cheat file
   (**30 FPS Cap**, on by default); without a cap the game would try to run at 60 and slow down.
   Once you reach the START menu (after the prologue), open its third page, **Mod Settings**,
   **Display** (kept in your save):
   - Aspect ratio **16:9**, if you made the mod with it.
   - **FPS: 30** - recommended. It's the same as the cheat in normal play, but when the GPU falls
     behind it keeps the game at full speed instead of letting it slow down.
   - 3D Mode off with no dithering, and Bloom and Depth Blur off (the web installer's defaults).

   In the port's pause menu (**Select + Start**), **Settings**: set **Picture shape** to
   **Wide (16:9)** for a 16:9 mod.

Your saves, save states and settings are in `ux0:data/mh4u`.

## What's new since 0.3

- **Grassy areas at 30 fps.** Areas with a lot of grass and plants could drop to 8-11 fps (worst on
  the first load of a save state there). The plants' geometry is now built on the Vita's GPU
  instead of its CPU, and drawn with a depth pre-pass so the GPU doesn't shade hidden leaves: the
  same spot now runs at 29.7 fps, with the same picture.
- **Faster lighting**, about 1.9 ms less GPU time a frame in a busy area, with the same picture.
- **Crash fix in the pause menu.** Opening the menu while a direction was still held could close
  the app.
- **Intro movie after Quit Game.** After quitting from the game's menu (especially after loading
  a save state), the intro played slowly and had a line of noise along its bottom edge. Both are
  fixed.

What came in 0.3 is listed in its release notes.

## Known issues

- The quest counter runs below 30 fps.
- After moving to another area, the frame rate can dip for a few seconds before it settles at 30.
  It is worst the first time you visit an area after installing a new version: the shaders saved
  in `ux0:data/mh4u/programs.bin` are rebuilt for each new build, so the first visits make them
  again. Later visits are faster.
- A save state carries the mod's code from when it was saved: after changing the mod, load your
  in-game save once instead of an older state.

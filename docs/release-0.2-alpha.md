# MH4U for PS Vita 0.2-alpha

Monster Hunter 4 Ultimate on the PS Vita, through a port of the Azahar 3DS emulator tuned for this one
game. Hunts, the village and menus run at or near 30 fps; the quest counter is still slower. Alpha:
back up your saves.

No game files are included: you need your own decrypted copy of the game (USA).

## Downloads

- `MH4U.vpk`: the app (title ID `MH4UVITA0`).
- `mh4unative.skprx`: the kernel plugin it needs.
- Also needed, not ours: `CapUnlocker.skprx` from https://github.com/GrapheneCt/CapUnlocker/releases

## Install

1. **The app.** Copy `MH4U.vpk` to the Vita and install it with VitaShell.
2. **The kernel plugins.** Copy `mh4unative.skprx` and `CapUnlocker.skprx` to `ur0:tai/` and add
   them to the `*KERNEL` section of `ur0:tai/config.txt` (or `ux0:tai/config.txt` if that is the
   one your console uses):
   ```
   *KERNEL
   ur0:tai/mh4unative.skprx
   ur0:tai/CapUnlocker.skprx
   ```
   Reboot.
3. **The game.** Create `ux0:data/mh4u/roms/` and put your decrypted Monster Hunter 4 Ultimate (USA)
   `.3ds` (or `.cci`/`.cxi`) in it. Optional: also put the decrypted v1.1 update `.cia` there. The app
   installs the update on its next start; after that you can delete the update's `.cia`. (Any
   other `.cia` in that folder, such as DLC, is installed the same way.)
4. **The Heavenfall mod** (single-screen menus; the port is made to be played with it):
   1. Open the web installer, https://popzol.github.io/4u-heavenfall-mod-beta/, on a PC or phone.
      Your game is never uploaded.
   2. Choose your `.3ds`.
   3. Options: keep **Single Screen mode** on, and keep the quality-of-life ("other") module on (the
      port needs it). Pick the HUD size you like. Leave the aspect ratio at **5:3** (the default).
      The 16:9 option (**Allow other aspect ratios (EXPERIMENTAL)**) is playable, but not
      recommended: heavy scenes drop to 28-30 fps where 5:3 holds a steady 30.
   4. Press **Make the mod**, download the zip and extract it. You get a folder named
      `0004000000126300`.
   5. Copy that folder to `ux0:data/mh4u/load/mods/`, so that you end up with
      `ux0:data/mh4u/load/mods/0004000000126300/romfs`. You only need `romfs/system` and your
      language's folder (`romfs/eng` for English).
5. **Start the game.** In the game, open the START menu, third page, **Mod Settings**, **Display**:
   set **FPS** to **30** (a web-installer mod starts with it off, and the game then tries to run at
   60), 3D Mode off with no dithering, and Bloom and Depth Blur off. If you made the mod with 16:9,
   set the aspect ratio to 16:9 there too. In the port's pause menu (**Select + Start**),
   **Settings**: **Picture shape** is **3DS (5:3)** (correct proportions, bars at the sides) or
   **Wide (16:9)** (fills the screen; stretched with a 5:3 mod, correct with a 16:9 one).
   **Sharp HUD text** in the same menu makes names and item names much easier to read.

Your saves, save states and settings are in `ux0:data/mh4u`.

## What's new since 0.1

- Runs as its own app, with its own data folder `ux0:data/mh4u`.
- Single-screen play with the Heavenfall mod; the hidden bottom screen is not drawn.
- Mostly 30 fps in the village and in hunts: GPU and render-thread work, a native path for the
  game's audio filter, cached shader programs.
- Correct water, lighting brightness, shadows and fresnel.
- Pause menu: save states (9 slots), upscale filter, FXAA, render scale, wide picture, and
  **Sharp HUD text**.
- A cheat file (3D and dithering off, bloom off, 30 fps cap) ships in the app and is set up on
  first start. Its codes only act on a mod built from the v1.1 update's executable.
- The game's audio filter runs natively on both the base game (v1.0) and the v1.1 update.

## Known issues

- The quest counter runs below 30 fps.
- An area seen for the first time can stutter briefly while shaders are made. They are cached
  after that.

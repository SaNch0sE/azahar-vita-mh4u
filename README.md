![Azahar Emulator](https://azahar-emu.org/resources/images/logo/azahar-name-and-logo.svg)

# Azahar Emulator for the PSVita

This is an early, almost entirely vibecoded port of Azahar for the PSVita, so do not expect playability or support from my side.

## Install

- Grab the latest vpk and skprx from [releases](https://github.com/Grarak/azahar/releases)
- Grab latest release of [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker/releases)
- Install the vpk
- Install both skprx files (azaharnative and CapUnlocker) to the KERNEL section in your config.txt
- Put your **decrypted** ROMs in ux0:/data/azahar/roms
- Set your CPU to 500MHz before launching a game

## Known Issues
- Pausing the game and resuming might freeze your console, you need a hardreset
- Savestates are untested and probably don't work
- Expect crashes

## Build
See [DEVELOPMENT.md](./DEVELOPMENT.md)

## This fork

This branch tunes the port for Monster Hunter 4 Ultimate: it installs as its own app (MH4UVITA0) and keeps its files in `ux0:data/mh4u`. Performance notes and plans are in [docs/vita-mh4u-performance.md](./docs/vita-mh4u-performance.md) and [docs/plans](./docs/plans). The pause menu has image options: an upscale filter (linear or sharp bilinear), FXAA, and an internal render scale (1x to 2x, applied on the next launch).

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

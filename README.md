![Azahar Emulator](https://azahar-emu.org/resources/images/logo/azahar-name-and-logo.svg)

# Azahar Emulator for the PS Vita

A port of [Azahar](https://github.com/azahar-emu/azahar), the Nintendo 3DS emulator, to the PlayStation Vita.

## Installation

1. You need a PS Vita on firmware **3.60, 3.65, 3.68 or 3.73**.
2. Install the vpk from the [releases](https://github.com/Grarak/azahar/releases) with VitaShell. If you installed `azaharnative.skprx` for the 0.1 release, remove it: the vpk ships its own.
3. Copy your decrypted 3DS games (`.3ds`, `.cci`, `.cxi`, `.3dsx`, `.cia`, or their compressed `.zcci`, `.zcxi`, `.z3dsx`, `.zcia` forms) to `ux0:data/azahar/roms/`.
4. Set your CPU to 500MHz before launching a game.

## Reporting a crash or bug

Open an [issue](https://github.com/Grarak/azahar/issues) with:

- the game, and where in it the problem happens;
- the azahar version and your firmware;
- after a crash, the file azahar's crash notice names (`ux0:data/azahar/crashes/crash-….zip`). If the system's error dialog appeared instead, attach the newest `ux0:data/psp2core-….psp2dmp` and, if it exists, `ux0:data/azahar/crash-last.txt`.

If the notice says the game ran with changed settings, try it again with their defaults first.

## Documentation

- [Building](docs/building.md)
- [Native execution](docs/native-execution.md): how the 3DS's code runs on the Vita's CPU
- [Threads and cores](docs/threads.md)
- [sgxd](docs/sgxd/README.md): how azahar drives the Vita's GPU
- [Debugging and profiling](docs/debugging.md)

## Credits

- The Azahar Team
- vitaGL
- Vita3K
- @TheIronUniverse for LiveArea assets

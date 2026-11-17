# Monster Hunter 4 Ultimate on the PS Vita: single-screen play

The port starts in the **Single screen** layout (the top screen fills the display). It is meant to be
played with the Heavenfall mod's single-screen mode (https://github.com/popzol/4u-heavenfall-mod-beta),
which moves the touch screen's menus to the top screen. The pause menu's *Screen layout* switches back.
In the Single screen layout the bottom screen's draws are not shipped at all (`nobottomskip` in
`gxm_flags.txt` keeps them).

## Building the mod

The mod is built on a PC from your own decrypted game and, if the update is installed in the port, your
own decrypted update (the mod patches the executable the port runs):

```sh
git clone https://github.com/popzol/4u-heavenfall-mod-beta && cd 4u-heavenfall-mod-beta
python3 -m core --rom "<game>.3ds" --code "<update>.cia" --out <folder> \
    --single-screen --no-layered-equipment --no-rare-exchange --quality-of-life
```

- `--code` is the update (v1.1, title 0004000E00126300) when it is installed under
  `ux0:data/azahar/sdmc/title/0004000e/00126300`; leave it out otherwise.
- Keep `--quality-of-life` on. With it off, the mod's `.data` ends before the region that holds its
  variables (`ext_vars`, 0x130A000-0x130D000), which the in-game Mod Settings page writes anyway: the
  desktop emulators drop writes to unmapped memory, the port maps guest memory natively and stops with
  a data abort ~15 s after boot. Its options (holding A, fast text, the item boxes...) can be turned
  off in the game's START menu, Mod Settings page; its 3D and bloom cheats match the port's cheat file.
- On Python 3.15 the `zstandard` wheel does not build; it is only needed for `.zcci`/`.zcia` input, so
  an empty `zstandard.py` on `PYTHONPATH` is enough for a `.3ds`/`.cia`.
- The output folder holds files made from your game; do not share it.

## Installing it

Copy into `ux0:data/azahar/load/mods/0004000000126300/` (USA; 0004000000126100 for Europe):

- `exheader.bin`, `exefs/code.bin`
- `romfs/system/` and the language folder the game runs in (`romfs/eng/` in English: ~200 files,
  11 MB; the other four languages are only needed if the game's language is changed).

The console log then shows `LayeredFS replacement file in use for /eng/data/...` and
`exefs/code.bin overriding built-in ExeFS file`.

## Wide picture (16:9)

User playtest, 2026-11-17 (0.2): with the 16:9 build, heavy scenes ran at 28-30 fps where the 5:3
build held a steady 30. Since the reflection fix (0.3: lit programs take the power form) the
16:9 build with HUD 100% holds 30 in the hunting area that was 27-28, so it is the recommended
setup again.

The mod's experimental aspect option keeps the game's 400x240 frame but widens its 3D views and
spreads the interface, for an emulator that stretches the picture to the screen's shape. Build it
with `--cheat-aspect-ratio 16:9` (ratios: 4:3, 16:10, 16:9, 20:9, 21:9, 32:9), install it as
above after removing the old mod folder, choose 16:9 in the game's START menu, third page, Mod
Settings, and set the port's *Picture shape* (Settings) to **Wide (16:9)** - `wide=1` in
`ux0:data/mh4u/video.txt` - which stretches the top screen over the whole 960x544 display
(Settings::AspectRatio::Stretch). The sharp upscale filter scales each axis separately.

## Touch

In the Single screen layout touches do not reach the game: the hidden bottom screen would
otherwise take them (invisible Hunter Search, Gathering Hall). The mod puts every menu on the
top screen and expects the touch screen only while the bottom screen is visible: layouts that
show it (or Swap screens) keep touch.

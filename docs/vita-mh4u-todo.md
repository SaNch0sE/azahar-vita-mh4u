# Monster Hunter 4 Ultimate on the PS Vita: TODO

The port is game-specific: an optimisation may break behaviour other titles rely on.
Measurements and history are in `vita-mh4u-performance.md`.

## Performance (in order)

1. **Per-vertex lighting for the lit world.** One fragment program (`25bbe5f060`: lighting
   config 1, one directional light) is ~46% of the GPU's fragment time on the ship's deck;
   106 of its 199 instructions are lighting. The deck has ~2 pixels per vertex, so lighting
   inputs computed per vertex and interpolated should look the same. Now headroom rather
   than a necessity: the deck is ~32.9 ms of GPU a frame with item 2.
2. Done, then replaced (2026-11-16): the reflection LUTs taken as 1 made the water white
   (slot 2). Now the rasterizer fits the reflection-red LUT on upload and the program
   computes |N.H|^n (MH4U's tables are exactly n = 16 and n = 40) - exact and fetch-free;
   see "Water and highlights" in vita-mh4u-performance.md.
3. **Hardware-filtered lighting LUTs** with one wait for all lookups (check the console can
   filter the chosen format; signed tables need care).
4. **F16 colour pipeline** in the fragment emitter (TEV, texels, lighting sums): the largest
   remaining lever, and the riskiest (known four-wide miscompiles on the console).
5. **Render thread:** the ~18% of geometry-shader glyph draws that change state pay a full
   draw setup (~150 us); a partial update would help character select most.
6. Done: the bloom / depth-of-field chain and the shadow-target clear are dropped by default
   (`postfx`, `shadowtarget` restore them), and colour-only framebuffers draw in the open
   scene (`noscenemerge`). Deck GPU ~37.8 -> ~35.5 ms a frame, shown fps 28-29.

9. Done: the vertex cache (hardware draws reuse the GPU copy of unchanged vertex data). Watch
   for stale geometry on area loads; `novtxcache` turns it off.
10. Next on the emulation thread (`emuprof`): command-list parsing outside draws (~90 ms/s),
    glyph draws (~60 ms/s), the DSP tick (~65 ms/s; mixing could split across cores by sample).

## After the optimisations

7. Done: **single-screen play** with the Heavenfall mod (`vita-mh4u-single-screen.md`); the
   port starts in the Single screen layout.
8. Done: in the single-screen layout the bottom screen's draws (240x320 viewport) are not shipped
   (`nobottomskip` keeps them): ~135 draws a frame on the deck; deck speed with the mod 82-86% ->
   90-93% (27-28 fps).

## Testing and compatibility

- **Long sessions (heap fragmentation).** Fixed in two batches (BigArena for large short-lived
  buffers; GS memo scratch, shared JIT label table, emergency reserve; performance notes,
  2026-11-18). Still to do: a 90+ minute session with `heapprof` to confirm (pass: `arena - used`
  within a few MB, largest free chunk in MiB, bigarena without fallbacks); if in-use heap still
  grows ~6 MB an hour, cap the long-lived caches using the `heapgrow` list.
- **Multiplayer: local and online.** Test local play (two Vitas, or a Vita and a PC running
  Azahar) through Azahar's network rooms, and online play through a room server: does it
  connect, does the frame rate hold, do quests stay in sync. Port the UDS simultaneous-join fix
  first (decomp ideas, O2).
- **EUR game (v1.1.0 update, 0004000E00126100).** Branch `eur-support`, untested (no EUR dump):
  the movie poll fix at +0x38 (from MH4UltimateDC's address map), an EUR cheat file, a
  region-aware bug report. Needs a tester with the European game; then merge.
- **Ideas from MH4UltimateDC** (`vita-mh4u-decomp-ideas.md`). Planned
  (`plans/vita-mh4u-decomp-perf-hud.md`, order: groundwork hook module, P2 measure, P3 log, P1, G2,
  HUD): P1 bottom render pass off at the source, P2 second-eye
  pass, P3 screen filters, G2 dither option, the sharp HUD. Kept for later, in this order:
  - **P4** frame cap and render word set natively (no dependence on the cheat file or the mod;
    EUR and no-mod installs behave the same).
  - **P5** pacing check: log the VBlank counter (`0x0110d004`) against frames submitted
    (`0x010571d4`); if they beat, cap at half the emulated refresh instead of 30.0.
  - **G1** cutscene depth of field drawn with PICA gas mode, which our fragment generator
    refuses: check the log in a cutscene; if so, emit transparent black as upstream does. May
    explain the lost colour grade on the focus pixels (open bug below).
  - **G3** quest-start loading race (late reads make the quest list miss a file; "Quest
    Failed" in a dark void): watch for it; fixes in the ideas doc.
  - **O2** UDS fix for consoles joining at the same moment (before multiplayer tests); **O3**
    net sync if area-load stalls drop us from a hall.
  - **O4** Kick, target camera and panel taps on buttons for the Single screen layout.
  - **O5** true wide render (424x240): late option, high risk. **O6** 60 fps: not feasible.
- **Start-up freeze.** Intermittent: the app stops right after `RunTitle: entering run loop`,
  the console drops off the network and powers off later. Seen 4 times on 2026-11-18 (the last
  on the first launch after a vpk install).

- **2D layer cost** (`plans/vita-mh4u-2d-layer.md`, experiments in the performance notes), in
  order: (1) skip the hidden 3D world under full-screen menus (`plans/vita-mh4u-menu-world-skip.md`):
  built 2026-11-19, opt-in `covertrap`; quest counter 20 -> ~26 fps, picture identical, 0
  mispredicts; paused after the user test (+3 fps at the counter, -1..2 in non-covering menus; see the
  plan's last section); (2) opaque sprites drawn without blending (exact);
  (3) fragment programs with 8-bit output so the patcher's blend code is cheaper for all 2D.
- **Heavenfall mod fixes** (built locally 2026-11-19 from the web build's own options; the base
  build is byte-identical to the web build): (A) hidden panes also get scale 0, so the game stops
  rasterising them (40 `.arc` files, only alpha-0 sprites changed; menus such as ui415, ui511,
  ui800, ui481, ui500); (B, optional) drop the quest list's full-screen backdrop, which `ss_fill`
  hides (~1 screen at the counter). Needs a play test reaching those menus without save states
  (states hold the loaded layouts). The camp HUD's alpha-0 area is the game's own: emulator side.

## Open bugs

- **Crash loading a state** (once, 2026-11-19: slot 7 loaded after browsing menus in slot 5; the
  same load from a fresh start worked). In `System::LoadState`, the boost input archive's
  destructor releases a tracked `Kernel::Process`; its `ClientSession` -> `ServerSession`
  destructor calls `ClientPort::ConnectionClosed` on a port already freed (r0 `ff0a0000`). Order
  of destruction in the archive's shared_ptr helper (a map by address), so intermittent. Dump
  `psp2core-1795073616-...`, build 8333a4424.
- **Software keyboard (chat, name entry) runs at ~20 fps.** Same as at character creation.

- ~~Pause menu opens by itself after a load~~ (fixed 2026-11-19: 1 s window after the clock jump; verify in play).
- Lit draws sent through the software vertex path (`nohwvs`, or the `litsw` experiment) hang
  the GPU at the first lit frame.
- The cutscene's colour grade on the depth-of-field focus pixels is lost (feedback draws are
  drawn alpha-only).

## Future ideas (not scheduled)

Picture quality, in order of value for effort. Not a priority while the mod's HUD at 100% keeps
most text readable (2026-11-18).

1. **HUD mask with its own upscale filter.** Copy the 400x240 frame just before the 2D layer
   starts (the grade pass, `hud_phase`, already marks that point). At the present, a pixel that
   differs between that copy and the finished frame is HUD: upscale it with a crisp or
   text-friendly filter (sharp bilinear, or a pixel-art scaler such as xBRZ), and the rest with
   linear or bicubic. Covers text, icons, minimap and frames; changes nothing in how the game
   draws, so no draw-order bugs. Estimated +0.5-1 ms GPU (one copy, one extra read per screen
   pixel), almost no CPU. It reshapes the 400x240 pixels only - no extra glyph detail.
2. **Bicubic (Catmull-Rom) upscale**, 5 texture reads: sharper 3D than linear without the
   blockiness of sharp bilinear, ~+1.5-2 ms. Optionally RCAS/CAS sharpening with a strength
   setting on top, ~+1.5-3 ms.
3. **Sharp HUD text again** (redrawn glyphs at screen resolution, removed in 57d1fd733; history
   58c3ea0d7..f834922b7): the only option that adds real detail to text. Could sit on top of (1)
   for the letters. Needs a re-test against today's renderer.
4. **FSR 1.0 (EASU + RCAS)**, trimmed: the best 3D upscale, but probably 5-10 ms at 960x544 on
   this GPU - an experimental option at most.
Not worth it: Lanczos (ringing, many reads), pixel-art scalers on the 3D scene (smeared look),
per-vertex highlights (`pvhalf`: white pond, see the performance notes).

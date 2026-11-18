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

- **Long sessions (heap fragmentation).** In progress: large short-lived buffers moved to the
  BigArena (performance notes, 2026-11-18). Still to do: the geometry-shader memo's scratch
  recorder, the JIT label table, an emergency reserve, then a 90+ minute session with
  `heapprof`; if in-use heap still grows ~6 MB an hour, cap the long-lived caches.
- **Multiplayer: local and online.** Test local play (two Vitas, or a Vita and a PC running
  Azahar) through Azahar's network rooms, and online play through a room server: does it
  connect, does the frame rate hold, do quests stay in sync. MH4UltimateDC's
  `docs/research/community-enhancements.md` (section 4) and its lobby/room server notes are a
  starting point.
- **EUR game (v1.1.0 update, 0004000E00126100).** Test the European release and find what to
  improve. Our code patches (the movie poll fix, `Mh4uPatchFmvPoll`) know only the USA v1.0 and
  v1.1 addresses, so EUR gets none (`code differs`). MH4UltimateDC's `config/shifts/eur_v110.txt`
  maps USA v1.3.0 (title version 1072, the same build as our "v1.1") addresses to EUR: the movie
  poll's `bcc` (0x968764) is at +0x38 there, so adding that shift to `Mh4uFmvShifts` may be all
  the patch needs (it checks the code word by word before writing).

## Open bugs

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

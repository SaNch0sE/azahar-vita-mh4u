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

- **Menu (done 2026-11-20):** every menu opens with its default button really focused (X works at
  once; ImGui 1.79's SetKeyboardFocusHere only drew the highlight - now SetFocusID), dropdowns
  open on the current value; release hides Speed limit, Screen layout, Swap screens and the full
  "Screen" HUD resolution (the 1.5x one is offered as "Screen (1.5x)"); leaving the settings
  after changing a launch-time setting (Render scale) asks to restart, and "Restart now" shuts
  the emulator down and relaunches the app (sceAppMgrLoadExec). The white area at tunnel exits
  (Primal Forest, slot 8) is the game's own: Azahar on Android (GL and Vulkan) shows the same.

## Next release checklist (from the user, 2026-11-20)

- Recommend the mod's **80% HUD size** (README updated).
- Default HUD resolution **1.33x** (done); Sharp bilinear / FXAA now also apply under the hi-res
  HUD (in progress) - the user decides their defaults after testing.
- Settings reset on the first start of the new release so the defaults apply (done:
  `version=` in video.txt, OptionsVersion 1; older files are replaced by the defaults once).
- README and release notes: recommend the **USA** game with the **"v1.3 (v1.1)"** update
  (exactly that wording; README updated).

## Fixed: character shadows camera-dependent (2026-11-20/21, user slots 8/9, Primal Forest area 1)

User: on Android the hunter / Palico shadows are stable; on the Vita they are "highly dependent
on camera angle - sometimes fine, sometimes partial or not visible". Character shadows are now on
by default (46f60cf2d). Established on the console:
- The shadow map (0x18154F00, 256x256) is right and camera-independent: dumped at two angles it
  holds the same hunter + Palico silhouettes (~200 texels, near the top-left).
- No effect: dbgshadownocull, nolightprenorm, nospottrim, nofresnelskip, refllut, noreflskip,
  nofogquery, keepdeadlight, nofrfit; dbgshadowsil (reference 1 - bias, ignoring w) still shows
  no hunter shadow at slot 8 at any angle -> not the w path; coordinates or the lighting hookup.
- logshadow at slot 8: receivers sample with ortho 1, bias 0x02067B, lighting shadow 1 primary 1
  secondary 1 invert 0 **alpha 1** selector 0, lights 1, **disable_shadow FF** (per-light shadow
  off for every light), tex0 in no TEV stage, wrap 1 1, border FFFFFFFF. So the shadow reaches the
  picture only through the lighting colours' alpha (shadow_alpha), which a TEV stage must read -
  check that path in fs_usse_gen (primary/secondary alpha with shadow_alpha, the Fresnel-skip
  `primary_alpha_read` logic, the dead-lighting skip with alpha-only readers) against
  glsl_fs_shader_gen.cpp:588-597 / 805-827.
- nohwvs never got past loading (black) - broken or too slow in this build.
- Fable's static comparison (receiver vs GLSL / software renderer) found no proven defect; its
  other candidates: TEXCOORD3 declared one wide vs the VS's two (fs_usse_gen ~786-806,
  vs_usse_gen VertexOutputs2), the 1-tap compare, the border handling, the half-pixel y shift on
  the shadow pass.
- Test-harness note: a second diag run in one script often lands on our pause menu over a garbled
  screen ("resume event: pausing ... 2 s clock jump" during a slow load) - a false resume
  detection worth fixing too.
- **2026-11-20, later:** `dbgshadowzero` darkens the bright ground (the path reaches the picture);
  `dbgshadowlit` = normal picture (0.9/255): the big dark areas are the game's own lighting, and
  normally almost every lookup is "lit". `dbgshadownoborder`, `dbgshadowflipt`: no change.
  `logshadowbind`: receivers bind the shadow pass's own target (same surface, GPU content).
  `dbgout_uv0` (new: receivers' lookup coordinates as colour): at the hunter's feet s~0.16
  t~0.20, most of the ground >= 1 (outside the map: lit, as in GL); identical with
  `dbgshadowrecvsw` (software vertex shading) - so the game's coordinates are what they are.
  In the dumped map the hunter's silhouette is at s~0.37-0.45, t~0.10-0.23. **Correction:** the
  "feet" reading came from a screenshot pixel on the hunter's own body (he is a receiver too) and
  JPEG noise; screen sampling of `dbgout_uv0` is not reliable enough (the HUD/minimap keep their
  colours). `dbgshadowswapst` (s<->t) and with `dbgshadowflipt`: no hunter shadow either.
  Next: numeric, not visual - log the receivers' texcoord0 (s, t, w) per vertex from the software
  path (`dbgshadowrecvsw`) for the ground draws around the hunter, and the casters' projected
  positions in the map from the shadow pass, then compare; check the shadow pass's viewport and
  the 3DS framebuffer orientation conventions for a 256x256 target.
- **Numbers (logshadowcoords, software paths at slot 8):** casters land at x/w -0.29..-0.09,
  y/w 0.51..0.77, z/w -0.93..-0.89 -> map s 0.36..0.45, t 0.76..0.89 (matches the dumped
  silhouette); ground receivers span s -2.7..2.6, t -0.7..4.2, w 0.67..1.28; ground vertices
  inside the silhouette's area have w 0.918..0.922 -> reference ~0.904 (bias 0x02067B ~ 0.016),
  while the map stores the hunter at 0.894..0.93: the comparison **should** shadow there. So the
  remaining suspects are the decode of the sampled texel (channel order / swizzle of the
  render-target texture as sampled, filtering) or the coordinate convention of the lookup.
  Swap/flip/no-border variants tried by the user in-game: all wrong. Next: `dbgout_shadow` (new:
  receivers painted (map depth, reference)) - captured once at slot 8 (o_shv) but not yet read;
  a dimmer-red hunter shape on the ground = the lookup works, uniform red = decode/sampling bug.

**Outcome (2026-11-21):** the receivers compared a single map texel, where GL's
`shadowTexture` compares four and blends them bilinearly. With one tap every map texel was a
hard square (the hunter's shadow a staircase) and, where the ground's depth sits within a texel
of the map's (reference ~0.904 against map 0.894-0.93), a pixel-sized lit/shadow speckle whose
pattern moved with the camera. Now four taps (`ShadowSampleTaps` / `ShadowCompareBilinear`),
which matches the Android shots: soft shadow edges, no speckle. Cost at slot 9: GPU +9 ms per
second (~0.3 ms a frame), render thread ~+1 ms a frame; still 30 fps. `shadowonetap` (file or
flag) restores the single lookup.
- Ruled out on the way: none of the 14 renderer kill switches changes the floor; vertex
  processing of casters and receivers on the CPU (`dbgshadowsw dbgshadowrecvsw`) gives the same
  picture, so the map and the lookup coordinates were right. Note those switches only move the
  vertex shader to the CPU - the pixels are still our fragment programs.
- The `dbgout_shadow` view saturates (the post effects brighten it after the program), so it
  cannot compare the two depths; not worth more time.
- Found on the way: the program disk cache's version field (48 bytes) cut the key at 47
  characters - MD5 (32) + '-' + the switch hash (16) is already 49 - so `-noshadow` (the
  Character shadows option) never reached the file and a toggled option was served the other
  kind's programs. Field now 80 bytes, file format 2 (old caches start over once).
- Also: textures' PICA LOD bias is now applied (`Sampler::Apply`, GXM 1/8-level steps;
  `nolodbias` file/flag to leave it out). MH4U's ground has a -1 level bias on unit 2; small
  visible effect - the ground is still softer than Android's at 1x (open, below).

## Open: ground textures softer than Android at the same 1x (2026-11-20)

User: "it really is just blurry" - same content (the blue patches are there), less detail.
Not the HUD copy (HUD 3DS vs 1.33x look the same), not the kill switches. The ground uses
128x128 textures with 4 mip levels on units 1-2 (`logtexmip`); unit 2 has a -1 level bias,
now applied, which helped only a little. Next: compare the mip level GXM picks with the PICA's
(the 240x400 target is rotated; derivatives may come out differently), and the filtering.

## Idea: one launcher for MH4U and MHXX (2026-11-20, later)

A test build of Monster Hunter XX exists: `cmake -DVITA_GAME=MHXX` gives MHXXVITA0 ("Monster
Hunter XX (test)"), data in ux0:data/mhxx, the game (`Monster Hunter XX ...` .cci) and its update
CIA (name containing "mhxx", e.g. the English patch 3.1) taken from ux0:data/azahar/roms, Hybrid
screen layout by default with the layout settings shown, 5:3 and the 3DS HUD by default. The
MH4U-specific hooks stay inactive (exact version checks). User test: **it runs**, but fps is low
in 3D scenes and in the system keyboard. Later: one launcher app that installs and runs both
games, with per-game improvements (hooks, defaults, cheats, mods) - for now documented only.
- **Comparison with the other Vita Azahar app (AZAHARPSV), same spot, 2026-11-20:** it also shows
  camera-dependent terrain shadows (a sharp diagonal dark area that appears / vanishes with a
  small camera turn) and no visible hunter shadow - but, per the user, much less visible and in
  fewer places than ours, where it covers larger areas with bigger visible issues. Android is
  stable. So: a shared Vita-side cause (core / GPU behaviour) plus something in our renderer that
  widens it. Next session: same spot, same angles in both Vita apps (and Android), then bisect
  our renderer's differences (our lighting programs and shortcuts, Smooth gradients / dither,
  opaque-sprite, dead-lighting, HUD copy) with their kill switches against the AZAHARPSV look.

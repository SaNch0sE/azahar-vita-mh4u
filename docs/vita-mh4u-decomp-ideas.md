# MH4UltimateDC: what it gives the Vita port

Research notes, 2026-11-18. Source: [MH4UltimateDC](https://github.com/CompaLarios18/MH4UltimateDC)
by CompaLarios18 (GPL-3.0-or-later). It statically recompiles MH4U to C and runs it on Azahar's
core for PC and Android, and documents the engine (MT Framework Mobile). Engine addresses and
notes below are from that project; credit it when using them. Their code and patch hunks are not
copied into this tree (GPL-3.0 vs our GPLv2-or-later): only facts (addresses, layouts, bit
meanings, protocol descriptions), rewritten in our own code.

All addresses are **USA v1.3.0** (title version 1072, the update we call "v1.1"; one address
cross-checked) unless marked EUR. "Not confirmed" marks what nobody has run yet.

Priority order: performance, then correct graphics, then HUD, then the rest.

**Corrections (2026-11-19, checked against our own v1.1/v1.0 executables; details in
`plans/vita-mh4u-decomp-perf-hud.md` section 0):** v1.0 data addresses are *not* the same as
v1.1 (singletons 8 lower, filter vtables 0x30 lower, code shifts vary per site); cheats run ~5.4
times a second, not every 1/60 s; render-word bit `0x40` guards only a second display transfer in
`buildFrame`, not a second scene draw (so P2 is probably worth little); the filter array is
`sScene+0x20` with its count at `+0x24`; the Heavenfall mod already hooks bloom's `render`.

## How their hooks map onto our architecture

Their hooks wrap recompiled C functions. We run the guest natively, so each becomes one of:

- **A. Data poke**: write guest memory from the emulation thread once a frame (or from the cheat
  file). No code change, no `SyncCode`. Most useful hooks reduce to this.
- **B. Word patch**: replace instructions after checking the code word by word, as
  `Mh4uPatchFmvPoll` does. For filters, writing a vtable slot (data) does the same and can be
  switched live.
- **C. Trap hook**: our private `svc 0xF0` pattern with a host handler. Fine once a frame
  (`sMhMain::frame` `0x00af5250`), too slow for per-object functions.

Anchors (`config/symbols.txt`, `docs/research/system-services.md`):

| Address | What |
|---|---|
| `0x01057194` | `sMhMain*`: `+0x2c` rate (60.0), `+0x30` cap, `+0x34` measured fps, `+0x5c` delta (1/60 s) |
| `0x010572cc` | `sLtRender*`: `+0x6ba8` render word, `+0x8aa8/+0x8aac` `mDisplaySkip`, `+0x8ab1/+0x8ab2` top/bottom present flags, `+0x6c28` main scene |
| `0x010571d4` | frames submitted |
| `0x0110d004` / `0x0110d008` | VBlank counters, top / bottom |
| `0x010572f0` | `sScene*`: filter array at `+0x20` |
| `0x0105729c` | `sMhCamera*`: 8 viewports of `0x1a8` at `+0x18`; 0 top, 7 bottom |
| `0x01057534` | `sMhLayout*` |

## Summary

| # | Item | Area | Benefit | Risk | Effort |
|---|---|---|---|---|---|
| P1 | Bottom render pass off in Single screen | perf | guest CPU + GX parse, every frame | low-medium | small |
| P2 | Second-eye pass: measure, then clear `0x40` | perf | up to one scene traversal of guest CPU | low | small |
| P3 | Filters skipped by vtable slot | perf / option | fewer draws with monsters, in cutscenes | low | small |
| P4 | Native render word and frame cap (no cheat file) | perf / robustness | small; no dependence on the mod | low | small |
| P5 | Cap at half the emulated refresh | pacing | removes a possible periodic dropped frame | low | tiny, after measuring |
| G1 | Gas-mode draws (cutscene depth of field) | graphics | explains an open bug | medium | medium |
| G2 | Dither off and 8-bit display buffers | graphics option | cleaner gradients | low | small-medium |
| G3 | Quest-start loading race | correctness | avoids a rare "Quest Failed" | low | small |
| H1 | Cached HUD layer at screen resolution | HUD | sharp HUD, maybe cheaper | medium-high | large |
| H2 | HUD parts by layout hash | HUD | enables H1; HUD at edges | low | small |
| O1 | EUR support by shift table | other | EUR gets all fixes | low | small (branch `eur-support`) |
| O2 | UDS fix for simultaneous joins | multiplayer | 3-4 player halls | low | tiny |
| O3 | Net sync against stalls | multiplayer | fewer drops | medium | medium |
| O4 | Kick, target camera, Skip on buttons | input | touch-free Single screen | low-medium | medium |
| O5 | True wide render (424x240) | widescreen | small sharpness gain, +6% pixels | high | large |
| O6 | 60 fps | - | not feasible on the Vita | - | - |

## 1. Performance

### P1. Turn the bottom render pass off in Single screen

- `sLtRender::buildFrame` (`0x00bf5a4c`) draws display 1 (camera draw, then a transfer) only while
  byte `sLtRender+0x8ab2` is set (or the applet state `[[0x0104dea0]+0x30]` is 4-6).
  `sLtRender::updateDisplaySkip` (`0x00bfa6f0`) recomputes it only while `mDisplaySkip[1]`
  (`+0x8aac`) is non-zero; with the default 0, a written value stays.
- Source: their `docs/research/second-screen.md` 1.2, 1.5; `system-services.md`, "Present and
  buffer swap".
- How: data poke `[[0x010572cc]+0x8ab2] = 0` while the layout is Single screen and not swapped,
  `1` when a layout shows the bottom screen; re-assert each frame.
- Why: our skip (`PicaCore::DrawArrays` 240x320 viewport, `GPU::QueueMemoryTransfer`) still lets
  the guest build the pass and the GX thread parse it (~135 draws a frame on the deck). This
  removes the traversal, the list bytes, their parse and the bottom clear. Expected ~1 ms a frame
  on the GX thread plus guest CPU (not measured).
- Picture unchanged in Single screen. Not confirmed: nobody has run with the flag at 0. Test the
  Heavenfall single-screen menus, the software keyboard, HOME/applets, and switching back to two
  screens. The mod does not already do this.

### P2. Second-eye pass: measure, then clear bit `0x40`

- Their port clears render-word bit `0x40` (`mULCDEnableEdit`) every frame; with it set the game
  draws the top screen once per eye even at slider 0 (their `port/src/hooks.cpp`,
  `docs/PLAYING.md`, setter table at `0x00e35a7c`).
- Our cheat and the Heavenfall mod write `0x011F036E` / `0x010F036E`: against the default
  `0x001F076E` that clears `0x400` (`mFSAA_2D` by their table) and sets bit 24, and **keeps
  `0x40`**. Their bit names are medium confidence.
- We run with `disable_right_eye_render`: `RightEyeDisabler` blocks a second top-screen list on
  the GPU side, but the guest would still build and submit it.
- How: count `RightEyeDisabler::ShouldAllowCmdQueueTrigger` refusals per second. About one a
  frame means the pass is live: clear `0x40` (data poke, or cheat words `...032E`) and measure.
- Benefit: zero if already gone; otherwise a whole scene traversal and its list copy per frame.

### P3. Skip screen filters at the source

Every post-process pass is a `uFilter` subclass; `render(this, cDraw*)` is vtable slot 12
(`+0x30`); the base `render` is `bx lr` at `0x00627bd8`.

| Filter | vtable | `render` | Used |
|---|---|---|---|
| `uBloomFilter` | `0x00e5e7f4` | `0x002bc238` | nearly every stage, 121 cutscenes |
| `uDOFFilter` | `0x00e5bafc` | `0x001b0890` | 193 cutscenes |
| `uBokehFilter` | `0x00e5e844` | `0x002bd53c` | 1 cutscene |
| `uRadialBlurFilter` / `uMhRadialBlurFilter` | `0x00e62520` / `0x00e6313c` | `0x003b2938` | 37 cutscenes; in hunts from `0x00c0814c` (~40 monster functions, roars?) |
| `uMultiBlurFilter` | `0x00e61e08` | `0x002d3168` | made by code (`0x00af3400`, up to 8): motion blur |
| `uHazeFilter` / `uMhHazeFilter` | `0x00e5d56c` / `0x00e5ff74` | `0x0025655c` | 4 cutscenes |
| `uColorCorrectFilter` | `0x00e62f28` | `0x003dd740` | intended colours: never skip |
| `uColorFilter` / `uFadeFilter` | `0x00e5e970` / `0x00e5d474` | `0x002bffc0` | fades: never skip |

- How: write `0x00627bd8` into `vtable+0x30` (switchable live).
- Bloom and depth of field are already off through the mod's default and dropped GXM-side. New:
  radial blur and multi-blur, which run in hunts and read the frame back (our feedback draws are
  alpha-only, so they are probably wasted or wrong). Possibly the roar dips.
- First log which filters are live (walk `sScene+0x20` each second; read `0x006c0d08` for the
  array layout - not confirmed). Make radial/multi-blur skipping an option unless we already fail
  to draw them.

### P4. Native render word and frame cap

- The cap is `[[0x01057194]+0x30] = 30.0`, exactly our cheat; no game code writes it after
  construction, so one write is enough (the cheat engine rewrites it every 1/60 s).
- Doing it natively makes EUR and no-mod installs behave the same.
- Experiment only: render-word bits `0x100` (`mParallelRenderingEdit`) and `0x4`
  (`mTripleBufferRenderWaitEdit`) change how the game's render thread overlaps the main loop.

### P5. Cap at half the emulated refresh

- The emulated LCD runs at ~59.83 Hz (29.915 per two VBlanks) while the cap is 30.000: if our
  presents follow emulated VBlank, a frame repeats or drops about every 12 s.
- How: log VBlank `0x0110d004` against frames submitted `0x010571d4` for a minute; if they beat,
  write the cap as half the emulated refresh. Not confirmed that we show the beat.

Not in their repo: culling or level-of-detail tunables, draw distance, grass, glyph GS draws.

## 2. Correct graphics

### G1. Cutscene depth of field uses PICA gas mode

- Their reading of Azahar issue #765: depth of field mixes a quarter-resolution blurred copy by
  distance through an `mpDensity` texture, drawn with gas rendering; Azahar writes transparent
  black for gas fragments (discarding breaks MH games).
- Ours: `FragmentEmitter::Unsupported()` (`renderer_gxm/usse/fs_usse_gen.cpp`) refuses gas
  configurations, so the draw has no program - different from upstream.
- How: look for "no fragment program ... gas" in a cutscene log; if present, emit a constant
  (0,0,0,0) program for gas draws. May bear on our open bug (colour grade on the focus pixels
  lost): `uDOFFilter` has an `mDOFColor` property. Not confirmed on our port.

### G2. Dither off and 8-bit display buffers (option)

- The game draws its own dither (render-word bits `0x100000` top, `0x200000` bottom) because its
  display buffers are RGB565; upscaled it shows as a fine grid. The mod's "No Dithering" clears
  `0x100000`. Option: dither off and keep the display-transfer destination 8-bit on the host.

### G3. Quest-start loading race

- Late file reads can make the quest list (`0x0069b21c`) miss a file: the hunter falls through a
  dark void and the quest fails. Seen by them at 60 Hz with a 50% CPU.
- Ours: large reads run on the `HLE_Async` pool, timing depends on the card. Not seen yet. If it
  appears: make the emulated read delay independent of host time, or hold the quest start a frame
  (holder `[0x00fbeffc]`, table `+0xe604`, list `+0xe608`, count `+0xe610`, release
  `0x006bc838`, start `0x00b7c010`).

## 3. HUD

What their notes give:

- The HUD is `rLayout`: one `uLayout` unit owned by `sMhLayout`, drawn by `uLayout::draw`
  (`0x00b04774`). Each object: name hash at `+0x00` (`~crc32` of the pane name), view number in
  bits 14-17 of `+0x44` (0 top, 7 bottom), visible bit `0x400`.
- Per object `0x00ae65a4` uploads translation and colour as a shader constant; `0x00ae6cd0`
  records or replays a command cache.
- Hunting HUD hashes: clock `2E7FA61D`, health `8293D551`, stamina `D07F4AA2`, sharpness
  `BD56507D` / `B2DA1296`, party rows `8AC1CCC4` `FDC6FC52` `64CFADE8` `13C89D7E`, item
  selection `1833E2C8`, item name `3C598C15`; 3D name tags `F24BC1DA`.
- The minimap draws mask passes (`ui25x_sprite_mask_write`, `ui258_sprite_mask_blend`), which
  matches our DstAlpha draw #66.
- Layout projection: the Heavenfall mod's `FUN_00b70fc0` is `0x00b70f70` in USA v1.3.0, called
  from `sLayout::beginViewport` (`0x00af3d60`).

What they do not give: a marker for where the 2D layer starts in the GPU stream (draws are
recorded into packets and replayed by `sLtRender::execute`, `0x00bfa7ec`), so our grade-pass
marker (`hud_phase`) stays the right split point.

### H1. Cached HUD layer at screen resolution

Draw the post-grade 2D layer into its own 960x544 target only when it changes (hash of the
layer's vertex data and uniforms, which the glyph memo already fingerprints) and blend it in the
present. Not free: redrawing at screen resolution costs ~3-5 ms on frames where the HUD changes;
saves ~2.8 ms GPU on static frames. Risks: minimap DstAlpha masks need the layer's own alpha;
name tags move every frame. Our todo's HUD-mask idea stays the cheaper first step.

### H2. HUD parts by layout hash

Move or hide single HUD parts by hash (e.g. HUD to the screen edges without the mod). Their hook
on `0x00ae65a4` is per object, too hot for a trap; a word patch into a small guest stub works.

## 4. Everything else

### O1. EUR support (done on branch `eur-support`, untested)

`config/shifts/eur_v110.txt` maps USA v1.3.0 `.text` to EUR v1.1.0 (title version 1040), 31,364
of 31,409 functions; outside `.text` addresses are equal, so every data anchor above holds.

| What | USA | EUR |
|---|---|---|
| Movie poll branch / return / loop return | `0x968764` / `0x9687AC` / `0x96417C` | `+0x38` each |
| Vector filter entry / exit (cheat checks) | `0x110C64` / `0x110DA8` | same |
| Bloom `render` | `0x2BC238` | `0x2BC258` |
| Depth of field `render` | `0x1B0890` | same |
| Radial blur `render` | `0x3B2938` | `0x3B2960` |
| Multi-blur `render` | `0x2D3168` | `0x2D3188` |
| Base `render` (`bx lr`) | `0x627BD8` | `0x627C00` |

The Heavenfall mod's symbols are in the EUR address space (four pairs checked), so the shift
table also translates between the mod's names and theirs. No map exists for v1.0.

### O2. UDS fix for consoles that join at once

Azahar's host broadcasts the EAPoL-Logoff that assigns a node id with `client_mac_address` left
at zero: two consoles joining together take the same id and one drops. Fix: fill the field in
`GenerateEAPoLLogoffFrame`, and have a connecting console ignore a logoff for another address
(zero still accepted). We lack it (`src/core/hle/service/nwm/`). Rewrite from this description.

### O3. Multiplayer stalls and net sync

The game drops a player silent ~1 s; a 2 s host stall migrates the host. Their answer: each
console sends its emulated clock 20 times a second (Wi-Fi packet type `0x4d`); one more than
200 ms ahead waits. Our area-load and state stalls could drop us: test first. Facts: the hall is
a UDS network on data channel 243; rooms use `network_version` 4; USA and EUR play together.

### O4. Kick, target camera, Skip on buttons

- Kick: OR `0x0001` into `sMhPad+0x3f0` (instance `[0x010572e0]`).
- Target camera: HUD byte `[0x0108260c]+0x27e` (counts as a tap, cleared in the same update).
- Any panel tap: panel id `0x0109a350+0xce`, flags `+0xcc` (`0x0200` tapped), read by
  `PanelManager::isTapped` (`0x00288f4c`).
- Timed before `sMhPad::update` (`0x00694c98`). The mod already has "touchless target"; check
  what Single screen still lacks.

### O5. True wide render (424x240)

Scene target immediates `0x00bfc094`/`0x00bfc0a4`, display buffers `0x00bfa5ac`/`0x00bfa5b0`,
viewport `0x00aefdb0`/`0x00aefdc0`, transfer row `sLtRender+0x6e1c = 512 - W`, plus
`0x005de49c`, `0x005de4a0`, `0x00af0c84`. +6% pixels on a GPU-bound frame and many 240x400
assumptions in our renderer; the mod's anamorphic 16:9 already works. Late option.

### O6. 60 fps

The engine is delta-time based and runs at 60/120 on PC, but the Vita's GPU alone takes ~30 ms a
frame. Not feasible.

## Suggested order

1. P2 measurement (one counter) and P3 filter log: cheap, may reveal hidden work.
2. P1 bottom pass off in Single screen.
3. P4/P5 native cap and pacing check.
4. G1 gas-mode check in a cutscene; G2 as an option.
5. H2, then H1 if the HUD-mask idea is not enough.
6. O2 before multiplayer testing; O3 if stalls drop us; O4 for Single screen.

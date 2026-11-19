# Plan: P1, P2, P3, G2 and a sharp HUD (from the MH4UltimateDC research)

Planned 2026-11-19 (Fable). Items and addresses come from `../vita-mh4u-decomp-ideas.md`, which
is based on [MH4UltimateDC](https://github.com/CompaLarios18/MH4UltimateDC) (facts only, no
code copied). Checked against our code and a disassembly of our own mod-built executables (the
v1.1 and v1.0 `exefs/code.bin` of the Heavenfall builds). Nothing here has run on the console:
every runtime claim is a reading of code and needs the stated measurement.

## 0. What the checks changed

Several premises in the ideas doc are wrong or incomplete:

1. **v1.0 data addresses are not identical.** Singleton pointers are 8 lower, filter vtables 0x30
   lower, and code shifts differ per site (-0x1A4, -0xFC, -0x128, -0x130, -0x5A8, -0x678,
   -0x698). Offsets inside objects are the same. Use the table below and check each word.
2. **Cheats run ~5.4 times a second, not 60.** `src/core/cheats/cheats.cpp`:
   `run_interval_ticks = 50'000'000` at 268,111,856 Hz (~186 ms). Fine for steady values, not
   for anything per-frame.
3. **`GxmFlag` is diagnostics-only** (always false in release, `gxm_flags.cpp`): release kill
   switches must be files in `ux0:data/mh4u` or `video.txt` keys.
4. **`nobottomskip` is inconsistent in release.** `PicaCore::DrawArrays` reads only the flag;
   `GPU::QueueMemoryTransfer` also reads the file. With the file present, release keeps the
   transfer but still drops the draws. Fix with P1.
5. **Likely data race in the right-eye path.** `GPU::QueueCmdList` calls
   `RightEyeDisabler::ShouldAllowCmdQueueTrigger` on the emulation thread; it calls
   `PicaCore::GuessCmdRenderProperties`, which resets the member `cmd_list` the GX thread may be
   parsing (gxfifo is the default). Fix before trusting P2 counters.
6. **The `0x40` test in `buildFrame` is on the live word `sLtRender+0x6ba4`; the cheat writes the
   edit word `+0x6ba8`.** `0x00bf990c` copies edit to live each frame and rewrites bit 24 from a
   query (`0x00d82318`), so the cheat's bit 24 does not stick.
7. **In `buildFrame`, `0x40` guards only a second display transfer, not a second scene draw**
   (path at `0x00bf5c58`: when `0x40` and (`cDraw+0x1bc` bit 7, or `[sMhCamera]+0xe08` == 0), a
   `cDraw` helper and a transfer into the other eye's buffer). The "whole scene traversal" claim
   is not supported.
8. **Filter array layout (from `0x006c0d08`):** `sScene+0x20` -> array of `uFilter*`, `+0x24`
   count, `+0x2c` twice the capacity, `+0xa7` dirty byte, `+0x04` lock.
9. **Filter vtables confirmed** in our v1.1 dump. Slot 5 is `ldr r0,[pc]; bx lr; .word DTI`.
10. **The Heavenfall mod already hooks bloom:** the first word of bloom `render` (`0x2BC238` v1.1,
    `0x2BC108` v1.0) is a branch into mod code.
11. **Our bloom/DoF drop is by address:** `RasterizerGxm` drops draws whose target or texture is
    `0x1847D400`, `0x1849D400` or in `[0x18070800, 0x18090000)`. Radial blur, multi-blur and haze
    probably share those buffers and are probably invisible today, but the guest still builds
    them.
12. **`updateDisplaySkip` confirmed:** it writes `+0x8ab2` only while `+0x8aac` is non-zero. Only
    `buildFrame` (`0x00bf5cd0`) and the present (`0x00bf8a20`) read the flag; both also test
    applet state 4/5/6 at `[[0x0104dea0]+0x30]`.

### Address table

| What | USA v1.1 | USA v1.0 | EUR v1.1 (untested) |
|---|---|---|---|
| `sLtRender*` | `0x010572CC` | `0x010572C4` | `0x010572CC` |
| `sScene*` | `0x010572F0` | `0x010572E8` | same as USA |
| `sMhCamera*` | `0x0105729C` | `0x01057294` | same |
| `sMhMain*` | `0x01057194` | `0x0105718C` | same |
| Applet object pointer | `0x0104DEA0` | `0x0104DEA0` | same |
| `buildFrame` | `0x00BF5A4C` | `0x00BF53D4` | `0x00BF5A9C` |
| `sLtRender` vtable slot with the render virtual | `0x00E6CAD8` -> `0x00BFA670` | `0x00E6CAA8` -> `0x00BF9FF8` | `0x00E6CAD8` -> `0x00BFA6C0` |
| Base `render` (`bx lr`) | `0x00627BD8` | `0x00627630` | `0x00627C00` |
| Radial vtables / `render` | `0xE62520`, `0xE6313C` / `0x3B2938` | `0xE624F0`, `0xE6310C` / `0x3B2810` | USA vtables / `0x3B2960` |
| Multi-blur | `0xE61E08` / `0x2D3168` | `0xE61DD8` / `0x2D3038` | USA / `0x2D3188` |
| Haze (two vtables) | `0xE5D56C`, `0xE5FF74` / `0x25655C` | `0xE5D53C`, `0xE5FF44` / `0x25642C` | USA / `0x25655C` |
| DoF | `0xE5BAFC` / `0x1B0890` | `0xE5BACC` / `0x1B0794` | USA / `0x1B0890` |
| Bokeh | `0xE5E844` / `0x2BD53C` | `0xE5E814` / `0x2BD40C` | USA / `0x2BD55C` |
| Bloom | `0xE5E7F4` / `0x2BC238` | `0xE5E7C4` / `0x2BC108` | USA / `0x2BC258` |

Object offsets are the same in all three: `+0x6ba4` live word, `+0x6ba8` edit word,
`+0x8aa8/+0x8aac` skip, `+0x8ab1/+0x8ab2` present flags.

## 1. Shared groundwork (about one day)

### 1.1 Game hook module

New `src/core/arm/vita/mh4u_hooks.{h,cpp}` (in `src/core/CMakeLists.txt`).

- **Call site:** `ARM_VitaNative::RunSlice`, next to `Mh4uPatchFmvPoll()` (guest stopped,
  emulation thread).
- **Gate:** run `Tick()` only when `Common::PipelineStats::guest_frames_total` changed (once per
  guest frame, one atomic load per slice); fallback every 64 slices while it stands still.
- **Static state** (each emulated core has its own `ARM_VitaNative`).
- **Why here:** a CoreTiming event would sit in every save state's queue; vblank/present code is
  in `video_core` and partly on other threads; HLE calls give no cleaner per-frame point; a cheat
  cannot see the layout and runs at ~5 Hz.
- **Version detection, data only:** on a process change (boot, state load) read `[0x00E6CAD8]`
  and `[0x00E6CAA8]`, compare with the table, pick v1.1 / v1.0 / EUR by title id. No match: log
  `[mh4u] hooks: unknown executable (...)` and do nothing. Independent of `mh4u_code_version`.
- **Before every poke:** `IsValidVirtualAddress`, non-zero singleton, expected current value.
  Wait 60 guest frames after a process change.
- **No code writes, no `SyncCode`:** all five items are data pokes, so the
  power-off-after-state-load hazard does not apply.
- **Clean save states:** `Mh4uHooks::BeforeSave()` from the `Signal::Save` case in
  `src/core/core.cpp` restores game defaults (`+0x8ab2 = 1`, original vtable slots, our
  render-word bits); the next tick re-applies. States stay vanilla and load on other builds.
- **After a load:** process pointer changes -> re-validate, re-apply. A slot already holding the
  no-op is accepted as patched.

### 1.2 Switch helper

`bool Mh4uSwitch(const char* name)`: true when `ux0:data/mh4u/<name>` exists or `GxmFlag(name)`
is set, cached. Used for every switch below and the `nobottomskip` fix.

### 1.3 Counters and log lines

`src/common/pipeline_stats.h`: `re_lists_blocked`, `re_xfers_blocked`, `top_xfers`,
`fx_dropped`. Release `cpu` line (`citra_vita_main.cpp`): `re l%u x%u t%u`, `bot d%u x%u`
(existing `bottom_draws_dropped` / `bottom_xfers_dropped`, today diagnostics-only), `fx %u`.
From the hook module, on change only: `[mh4u] render word live %08X edit %08X eye2 %u` and
`[mh4u] filters: ...`.

### 1.4 Measurement protocol

- Check the card for `ux0:data/mh4u/timedraw` (and other switch files) first.
- Speed: release build, scripted run, `autoload.txt` slot, 25-30 s, two passes; read `speed` and
  `cpu` lines (fps, `gx`, `render`, `gxc l/d`, per-thread CPU).
- GPU ms: diagnostics build, `scenetime timedraw`.
- Scenes (save states remade 2026-11-19, Single screen layout): 1 camp in a hunting area (the
  fishing spot), 2 heaviest open field in a quest, 3 water scene, 4 Val Habar with minimap, 5 in
  the house next to the item box, 6 the cat's Gallery menu on a heavy cutscene (Circle starts
  it), 7 the quest counter; 8 and 9 free. No "monster about to roar" state. Earlier numbers in
  this plan and the performance notes used the old slots.
- Picture: frame dumps (`dumpframe` / `DumpNextFrame`) on a still scene, compared pixel by pixel
  on the PC: zero differing pixels on a static shot, or noise level where the scene animates.

## 2. P2: second-eye pass (measure first)

1. **Fix the guess race:** `PicaCore::GuessCmdRenderProperties` uses a local `CommandList`
   instead of the member `cmd_list`. Deck and slot 7: picture unchanged.
2. **Count refusals** in `right_eye_disabler.cpp` (`re_lists_blocked`, `re_xfers_blocked`) and
   top transfers in `GPU::QueueMemoryTransfer` where `frame_boundaries` increments (`top_xfers`).
3. **Log the words:** `+0x6ba4`, `+0x6ba8`, byte `[sMhCamera]+0xe08`, on change.
4. **Read** on deck and slot 7: `re l` ~30/s = a second top list is live; `t` ~60/s at 30 fps =
   a second top transfer is live (~0.8 ms GPU blit each); both 0 and `t` ~30 = nothing to gain,
   close P2.
5. **If live:** opt-in `eyeoff`: each tick, clear `0x40` in the edit word if set (the game applies
   it in `0x00bf990c`). Re-measure; screenshots on deck, village, a cutscene must be identical.
6. **If identical and useful:** default on, kill switch `noeyeoff`.

Cheat conflict: our cheat words `0x011F036E` / `0x010F036E` keep `0x40` (both entries off by
default in our file); the mod writes the same words on its own schedule. The poke clears one bit;
if the bit is set again on three consecutive ticks another writer owns the word: stop and log
`[mh4u] eyeoff: another writer keeps 0x40 (mod or cheat)`.

- **Benefit:** probably none (fps counter counts top-sized transfers and reads 30; no second scene
  draw). Worth half a day: it closes the question and fixes the race.
- **Risk:** low. **Effort:** 0.5 day (+0.5 if live).

## 3. P3: filter log, then skip at the source

### 3a. Log

1. Each tick: `scene = [sScene*]`, `arr = [scene+0x20]`, `n = [scene+0x24]` (bounded to 64); name
   each entry's vtable from the table.
2. Unknown vtables: follow slot 5 to the DTI object, log it once with its first 8 words (class
   name pointer expected at DTI `+4`).
3. `[mh4u] filters: bloom colcorr radial ...` when the set changes; switch `filterlog` adds a
   once-a-second count line.
4. In `RasterizerGxm` count `fx_dropped` where the `no_postfx` chain drops a draw, and show the
   alpha-only feedback count per second: draws built and parsed for nothing.

### 3b. What our renderer shows today (before any skip)

Slot 7 with a roaring monster, and the ship cutscene; three frame dumps each: default;
`postfx keepfeedback feedbackbreak` (closest to the real picture); default with the 3c skip.
`logfeedback` shows what each pass samples. Default == skip -> skip can be on by default; default
shows a usable blur -> skip stays an option, off by default.

### 3c. Skip

1. Per-version table: vtable, expected `render`, no-op (base `bx lr`).
2. Apply: `[vtable+0x30]` == expected `render` -> write no-op; already no-op -> done; anything
   else -> log and leave.
3. Restore on switch-off and in `BeforeSave()`.
4. Classes: radial (two vtables), multi-blur, haze (two vtables). Never colour correction,
   colour or fade.
5. Second group, "what we drop anyway": bloom, DoF, bokeh, only while `postfx` is not set (their
   composites are already dropped GXM-side); verify in the village and the ship cutscene. Writing
   the bloom slot bypasses the mod's hook, which is harmless.

Switches: `video.txt` `fx_radial`, `fx_mblur`, `fx_haze` (1 = the game's own behaviour, 0 =
skip), pause menu "Effects" (`vita_ui.cpp`, `gxm_video_options.*`); `fxsrcskip` for the
bloom/DoF/bokeh group while tested; kill switch `nofilterskip`.

- **Benefit:** guest CPU and GX parse during roars, effects and cutscenes; nothing in quiet play
  with the mod's bloom off; without the mod, bloom's draws every frame.
- **Risk:** low (one data word per class, switchable live). A filter's `render` may advance state
  other code reads (multi-blur's frame history); MH4UltimateDC reports skipping `render` as safe.
- **Effort:** 1 day log and counters, 1 day skip and options, plus screenshots.

## 4. P1: bottom render pass off at the source

Layout test: `Settings::values.layout_option == SingleScreen && !Settings::values.swap_screen`
(as `PicaCore::DrawArrays` and `GPU::QueueMemoryTransfer` use). One helper,
`Mh4uHooks::BottomHidden()` (false when `nobottomskip`), used in all three places.

### Zero-code first measurement

A disabled entry in `src/citra_vita/cheats/0004000000126300.txt`, enabled on the card:

```
[Bottom pass off (experiment)]
50110C64 EE084A00
610572CC 00000000
B10572CC 00000000
20008AB2 00000000
D2000000 00000000
50110AC0 EE084A00
610572C4 00000000
B10572C4 00000000
20008AB2 00000000
D2000000 00000000
```

One write sticks (`mDisplaySkip[1]` is 0). Deck and slot 7: `bot d` should fall from ~4,000/s to
0; read `gx` ms, `gxc l/d`, emulation CPU, fps. Decides whether the native version is worth it.

### Native steps

1. `Tick()`: `want = BottomHidden() ? 0 : 1`; if `[sLtRender+0x8aac] == 0` and
   `[sLtRender+0x8ab2] != want`, write the byte (re-assert every tick, two reads).
2. A layout switch costs at most one frame with a stale bottom buffer.
3. `BeforeSave()` writes 1.
4. Keep both GPU-side skips as the fallback (applet states 4-6 force the pass; poke off or
   unvalidated).
5. Fix the `nobottomskip` file handling in `pica_core.cpp`.
6. Stage 1: opt-in `srcbottom`. Stage 2, after the tests: default on, kill switch `nosrcbottom`;
   `nobottomskip` turns off this and the GPU-side skips.

In the guest: no `sCamera::draw` for display 1, no applet bottom draw, no bottom transfer or
swap; the present's wait for an unshown bottom buffer (`0x00bf6f20`) no longer triggers (check
the fps line for jitter).

### Tests

- Deck, slots 1, 2, 7: fps and picture identical.
- Village slot 4: minimap and party list (drawn on the top screen by the mod).
- Menus with rendered previews: equipment box, guild card, item box, map, quest counter.
- Software keyboard (name entry, chat), HOME/applet path: no hang.
- Layout switching (Single -> Default -> Single, Swap screens): bottom screen back within a
  frame or two, touch works.
- Save states across layouts; slots made by older builds.
- Loading screens, the intro movie; a 30-minute session watching `[mh4u] stall` lines.

- **Benefit:** guest traversal of viewport 7 and the view-7 layout pass, ~135 draws of list
  bytes and their parse: a guess of 1-3% of the emulation and GX threads, no GPU change.
- **Risk:** low to medium (nobody has run with the flag at 0). **Effort:** 0.5 day cheat run,
  1 day native, 1 day testing.

## 5. G2: dither off with 8-bit display buffers (opt-in)

Where 5:6:5 happens: the game transfers its RGBA8 main target (256x512, cropped) into one of three
240x400 RGB565 display buffers: `GPU::QueueMemoryTransfer` ->
`RasterizerGxm::AccelerateDisplayTransfer` -> cache -> `TextureRuntime::BlitTextures` ->
`RasterizerGxm::GpuBlit` (textured quad into a `U5U6U5` surface, `FormatTable` in
`gxm_texture_runtime.cpp`; ~0.8 ms a frame). The present finds it in
`RasterizerGxm::AccelerateDisplay` and samples it in `GxmPresent::DrawGuestScreen`.

"8-bit on the host": a renderer-owned 240x400 RGBA8 copy of each top display buffer (at render
scale), filled by the same blit instead of the RGB565 one, shown by the present. (Changing
`FormatTable` or a per-surface storage override was rejected: it touches every place that treats
`pixel_format` as the storage layout and sends same-size transfers down a GPU-waiting path.)

1. `RasterizerGxm`: `struct DisplaySlot { PAddr addr; Surface wide; u32 frame; bool valid; }`,
   four slots by destination address; `wide` is a plain RGBA8 colour-target `Surface`, not cached.
2. `AccelerateDisplayTransfer`, option on, RGB565 top-sized output, no scaling: source via
   `res_cache.GetSurfaceSubRect`, build the `TextureBlit` (crop, flip as the cache does),
   `GpuBlit(source, slot.wide, blit)`, mark valid, return true. On failure: normal path,
   invalidate the slot.
3. `AccelerateDisplay`: return the valid slot's texture and rectangle for a matching address.
4. Invalidate a slot on any other write to its range: `AccelerateFill`, `AccelerateTextureCopy`,
   a normal-path display transfer, `InvalidateRegion`, `ClearAll` (loading screens are fills).
5. Same `presented` / serial handling as the cached surface (`EndFrame`).
6. Dither: the hook module clears `0x100000 | 0x200000` in the edit word while on; restore in
   `BeforeSave()`; same "another writer" detection as P2.
7. Diagnostics `fb8both` runs both blits for comparing.

Switches: `video.txt` `smooth=1`, pause menu "Smooth gradients (no dither)", off by default;
kill switch `nofb8`; diagnostics `fb8`, `nodither`, `fb8both`.

With the mod's "No Dithering": the bit is already clear, so `smooth=1` only adds the 8-bit copy
and removes the banding that setting causes today. If the mod keeps writing the word with dither
on, stop poking, log once, and show "turn on No Dithering in Mod Settings" in the pause menu.

- **Cost:** about zero (same blit pixel count, 4 bytes instead of 2, ~1.2 MiB more GPU memory;
  maybe ~0.1 ms saved if one 2D full-screen quad is the dither pass).
- **Risk:** the cached RGB565 surface is stale while on (check frame dump, bug report screenshot,
  state save/load, render scales above 1x).
- **Verify:** sky gradient zoomed on the PC in four cases (default; `nodither` alone; `smooth=1`;
  mod's No Dithering with `smooth=1`); fps on deck and slot 7; loading screens, fades, the intro
  movie, a layout switch.
- **Effort:** 2-3 days.

## 6. HUD: sharper HUD

| Path | Gives | Real cost | Problems |
|---|---|---|---|
| (a) HUD mask as in the todo (copy at `hud_phase`, compare at present) | Crisp 400x240 HUD pixels | Not 0.5-1 ms: the copy is a blit scene (~0.8 ms) and splits the world scene (~1 ms store/reload), plus the filter | Blend differences give a noisy mask |
| (b) H1 cached 960x544 layer | Real screen-resolution HUD | 3-5 ms on every frame the layer changes - most hunting frames | Minimap DstAlpha masks vs coverage alpha; full-screen quads; additive blends |
| (c) H2 layout hashes | Move or hide parts | Small | No sharpness by itself |
| **(a') mask carried in alpha (recommended)** | Same look as (a) | ~0.3-0.6 ms in play | Needs G2's RGBA8 copy; alpha handling must be proven identical |
| (d) text replay (our removed Sharp HUD text) | Glyphs drawn at 960x544 | Measured within noise after optimisation | Text only; re-test with GPU GS now the default |

Recommendation: build (a') on top of G2's display slots; keep (d) as the follow-up if text is
still too soft; drop H1; H2 only to move HUD parts without the mod. (a') has no scene split and no
copy (the mask rides in the alpha G2's copy carries), treats text, icons, gauges, minimap and name
tags the same, changes nothing in how the game draws, and costs GPU only where the HUD is.
Limit: it adds no detail - HUD pixels become as crisp as on a real 3DS while the 3D stays linear.

**Look check first (no code):** `upscale=1` (sharp bilinear everywhere), frame dumps at the camp,
the village and the item box. The HUD there is what (a') will show; if it is not clearly better
than linear, stop.

1. **Tag:** the grade pass is drawn by us as a blend (`IsGradeDouble`, the `grade_double` branch
   near line 2444 of `gxm_rasterizer.cpp`). With the option on, give it alpha Zero/Zero with
   alpha write on, so the frame's alpha becomes 0 for free; following HUD draws write alpha.
2. **Prove the tag harmless:** tag on, present unchanged: colour output pixel-identical at village
   slot 4 (minimap, DstAlpha draw #66), camp, item box. If the minimap depends on that alpha, tag
   with a separate alpha-only quad after its mask draws, or exclude its rectangle.
3. **Extents:** while `hud_phase` is set, accumulate the 2D draws' bounding rectangles in target
   pixels, merged to at most 8 (draws without a known extent mark the whole screen).
4. **Pair with the frame:** at the display transfer store the rectangles and `has_mask` (grade
   seen this frame) in the `DisplaySlot`; title/menu frames without a grade stay linear.
5. **Carry alpha:** the slot blit must copy alpha (check `GpuBlit`'s fragment program).
6. **Present:** extend `GuestScreen` (`gxm_present.h`) and `Renderer::GetGuestFramebuffer` with
   the rectangles; in `GxmPresent::DrawGuestScreen`, after the normal quad, one quad per rectangle
   with a new fixed program: sharp-bilinear colour, alpha = `saturate(a * k)`, blended
   SrcAlpha/OneMinusSrcAlpha (beside `sharp_fp`).
7. **Rotation:** rectangles are in framebuffer space (240 across, 400 along); map them with the
   full quad's u/v-to-screen mapping (screen x from v, screen y from 1 - u); 16:9 mode scales the
   axes separately, as the sharp filter does.
8. **Coverage cap:** over ~60% of the screen (full-screen menus): one full-screen HUD quad, or
   linear. Decide from measurement.

Switches: `video.txt` `hudsharp=1`, pause menu "Sharp HUD", off by default (turns on the 8-bit
display copy without the dither change, so the 3D carries the game's dither at 8-bit instead of
5:6:5 - say so in the menu text); kill switch `nohudsharp`; diagnostics `dbghudmask`, `hudrects`,
`hudk<N>`.

GPU cost: tag 0; blit unchanged; present: the sharp filter cost +1.9 ms over linear for
960x544, HUD rectangles cover ~15-30% in a hunt -> +0.3-0.6 ms; full-screen menu up to +2 ms. The
camp (no margin) decides whether it can ever be a default.

Risks: minimap DstAlpha masks (step 2 is the gate); HUD draws without alpha write (`hudlog` prints
mask bits); additive/low-alpha effects partly sharpened (acceptable); FXAA runs before the present
and would blur the HUD.

Measure: fps and GPU ms at slot 1, slot 4, slot 7 and with the item box open, on and off, two
passes; frame dumps of one still frame with linear, `upscale=1`, `hudsharp=1`, `dbghudmask`.

Follow-up (d): reapply Sharp HUD text (58c3ea0d7..f834922b7, reverted by 57d1fd733) on top of
the display slots; first check where the A4 font draws go now that the GPU geometry shader is the
default. Effort: (a') 3-4 days after G2; (d) ~2 more.

## 7. Order and gates

| # | Work | Measure before committing |
|---|---|---|
| 1 | Groundwork: hook module, switch helper, counters, guess-race fix | Deck and slot 7 fps and picture unchanged |
| 2 | P2 measurement | `re l/x/t` and the word log; stop unless a second list or top transfer is live |
| 3 | P3 log and `fx` counter | Live filters in the village, a hunt with a roar, a cutscene; draws dropped per second |
| 4 | P1 by cheat line | `bot d`, `gx` ms, emulation CPU on deck and slot 7; go native if measurable and menus clean |
| 5 | P1 native, then default after the test matrix | Section 4 tests |
| 6 | P3 skip options (and the "dropped anyway" group) | Section 3b three-way screenshots; fps during a roar |
| 7 | G2 | Gradient shots in four cases; fps unchanged |
| 8 | HUD (a') | `upscale=1` look check; step 2 identity test; cost at the camp |

Rough total: 2 days for 1-4, 3-4 for 5-6, 2-3 for G2, 3-4 for the HUD.

# Why the 2D layer is expensive on the GPU, and how to draw it at screen resolution cheaply

Analysis 2026-11-19 (Fable), from the code at b39189053 and the console logs (diag `timedraw
scenetime`, slot 7 = quest counter). Savings are estimates until the experiments (section 5) run.

## 1. Verdict

- **The cost is fragment ALU in our generated sprite program, not a pathology.** Every 2D sprite
  runs a ~41-46 instruction program with two dependent texture reads; the plain equivalent is
  6-10 instructions.
- **Why so long:** MH4U's 2D material has two combiner stages, the second reading the procedural
  texture: `colour = 2 * (stage0 + proctex(texcoord2.u) - 0.5)`. Every HUD pixel pays the proctex
  strip lookup plus the general emitter's rounding and register traffic.
- **No kill, no depth write, no scissor test, no lighting, no fog** in these programs.
- **Scene-level overhead is small:** ~2.6 ms for the HUD scene without sprites, 2.5 ms vertex.
- **Best fix:** a compact fragment emitter for unlit sprite-shaped configurations: HUD scene at
  the quest counter ~76 -> ~30 ms at screen resolution, ~8 ms saved at 1x with `hudres` off.

## 2. What the logs and code show

| Slot 7, per frame | Main target | HUD scene 544x960 |
|---|---|---|
| `hudres` off | 43.8 ms (vertex 5.1), 237 binds | - |
| `hudres=1` | 22.4 ms (vertex 3.1), 143 binds | 76.3 ms (vertex 2.5), 88 binds |
| `hudres=1`, GS draws dropped | 22.1 ms | 2.57 ms (vertex 0.63) |

- ~700 software GS draws and ~35 GPU GS draws (224 points) a frame: ~1,000 sprites, ~2,000
  triangles. "88 draws" counts program binds; repeat and texture-only paths merge the rest.
- Every bind had its scissor folded away (`scissor_covers_draw`). One HUD scene per frame.

The program (dumped 2026-11-15, `us_0000003929a6cf48` sprites, `us_0000001b9bdae1b6` font):
stage 0 Modulate(primary, tex0) (font: primary x constant colour, primary.a x tex0.a alpha);
stage 1 AddSigned with Texture3 (proctex), colour scale 2, alpha unchanged; proctex coordinate 2,
clamp, combiner U, linear, width 128 (strip path: ~9 instructions and a second dependent read).
Framebuffer word `0x08e1`: alpha test Always, scissor Disabled, Z-buffering.

| Part | Instructions |
|---|---|
| Phase marker and nop | 2 |
| tex0 sample and wait | 2 |
| Strip coordinate, sample, wait | ~9 |
| `RoundPrimary` (vertex colour to 8 bits, per pixel) | 7 |
| Zeroing the combiner output | 2 |
| Stage 0 | ~6 |
| Stage 1 (AddSigned, clamp, byte round, x2, clamp, store) | ~16 |
| Pack | 1 |

Plus the patcher's blend and half-to-UCHAR4 tail (length unknown).

Also checked: units 6/7 (6 = proctex strip, sampled; 7 fog strip, not), no discard, no
framebuffer fetch, no feedback reads in the sprites; A4/IA4/I8/ETC1/ETC1A4 converted to RGBA8 at
upload (no shader decode; the A4 font atlas is 4 MiB of RGBA8); region clip constant across the
layer; sprite area/overdraw not measured yet (`hud_post_area` sums viewport rectangles).

Quantification: 2D fragment time 19.4 ms at 1x and 71.9 ms at 544x960 -> ~11.8 ms per 96,000
pixels (~123 ns, ~109 GPU-core cycles per screen pixel at 1x) plus ~7.6 ms independent of
resolution (per-primitive/per-tile work for ~2,000 small blended triangles; unconfirmed). At ~50
instructions per sprite pixel that is about 2-5 screens of blended sprite coverage: plausible for
~1,000 sprites over a full-screen menu.

## 3. Scene-level costs

Not the problem: scene base 2.57 ms (copy + 522,000-pixel store), vertex 2.5 ms, one scene per
frame, no partial-render signs, constant region clip and viewport. Open: the ~7.6 ms fixed part.

## 4. Design, ranked

| # | Option | Saving at the counter | Risk | Effort |
|---|---|---|---|---|
| 1 | Compact sprite fragment emitter | ~43-48 ms at screen res; ~8 ms at 1x | Medium | 2-4 days |
| 2 | Hidden-world skip under full-screen menus (menu plan Phase 3) | 22.4 ms GPU + render thread | Medium-high | 1-2 weeks |
| 3 | Blend off for provably opaque sprites (hidden-surface removal) | Unknown until E4/E6 | Medium | 3-5 days |
| 4 | Trim the fixed ~7.6 ms (lean sprite vertex program) | a few ms | Low | 1 day |
| 5 | Native A8 / L8A8 formats | GPU only if E3 shows filtering matters; -3 MiB CDRAM | Low | 1-2 days |

Rejected: panels at 1x magnified into the hi-res target (same per-pixel cost as option 1, plus
scene splits); batching for the GPU (vertex is 2.5 ms); non-dependent reads (no gain measured
2026-11-16, crashes with multi-query programs); avoiding discard (there is none).

### Option 1

`usse/fs_usse_gen.{h,cpp}`: `SpriteShape(const FSConfig&)` and `EmitSpriteFragmentProgram`,
called from `EmitFragmentProgram` after the `dbgminfs` block; fall through on any mismatch.

Recognise (final config after `BindPrograms`): lighting off, fog None, alpha test Always, scissor
Disabled, Z-buffering, no shadow; texture0 plain 2D, no border colour, units 1-2 unread, combiner
buffer unused; at most two active stages; ops Replace/Modulate/Add/AddSigned; sources Primary,
Texture0, Constant, Previous, and Texture3 only when `ProcTexStripEligible`; plain modifiers.

Version 1 (same maths, dead work removed): phase marker, nop, tex0 sample, strip coordinate and
sample, one wait for both; stage 0 from the primary attribute and texel into an internal
register; stage 1 in place (add strip, -0.5, clamp, x2, clamp); pack. Drops `RoundPrimary`, the
output zeroing, stores/reloads between stages and the last-stage byte round: ~18-20 instructions.

Version 2: 2a - a sprite pass-through vertex program writes the strip coordinate per vertex
(when `texcoord2.u >= 0` on the whole run); 2b - if the ramp is constant per draw, a uniform
instead of the read (8-10 instructions).

Key: a bit in the `fs_hash` combine in `BindPrograms`; bump `program_version.cmake`; kill switch
`nospritefs`. Rounding: drop the last-stage round and rely on the output conversion (if it
truncates, add 0.5/255); at most 2/255 worst case. Verify on the host (`tools/usse_host/fs_emit.cpp`
on the dumped `.fq`), on the console (`dumpframe` A/B with `nospritefs`, slots 1, 5, 7, both
`hudres`; max channel difference 2), and `scenetime`. Risks: emitter mistakes (reuse proven
instruction forms, `FinishNosched`), a config the recogniser misses (strict, counted), one more
program per shape and blend.

### Option 3

Blending off for SrcAlpha/OneMinusSrcAlpha sprites with vertex alpha 1 and a proven opaque texture
rectangle (menu plan 6.3, keyed on `upload_hash`): the tile renderer then shades nothing under
them. Decide after E4/E6.

## 5. Experiments (diag `timedraw scenetime`, slot 7, the `544x960` entry)

| # | Flags | Answers |
|---|---|---|
| E0 | `usse_dump logtemps hudlog` (`hudres=1`) | today's 2D programs |
| E0b | new `hudptlog`: per draw min/max `texcoord2.u` and the strip over it | version 2b / 2a |
| E1 | `dbgminfs dbgminsel_unlit`, then `+minkeep_tex`, then `+minkeep_tex minkeep_tev` | floor; ~compact program; the strip's price |
| E2 | `hudres=2` | third point for fixed/proportional |
| E3 | `dbghudpoint`; separately `nohwgs` | filtering cost; GPU GS share |
| E4 | new `hudarea`: real triangle areas per draw | overdraw, big panels |
| E5 | new `hudresnoblend` | the blend tail |
| E6 | new `hudresbig<N>` | panels vs glyphs |

Decision: build option 1 if E1 `minkeep_tex` brings the HUD scene under ~35 ms; if the bare run
stays above ~25 ms, the fixed part or overdraw dominates - run E4-E6 and move option 3 up.

## 6. Expected (estimates)

| | Today | After 1 | After 1 + 2 |
|---|---|---|---|
| Counter, `hudres=1`, GPU per frame | 22 + 76 | 22 + ~30 | ~30 |
| Counter, `hudres` off | 44 | ~35 | ~13 |
| Camp, `hudres=1`, 2D layer | ~14 ms | ~4-5 ms | same |

The counter is CPU-bound at 23-26 fps with `hudres` off, so option 1 alone takes `hudres=1`
there from ~10 to ~18-20 fps; 30 needs option 2. The camp should return to ~28-30.

## 7. Second analysis (Fable, 2026-11-19, read-only)

- The `dbgmin*` variants change only the emitted program (`EmitFragmentProgram` edits a copy of
  the configuration); the rasterizer's proctex strip bake, unit-6 bind, repeat keys and merges
  run from `regs` in every run. Per-draw state and blocked merges are ruled out; unit 6 is not
  re-set per draw (`BindFragmentTexture` compares descriptors).
- The strip read and the texcoord 2 input were each removed alone (both 76 ms), never together.
  Cost independent of instruction count with a step between program shapes fits a threshold,
  most likely occupancy (temporaries + primary attributes per pixel limiting how many pixels are
  in flight to hide texture latency).
- `hudresnoblend` (18 ms with the full program, below the blended bare program) is hidden-surface
  removal, a second lever (opaque sprites unblended) for the 15-19 ms floor.

Experiments (slot 7, `hudres=1`, `timedraw scenetime nodiskprograms`):

| # | Flags | Reading |
|---|---|---|
| R1 | `dbgminfs dbgminsel_unlit minkeep_tex minkeep_tev tempinflate logtemps` | 40 ms program + 24 temporaries: toward 60-76 = occupancy |
| R2 | `tempinflate logtemps` | full program, more temporaries: well above 76 = occupancy-sensitive |
| R3 | `dbgminfs dbgminsel_unlit minkeep_tex minkeep_tev minkeep_pt05` | 0.5 instead of 0 for Texture3: near-correct picture; 40 ms = the black output was not why |
| R4 | `ptarith ptcoord0` | one read, no texcoord 2: ~40 ms confirms the either-condition threshold |
| R5 | `dbgminfs dbgminsel_unlit minconst hudresnoblend` | floor without blending: sizes the opaque-sprite lever |

Fix if confirmed: a sprite program below the threshold - one sampler, strip arithmetic (guarded by
the bake's line check), the strip coordinate in a spare lane of texcoord 0, stage values in
internal registers. Estimate: HUD scene 76 -> ~40 ms at `hudres=1`, ~7 ms saved at 1x.

# Blend cost in the 2D layer: what the evidence says, and what to try

Research 2026-11-19 (Fable), read-only; nothing here run on the console, savings are estimates.

## 1. Verdict

- **`hudresnoblend` does not isolate the blend code.** Turning blending off also makes every
  sprite opaque, so the tile renderer shades only the top layer of ~5 stacked layers - which
  alone predicts the measured 76 -> 18 ms.
- **Our own numbers bound the patcher's blend tail at ~19 ms, not ~58 ms:** `dbgminfs` (bare
  program, blending kept, same half4 pack) ran the same 2.6 M blended pixels in ~22 ms.
- **What the patcher appends cannot be read from anything on this machine** (no Vita3K source, no
  SDK documents, no libgxm disassembly); it can be measured and dumped on the console.
- **8-bit program output** is possible in principle but an unproven instruction form, with a
  ceiling of ~19 ms: not before the experiments below.
- **The likely lever is translucent overdraw:** drawing opaque sprites without blending, or not
  drawing what a cover hides.

## 2. Evidence

| Run (slot 7, hudres=1, HUD scene, ~2.6 M sprite px, overdraw ~5) | Measured | "Blend tail" model | "Hidden-surface removal" model |
|---|---|---|---|
| Full program, blended | 76 ms | reference | 2.6 M px x ~28 ns |
| Full program, `hudresnoblend` | 18 ms | tail = 58 ms | 2.6 + 73.4 / 5.0 = 17.3 ms |
| Bare program (`dbgminfs`), blended | 22 ms | tail <= 19 ms (contradiction) | 2.6 M px x ~7.5 ns |

The removal model fits all three with one assumption (a shaded full-sprite pixel costs ~28 ns,
blended or not); `dbgminfs` keeps the blend (only the FSConfig changes) and ends in the same
`PackF16`. Unverified guess: GXM treats `BLEND_FUNC_NONE` draws without a depth test as opaque for
deferred shading. Other facts: the upscale copy (522,000 opaque px) costs ~0.3 ms (~0.6 ns/px);
something in the sprite programs other than instruction count costs ~54 ms (unexplained).

Output path: our programs declare output type F16 x4 and pack f32 -> f16 into pa0
(`gxp_writer.cpp` 222-225, `fs_usse_gen.cpp`), as psp2cgc does for `float4 : COLOR`; pipelines
use `OUTPUT_REGISTER_FORMAT_UCHAR4`; colour surfaces `OUTPUT_REGISTER_SIZE_32BIT`. Unverified from
memory: the patcher converts pa0 to the register format and blends against the target value; the
SDK compiler has a native-colour output and a FRAGCOLOR input; a 64-bit register allows HALF4 on
an 8-bit surface.

## 3. Approaches

| # | Approach | Expected at the counter (76 ms) | Risk | Depends on |
|---|---|---|---|---|
| 1 | Opaque sprites unblended; hide what a full-screen cover hides | up to ~58 ms if A confirms removal | medium (exactness) | A, D |
| 2 | HALF4 register format on the HUD target | 0 to a few ms | low-medium | B, E |
| 3 | 8-bit program output (u8 pack, header type 7) | 0 to a few ms; ceiling ~19 ms | high (new instruction form) | B, C, a compiled reference |
| 4 | In-program blend | none expected | high | a compiled reference |

Rank 1 deserves a second look (vertex alphas "never read 1" needs explaining; opaque texels need a
per-upload scan): routes are the layout-driven cover skip, the mod data fix, and an opaque-rectangle
scan keyed on `upload_hash`.

## 4. Experiments (slot 7, hudres=1, diag `timedraw scenetime nodiskprograms`, HUD scene line)

| # | Switch | Change | Reads as |
|---|---|---|---|
| A | `hudresblendquad` | after the upscale copy, the same quad 4 more times blended SrcAlpha/OneMinusSrcAlpha, with `hudresnogs` (picture unchanged: c*a + c*(1-a) = c; ~2.1 M blended px) | ~+55 ms: the tail is the cost; a few ms: it is not - go to rank 1 |
| B | `patchsize` | log `sceGxmShaderPatcherGetFragmentUsseMemAllocated` growth per program, blend on/off, format | tail length in instructions |
| C | `patchdump` | dump the patcher's USSE block for one sprite program | the actual tail code |
| D | `hudrescoveropaque` | sprites covering >= 90% of the target drawn with blend NONE | what one opaque cover removes |
| E | `hudreshalf` | HUD target 64-bit register, HUD pipelines HALF4 | tail without conversions |
| F | `hudresu8` | only after C and a psp2cgc reference: 8-bit output | rank 3's gain |

Risks: A-D use existing programs and API state only; E changes surface and pipeline formats
together; F and rank 4 emit forms no Sony program in our set uses (past GPU crashes came from such);
rank 1 must stay exact (2D draws overwrite destination alpha, which the minimap reads).

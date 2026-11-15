<!-- Image quality options: plan (Fable, 2026-11-15). Estimates unless measured. -->

# Image quality options: upscale filter, FXAA, internal scale, MSAA

Owner request (2026-11-15): toggleable options to try on the console: an upscale filter at the present, FXAA, 4x MSAA, and internal render scales 1.25x/1.5x/1.75x/2x. MH4U renders its top screen at 400x240; the present stretches it to 960x544 with a linear filter. Defaults stay as today (1x, linear, no AA), since the GPU is the limit (area 2 ~31.5 ms of 33.3).

## Findings
- Present shaders are prebuilt GXP (`src/citra_vita/shaders/*.cg`, compiled offline with psp2cgc, which is not installed here). New programs are written with the project's emitters, like `EmitBlitFragmentProgram` (`usse/fixed_usse_gen.cpp`).
- Settings are not persisted; `citra_vita_main.cpp` forces Single screen each boot.
- The GXM backend is hard-wired to 1x (`Framebuffer::Scale()` = 1, `Surface::ScaleUp` empty) and `SCE_GXM_MULTISAMPLE_NONE` everywhere.

## Plan, in order
0. **Settings plumbing**: `VideoOptions` (upscale filter, FXAA, scale in quarters, MSAA), saved to `ux0:data/mh4u/settings.txt`; pause-menu entries, with an "applies on next launch" marker for scale and MSAA.
1. **A, sharp bilinear** at the present (`DrawGuestScreen`): one fragment program with a texture-size uniform. ~+0.5-1 ms.
2. **B, FXAA**: a pass on the top screen's 400x240 image into its own small target before the present. ~+1.5-2.5 ms.
3. **D, internal scale**: backend-private quarter-step scale for Color/Depth surfaces (generic cache stays at res_scale 1). Scaled allocation, viewport/scissor/clip, blits by each surface's own scale (mixed scale through the linear GPU blit), downsample on guest download, upsample on upload. Restart-applied first. Extra memory ~+1.0/2.2/3.7/5.4 MB at 1.25/1.5/1.75/2x (CDRAM has ~3 MB free).
4. **C, 4x MSAA**: multisampled render targets with MSAA-downscale colour surfaces; depth at 2x2 samples (reuses D's machinery); a 4X variant of every fragment program (pipeline key). No alpha-to-coverage in libgxm, so cut-out grass stays aliased.
5. **A, edge-aware upscale**: an edge weight from B's pass, plus one extra tap at the present. ~+3-4 ms.

## Expected GPU in area 2 (estimates)
| Option | Frame | Shown fps |
|---|---|---|
| Sharp bilinear | +0.5-1 ms | ~30 |
| FXAA | +1.5-2.5 ms | 28-29 |
| Edge-aware | +3-4 ms | ~27 |
| MSAA 4x | 36-42 ms | 24-27 |
| 1.25x | ~45 ms | ~21 |
| 1.5x | ~63 ms | ~15 |
| 1.75x | ~85 ms | ~11 |
| 2x | ~110 ms | ~9 |

## Tests
Release, slots 1 and 2: screenshots at fixed views with each option off (identical to base) and on; the camera script for fps; diag `timedraw scenetime` and the `pools` line. For scale and MSAA also: quest counter, an area transition, a state load, two-screen layout.

## Status (2026-11-15)
- Step 0 + A (sharp bilinear): done, commit 9e21cdd43. Slot 1: crisper, 30 fps unchanged.
- B (FXAA): done. Slot 1: edges visibly smoother, slight overall softening (HUD included); shown 29-30 (a few skips).
- D (render scale): first cut committed (menu "Render scale", next launch; video.txt scale_q=4..8). Verified identical at 1x; 1.25x-2x not yet run on the console. Not covered yet: diag surface dumps, the feedback/alias checks that compare surface sizes.
- C (MSAA): not started.
- Slot 2's save state crashes the game right after loading (guest prefetch abort at PC 0, fault.txt); slot 2 was re-saved 2026-11-15 11:19. Slot 1 loads fine with the same build.

## TuizaVita comparison (Fable, 2026-11-15)
The other Vita port (github.com/TutuiFC/TuizaVita-3ds-emu, cloned to ~/Documents/Personal) is a general-compatibility port with a block JIT and runtime Cg shaders; it is behind ours in CPU, shaders, frame pacing and I/O. Taken: clamp the time-stretch output before the s16 cast. Candidates left: constant blend factors carried in source alpha (`MapCarrierFactor`; ours approximates them; check `logblend` whether MH4U uses them), preloading the program disk cache into RAM to soften area-entry hitches (measure `Find` during a transition first).

## TuizaVita 0.3B follow-up (2026-11-15)
Source: tag v0.3B (branch vita-0.1.7) of github.com/TutuiFC/TuizaVita-3ds-emu; the earlier review only saw `main` (0.1B).
- Mipmaps: done (commit ac796f726, kill switch `nomipmaps`). Slot 1: distant textures smoother, GPU about the same.
- 0.75x render scale: added to the menu; untested on the console, like 1.25x-2x.
- RomFS/LayeredFS reads on a worker thread: next; first time the reads during an area crossing (the 1-2 s dip).
- Condition-variable wait wrap for the remaining polling waits: small, later.
- Z-order (swizzled) textures: TuizaVita pairs it with mipmaps; not taken yet (every copy/blit/download path would need an unswizzle).

## Render scale measured (2026-11-15, release, slot 1, area 2 spot)
| Scale | Shown fps | Notes |
|---|---|---|
| 0.75x | 30 | softer; GPU headroom |
| 1x | 30 | identical to before the scale code |
| 1.25x | 22 | |
| 1.5x | 17-18 | |
| 2x | 11-12 | the game itself slows too (speed 50-77%, GX 600-860 ms/s waiting on the renderer); sharp |
Pictures correct at every scale (HUD, world, sky). Mipmaps A/B, diag scenetime: 29.4-31.5 ms without, 27.2-31.2 with (within noise).
